/*
 * FireWireLivesOHCI.cpp - PCIDriverKit driver for 1394 OHCI controllers.
 *
 * Bring-up order is dictated by OHCI 1.1 and is not negotiable. Getting it
 * wrong produces symptoms that look like broken hardware:
 *
 *   1. Enable PCI bus mastering, or DMA silently never happens.
 *   2. Soft reset the controller and wait for the bit to clear.
 *   3. Set LPS (link power status) and let the PHY settle. Touching PHY or
 *      link registers before LPS is stable returns garbage.
 *   4. Program the self-ID buffer address and enable self-ID reception.
 *   5. Arm interrupts, then set linkEnable.
 */

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSBoolean.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "FireWireLivesOHCI.h"

#include "fw/ohci_regs.h"
#include "fw/fw_selfid.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "FireWireLives: " fmt, ##__VA_ARGS__)

/* BAR0 holds the OHCI register file. */
#define OHCI_BAR_INDEX 0

/*
 * The self-ID buffer must be 2 KB and 2 KB aligned per OHCI. The DART on this
 * platform maps at 16 KB granularity, so a whole mapper page is the practical
 * minimum allocation regardless.
 */
#define SELFID_BUFFER_SIZE  2048
#define SELFID_BUFFER_ALIGN 2048

/* PCI command register bits, for enabling bus mastering. */
#define PCI_COMMAND_REGISTER      0x04
#define PCI_COMMAND_MEMORY_SPACE  (1 << 1)
#define PCI_COMMAND_BUS_MASTER    (1 << 2)

/* A failed MMIO read comes back as all-ones. */
#define OHCI_BAD_READ 0xffffffffu

struct FireWireLivesOHCI_IVars
{
    IOPCIDevice               *pciDevice;
    IOMemoryMap               *barMap;
    uint64_t                   barLength;

    IODispatchQueue           *workQueue;
    IOInterruptDispatchSource *interruptSource;
    OSAction                  *interruptAction;

    /* Self-ID DMA buffer: the controller writes post-reset topology here. */
    IOBufferMemoryDescriptor  *selfIDBuffer;
    IODMACommand              *selfIDDMACommand;
    uint64_t                   selfIDDeviceAddress;  /* IOVA, not physical */
    void                      *selfIDVirtualAddress;

    fw_topology_t              topology;
    uint32_t                   ohciVersion;
    uint64_t                   controllerGUID;
    uint32_t                   generation;
    uint16_t                   localNodeID;
    bool                       isRoot;
};

bool
FireWireLivesOHCI::init(void)
{
    if (!super::init())
        return false;

    ivars = IONewZero(FireWireLivesOHCI_IVars, 1);
    return ivars != nullptr;
}

void
FireWireLivesOHCI::free(void)
{
    if (ivars)
        IOSafeDeleteNULL(ivars, FireWireLivesOHCI_IVars, 1);
    super::free();
}

/* ------------------------------------------------------------------ */
/* MMIO accessors                                                      */
/* ------------------------------------------------------------------ */

/*
 * PCIDriverKit's MemoryRead32 returns void and writes 0xffffffff on failure,
 * so these mirror that rather than inventing a status code. Callers check for
 * OHCI_BAD_READ where it matters.
 */
uint32_t
FireWireLivesOHCI::RegisterRead(uint32_t offset)
{
    uint32_t value = OHCI_BAD_READ;

    if (offset + sizeof(uint32_t) > ivars->barLength)
        return OHCI_BAD_READ;

    ivars->pciDevice->MemoryRead32(OHCI_BAR_INDEX, offset, &value);
    return value;
}

void
FireWireLivesOHCI::RegisterWrite(uint32_t offset, uint32_t value)
{
    if (offset + sizeof(uint32_t) > ivars->barLength)
        return;

    ivars->pciDevice->MemoryWrite32(OHCI_BAR_INDEX, offset, value);
}

/* ------------------------------------------------------------------ */
/* Controller bring-up                                                 */
/* ------------------------------------------------------------------ */

kern_return_t
FireWireLivesOHCI::EnableBusMastering(void)
{
    uint16_t command = 0;

    ivars->pciDevice->ConfigurationRead16(PCI_COMMAND_REGISTER, &command);
    command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
    ivars->pciDevice->ConfigurationWrite16(PCI_COMMAND_REGISTER, command);

    /* Read back: without bus mastering every DMA silently does nothing, and
     * that failure mode is indistinguishable from dead hardware. */
    uint16_t check = 0;
    ivars->pciDevice->ConfigurationRead16(PCI_COMMAND_REGISTER, &check);
    if ((check & PCI_COMMAND_BUS_MASTER) == 0) {
        Log("ERROR: bus master enable did not stick (command=0x%04x)", check);
        return kIOReturnNotWritable;
    }

    Log("PCI command register: 0x%04x (bus mastering on)", check);
    return kIOReturnSuccess;
}

kern_return_t
FireWireLivesOHCI::ResetController(void)
{
    RegisterWrite(OHCI_HCControlSet, OHCI_HCControl_softReset);

    /* softReset self-clears when the reset completes. The spec allows up to
     * 500 ms; poll so a wedged controller is reported rather than silently
     * half-initialised. */
    for (int i = 0; i < 500; i++) {
        uint32_t hcControl = RegisterRead(OHCI_HCControlSet);

        if (hcControl == OHCI_BAD_READ) {
            Log("ERROR: register file unreadable during reset");
            return kIOReturnNoDevice;
        }
        if ((hcControl & OHCI_HCControl_softReset) == 0) {
            Log("soft reset completed after %dms", i);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }

    Log("ERROR: soft reset never cleared - controller is wedged");
    return kIOReturnTimeout;
}

kern_return_t
FireWireLivesOHCI::EnableLinkPowerStatus(void)
{
    /* LPS powers the PHY and starts PHY/link communication. Until it settles,
     * PHY register reads and NodeID return garbage - the most common 1394
     * bring-up bug. */
    RegisterWrite(OHCI_HCControlSet, OHCI_HCControl_LPS);
    IOSleep(50);

    for (int i = 0; i < 100; i++) {
        uint32_t hcControl = RegisterRead(OHCI_HCControlSet);

        if (hcControl != OHCI_BAD_READ && (hcControl & OHCI_HCControl_LPS)) {
            uint32_t version = RegisterRead(OHCI_Version);

            if (version == OHCI_BAD_READ || version == 0) {
                Log("ERROR: BAR reads back 0x%08x - MMIO is not live", version);
                return kIOReturnNotReady;
            }

            ivars->ohciVersion = version;

            /* The controller's own EUI-64, readable once LPS is up. Proves we
             * are talking to the FireWire chip and not reading back noise. */
            uint32_t guidHi = RegisterRead(OHCI_GUIDHi);
            uint32_t guidLo = RegisterRead(OHCI_GUIDLo);
            ivars->controllerGUID = ((uint64_t)guidHi << 32) | guidLo;

            Log("LPS stable; OHCI version %u.%u, GUID %08x%08x",
                (version >> 16) & 0xff, (version >> 8) & 0xff, guidHi, guidLo);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }

    Log("ERROR: LPS never came up - check cable power and the PHY");
    return kIOReturnTimeout;
}

kern_return_t
FireWireLivesOHCI::ConfigureSelfIDBuffer(void)
{
    kern_return_t ret;

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           SELFID_BUFFER_SIZE,
                                           SELFID_BUFFER_ALIGN,
                                           &ivars->selfIDBuffer);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: self-ID buffer allocation failed: 0x%x", ret);
        return ret;
    }

    /*
     * OHCI's SelfIDBuffer register is 32 bits wide, so the address the
     * controller is given must fit in 32 bits. On Apple Silicon the device
     * reaches memory through the DART IOMMU, so that address is an IOVA
     * produced here - handing the controller a CPU physical address would
     * fault the DART.
     */
    IODMACommandSpecification spec = {};
    spec.options        = kIODMACommandSpecificationNoOptions;
    spec.maxAddressBits = 32;

    ret = IODMACommand::Create(ivars->pciDevice,
                               kIODMACommandCreateNoOptions,
                               &spec,
                               &ivars->selfIDDMACommand);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: IODMACommand::Create failed: 0x%x", ret);
        return ret;
    }

    uint64_t         dmaFlags = 0;
    uint32_t         segmentCount = 1;
    IOAddressSegment segments[32] = {};

    ret = ivars->selfIDDMACommand->PrepareForDMA(
              kIODMACommandPrepareForDMANoOptions,
              ivars->selfIDBuffer,
              0, SELFID_BUFFER_SIZE,
              &dmaFlags, &segmentCount, segments);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: PrepareForDMA failed: 0x%x", ret);
        return ret;
    }

    /* A 2 KB buffer must map as one contiguous segment; anything else means
     * the controller would read past the end of the first chunk. */
    if (segmentCount != 1) {
        Log("ERROR: self-ID buffer scattered across %u segments", segmentCount);
        return kIOReturnNoResources;
    }

    ivars->selfIDDeviceAddress = segments[0].address;

    if (ivars->selfIDDeviceAddress > 0xffffffffull) {
        Log("ERROR: DMA address 0x%llx exceeds OHCI's 32-bit register",
            ivars->selfIDDeviceAddress);
        return kIOReturnNoResources;
    }

    IOAddressSegment virtualSegment = {};
    ivars->selfIDBuffer->GetAddressRange(&virtualSegment);
    ivars->selfIDVirtualAddress = (void *)virtualSegment.address;

    Log("self-ID buffer: IOVA 0x%llx, %u byte(s), CPU 0x%llx",
        ivars->selfIDDeviceAddress, (unsigned)SELFID_BUFFER_SIZE,
        virtualSegment.address);

    RegisterWrite(OHCI_SelfIDBuffer, (uint32_t)ivars->selfIDDeviceAddress);

    /* Tell the link to accept self-ID and PHY packets. */
    RegisterWrite(OHCI_LinkControlSet,
                  OHCI_LinkControl_rcvSelfID | OHCI_LinkControl_rcvPhyPkt);

    return kIOReturnSuccess;
}

kern_return_t
FireWireLivesOHCI::SetupInterrupts(void)
{
    kern_return_t ret;

    ret = IODispatchQueue::Create("FireWireLivesQueue", 0, 0,
                                  &ivars->workQueue);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: dispatch queue creation failed: 0x%x", ret);
        return ret;
    }

    ret = IOInterruptDispatchSource::Create(ivars->pciDevice, 0,
                                            ivars->workQueue,
                                            &ivars->interruptSource);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: interrupt source creation failed: 0x%x", ret);
        return ret;
    }

    ret = CreateActionInterruptOccurred(0, &ivars->interruptAction);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: interrupt action creation failed: 0x%x", ret);
        return ret;
    }

    ret = ivars->interruptSource->SetHandler(ivars->interruptAction);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: SetHandler failed: 0x%x", ret);
        return ret;
    }

    ret = ivars->interruptSource->SetEnable(true);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: enabling the interrupt source failed: 0x%x", ret);
        return ret;
    }

    Log("interrupt source armed");
    return kIOReturnSuccess;
}

kern_return_t
FireWireLivesOHCI::EnableLink(void)
{
    /* Unmask only what we handle; leaving the rest masked stops a misbehaving
     * controller from livelocking the interrupt handler. */
    RegisterWrite(OHCI_IntMaskSet,
                  OHCI_Int_busReset |
                  OHCI_Int_selfIDComplete |
                  OHCI_Int_reqTxComplete |
                  OHCI_Int_respTxComplete |
                  OHCI_Int_RQPkt |
                  OHCI_Int_RSPkt |
                  OHCI_Int_postedWriteErr |
                  OHCI_Int_unrecoverableError |
                  OHCI_Int_cycleTooLong |
                  OHCI_Int_regAccessFail);

    RegisterWrite(OHCI_HCControlSet, OHCI_HCControl_linkEnable);
    RegisterWrite(OHCI_IntMaskSet, OHCI_Int_masterIntEnable);

    Log("link enabled");
    return kIOReturnSuccess;
}

void
FireWireLivesOHCI::PublishStatus(void)
{
    OSDictionary *props = OSDictionary::withCapacity(16);
    if (props == nullptr)
        return;

    /* Small helper: set a numeric property and drop our reference. */
    #define PUT_NUM(key, value, bits)                                    \
        do {                                                             \
            OSNumber *_n = OSNumber::withNumber((uint64_t)(value), bits); \
            if (_n) { props->setObject(key, _n); _n->release(); }         \
        } while (0)

    PUT_NUM("OHCIVersion",       ivars->ohciVersion, 32);
    PUT_NUM("OHCIVersionMajor",  (ivars->ohciVersion >> 16) & 0xff, 32);
    PUT_NUM("OHCIVersionMinor",  (ivars->ohciVersion >> 8) & 0xff, 32);
    PUT_NUM("ControllerGUID",    ivars->controllerGUID, 64);
    PUT_NUM("BARLength",         ivars->barLength, 64);
    PUT_NUM("SelfIDBufferIOVA",  ivars->selfIDDeviceAddress, 64);
    PUT_NUM("BusGeneration",     ivars->generation, 32);
    PUT_NUM("LocalNodeID",       ivars->localNodeID, 32);
    PUT_NUM("NodeCount",         ivars->topology.node_count, 32);
    PUT_NUM("RootNodeID",        ivars->topology.root_id, 32);
    PUT_NUM("GapCount",          ivars->topology.gap_count, 32);

    props->setObject("IsRoot", ivars->isRoot ? kOSBooleanTrue : kOSBooleanFalse);
    props->setObject("GapCountConsistent",
                     ivars->topology.gap_count_consistent ? kOSBooleanTrue
                                                          : kOSBooleanFalse);
    props->setObject("DMAMappedThroughDART",
                     ivars->selfIDDeviceAddress ? kOSBooleanTrue
                                                : kOSBooleanFalse);
    #undef PUT_NUM

    SetProperties(props);
    props->release();
}

/* ------------------------------------------------------------------ */
/* PHY register access                                                 */
/* ------------------------------------------------------------------ */

kern_return_t
FireWireLivesOHCI::PhyRead(uint8_t address, uint8_t *value)
{
    RegisterWrite(OHCI_PhyControl, OHCI_PhyControl_Read(address));

    for (int i = 0; i < 100; i++) {
        uint32_t phyControl = RegisterRead(OHCI_PhyControl);

        if (phyControl != OHCI_BAD_READ &&
            (phyControl & OHCI_PhyControl_ReadDone)) {
            *value = (uint8_t)((phyControl >> OHCI_PhyControl_ReadDataShift) & 0xff);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }

    return kIOReturnTimeout;
}

kern_return_t
FireWireLivesOHCI::PhyWrite(uint8_t address, uint8_t value)
{
    RegisterWrite(OHCI_PhyControl, OHCI_PhyControl_Write(address, value));
    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/* Interrupt handling                                                  */
/* ------------------------------------------------------------------ */

void
IMPL(FireWireLivesOHCI, InterruptOccurred)
{
    /* The IMPL macro supplies these; we service the controller by reading its
     * event register rather than by trusting the delivered count. */
    (void)action;
    (void)count;
    (void)time;

    uint32_t events = RegisterRead(OHCI_IntEventClear);
    uint32_t mask   = RegisterRead(OHCI_IntMaskClear);

    if (events == OHCI_BAD_READ || mask == OHCI_BAD_READ)
        return;

    /* The event register reports everything regardless of the mask. */
    events &= mask;
    if (events == 0)
        return;

    /* Acknowledge up front so an event arriving mid-handler is not lost. */
    RegisterWrite(OHCI_IntEventClear, events);

    if (events & OHCI_Int_unrecoverableError) {
        Log("FATAL: controller reported an unrecoverable error; masking");
        RegisterWrite(OHCI_IntMaskClear, OHCI_Int_masterIntEnable);
        return;
    }

    if (events & OHCI_Int_busReset)
        HandleBusReset();

    if (events & OHCI_Int_selfIDComplete)
        HandleSelfIDComplete();
}

void
FireWireLivesOHCI::HandleBusReset(void)
{
    /*
     * A bus reset invalidates every outstanding transaction and every node ID.
     * The generation counter is what makes that safe: a request issued under a
     * stale generation must be failed rather than retried, or it could land on
     * a different device that has since taken the node ID.
     */
    ivars->generation++;
    Log("bus reset (generation %u)", ivars->generation);
}

void
FireWireLivesOHCI::HandleSelfIDComplete(void)
{
    uint32_t selfIDCount = RegisterRead(OHCI_SelfIDCount);

    if (selfIDCount == OHCI_BAD_READ)
        return;

    if (selfIDCount & OHCI_SelfIDCount_selfIDError) {
        Log("self-ID error reported; waiting for the next reset");
        return;
    }

    /* Size is a byte count including the header quadlet the controller writes
     * at the start of the buffer. */
    uint32_t sizeBytes = (selfIDCount >> OHCI_SelfIDCount_sizeShift) &
                         OHCI_SelfIDCount_sizeMask;
    sizeBytes *= 4;

    if (sizeBytes < 8 || sizeBytes > SELFID_BUFFER_SIZE) {
        Log("implausible self-ID size %u bytes", sizeBytes);
        return;
    }

    const uint32_t *buffer = (const uint32_t *)ivars->selfIDVirtualAddress;
    if (buffer == nullptr)
        return;

    /* Skip the header quadlet; the rest is quadlet/inverse pairs. */
    size_t quadlets = (sizeBytes / sizeof(uint32_t)) - 1;

    fw_err_t err = fw_selfid_parse(&buffer[1], quadlets, true, &ivars->topology);
    if (err != FW_OK) {
        Log("self-ID parse failed: %d", err);
        return;
    }

    ivars->topology.generation = ivars->generation;

    uint32_t nodeID = RegisterRead(OHCI_NodeID);
    if (nodeID != OHCI_BAD_READ && (nodeID & OHCI_NodeID_idValid)) {
        ivars->localNodeID = (uint16_t)(nodeID & 0xffff);
        ivars->isRoot = (nodeID & OHCI_NodeID_root) != 0;
    }

    Log("bus enumerated: %u node(s), root is phy %u, we are phy %u%s",
        ivars->topology.node_count,
        ivars->topology.root_id,
        ivars->localNodeID & OHCI_NodeID_nodeNumberMask,
        ivars->isRoot ? " (root)" : "");

    for (uint8_t i = 0; i < ivars->topology.node_count; i++) {
        const fw_node_t *n = &ivars->topology.node[i];
        Log("  node %u: %s, %u port(s), %u hop(s) from root%s",
            n->phy_id, fw_speed_name(n->speed), n->port_count, n->hop_count,
            n->link_active ? ", link active" : "");
    }

    if (!ivars->topology.gap_count_consistent)
        Log("gap counts disagree; a PHY config packet is needed");

    PublishStatus();
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

kern_return_t
IMPL(FireWireLivesOHCI, Start)
{
    kern_return_t ret;

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        Log("super::Start failed: 0x%x", ret);
        return ret;
    }

    ivars->pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (ivars->pciDevice == nullptr) {
        Log("ERROR: provider is not an IOPCIDevice");
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }
    ivars->pciDevice->retain();

    ret = ivars->pciDevice->Open(this, 0);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: could not open the PCI device: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }

    /* Resolve BAR0's length before any register access, so the accessors can
     * bounds-check against something real. */
    uint64_t barSize = 0;
    ret = ivars->pciDevice->GetBARInfo(OHCI_BAR_INDEX, nullptr, &barSize, nullptr);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: GetBARInfo failed: 0x%x", ret);
        Stop(provider, SUPERDISPATCH);
        return ret;
    }
    ivars->barLength = barSize;
    Log("BAR%d length 0x%llx", OHCI_BAR_INDEX, ivars->barLength);

    /* The OHCI register file runs to 0x800 with all 32 iso contexts present. */
    if (ivars->barLength < 0x800) {
        Log("ERROR: BAR is 0x%llx bytes, too small for an OHCI register file",
            ivars->barLength);
        Stop(provider, SUPERDISPATCH);
        return kIOReturnNoDevice;
    }

    if ((ret = EnableBusMastering())    != kIOReturnSuccess) goto fail;
    if ((ret = ResetController())       != kIOReturnSuccess) goto fail;
    if ((ret = EnableLinkPowerStatus()) != kIOReturnSuccess) goto fail;
    if ((ret = ConfigureSelfIDBuffer()) != kIOReturnSuccess) goto fail;
    if ((ret = SetupInterrupts())       != kIOReturnSuccess) goto fail;
    if ((ret = EnableLink())            != kIOReturnSuccess) goto fail;

    PublishStatus();
    RegisterService();
    Log("started");
    return kIOReturnSuccess;

fail:
    Stop(provider, SUPERDISPATCH);
    return ret;
}

kern_return_t
IMPL(FireWireLivesOHCI, Stop)
{
    Log("stopping");

    if (ivars->pciDevice) {
        /* Quiesce the link before letting go. */
        RegisterWrite(OHCI_IntMaskClear, 0xffffffff);
        RegisterWrite(OHCI_HCControlClear, OHCI_HCControl_linkEnable);
    }

    if (ivars->interruptSource)
        ivars->interruptSource->SetEnable(false);

    if (ivars->selfIDDMACommand)
        ivars->selfIDDMACommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);

    OSSafeReleaseNULL(ivars->interruptAction);
    OSSafeReleaseNULL(ivars->interruptSource);
    OSSafeReleaseNULL(ivars->workQueue);
    OSSafeReleaseNULL(ivars->selfIDDMACommand);
    OSSafeReleaseNULL(ivars->selfIDBuffer);
    OSSafeReleaseNULL(ivars->barMap);

    if (ivars->pciDevice) {
        ivars->pciDevice->Close(this, 0);
        OSSafeReleaseNULL(ivars->pciDevice);
    }

    return Stop(provider, SUPERDISPATCH);
}
