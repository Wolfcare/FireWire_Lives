/*
 * FireWireLivesOHCI.cpp - PCIDriverKit driver for 1394 OHCI controllers.
 *
 * NOT YET COMPILED - see the note in FireWireLivesOHCI.iig. This is the
 * bring-up sequence written against the OHCI 1.1 specification and the
 * PCIDriverKit headers as documented; every call marked VERIFY needs checking
 * against the real SDK once Xcode is installed.
 *
 * Bring-up order matters and is not negotiable (OHCI 1.1 section 5.7 plus the
 * initialisation notes in section 3):
 *
 *   1. Enable PCI bus mastering, or DMA silently never happens.
 *   2. Soft reset the controller and wait for the bit to clear.
 *   3. Set LPS (link power status) and wait ~50ms for the PHY to come up.
 *      Touching PHY or link registers before LPS settles returns garbage.
 *   4. Program the self-ID buffer address and enable self-ID reception.
 *   5. Set linkEnable, then issue a bus reset to enumerate.
 */

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <PCIDriverKit/PCIDriverKit.h>

#include "FireWireLivesOHCI.h"

#include "fw/ohci_regs.h"
#include "fw/fw_selfid.h"

#define Log(fmt, ...) os_log(OS_LOG_DEFAULT, "FireWireLives: " fmt, ##__VA_ARGS__)

/* BAR0 holds the OHCI register file. */
#define OHCI_BAR_INDEX 0

/* The self-ID buffer must be 2KB, 2KB-aligned, and physically contiguous. */
#define SELFID_BUFFER_SIZE  2048
#define SELFID_BUFFER_ALIGN 2048

/* PCI command register bits, for enabling bus mastering. */
#define PCI_COMMAND_REGISTER      0x04
#define PCI_COMMAND_MEMORY_SPACE  (1 << 1)
#define PCI_COMMAND_BUS_MASTER    (1 << 2)

struct FireWireLivesOHCI_IVars
{
    IOPCIDevice              *pciDevice;
    IOMemoryMap              *barMap;
    uint64_t                  barAddress;
    uint64_t                  barLength;

    IOInterruptDispatchSource *interruptSource;
    OSAction                  *interruptAction;
    IODispatchQueue           *workQueue;

    /* Self-ID DMA buffer: the controller writes the post-reset topology here. */
    IOBufferMemoryDescriptor  *selfIDBuffer;
    IODMACommand              *selfIDDMACommand;
    uint64_t                   selfIDPhysicalAddress;
    void                      *selfIDVirtualAddress;

    /* Current bus state. */
    fw_topology_t              topology;
    uint32_t                   generation;
    uint16_t                   localNodeID;
    bool                       isRoot;
};

bool FireWireLivesOHCI::init(void)
{
    if (!super::init())
        return false;

    ivars = IONewZero(FireWireLivesOHCI_IVars, 1);
    return ivars != nullptr;
}

void FireWireLivesOHCI::free(void)
{
    if (ivars)
        IOSafeDeleteNULL(ivars, FireWireLivesOHCI_IVars, 1);
    super::free();
}

/* ------------------------------------------------------------------ */
/* MMIO accessors                                                      */
/* ------------------------------------------------------------------ */

kern_return_t FireWireLivesOHCI::RegisterRead(uint32_t offset, uint32_t *value)
{
    if (offset + sizeof(uint32_t) > ivars->barLength)
        return kIOReturnBadArgument;

    /* VERIFY: MemoryRead32 signature is (memoryIndex, offset, *value). */
    return ivars->pciDevice->MemoryRead32(OHCI_BAR_INDEX, offset, value);
}

kern_return_t FireWireLivesOHCI::RegisterWrite(uint32_t offset, uint32_t value)
{
    if (offset + sizeof(uint32_t) > ivars->barLength)
        return kIOReturnBadArgument;

    return ivars->pciDevice->MemoryWrite32(OHCI_BAR_INDEX, offset, value);
}

/* ------------------------------------------------------------------ */
/* Controller bring-up                                                 */
/* ------------------------------------------------------------------ */

kern_return_t FireWireLivesOHCI::ResetController(void)
{
    kern_return_t ret = RegisterWrite(OHCI_HCControlSet,
                                      OHCI_HCControl_softReset);
    if (ret != kIOReturnSuccess)
        return ret;

    /* softReset self-clears when the reset completes. The spec allows up to
     * 500ms; poll rather than sleeping blindly so a wedged controller is
     * reported instead of silently half-initialised. */
    for (int i = 0; i < 500; i++) {
        uint32_t hcControl = 0;
        ret = RegisterRead(OHCI_HCControlSet, &hcControl);
        if (ret != kIOReturnSuccess)
            return ret;

        if ((hcControl & OHCI_HCControl_softReset) == 0) {
            Log("soft reset completed after %dms", i);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }

    Log("ERROR: soft reset never cleared - controller is wedged");
    return kIOReturnTimeout;
}

kern_return_t FireWireLivesOHCI::EnableLinkPowerStatus(void)
{
    /* LPS powers the PHY and starts the PHY/link communication. Until it is
     * stable, reads of PHY registers and of NodeID return garbage - this is
     * the single most common bring-up bug in 1394 drivers. */
    kern_return_t ret = RegisterWrite(OHCI_HCControlSet, OHCI_HCControl_LPS);
    if (ret != kIOReturnSuccess)
        return ret;

    /* The spec requires a settling delay; 50ms is what every production
     * driver uses. Then confirm the link is actually answering. */
    IOSleep(50);

    for (int i = 0; i < 100; i++) {
        uint32_t hcControl = 0;
        ret = RegisterRead(OHCI_HCControlSet, &hcControl);
        if (ret != kIOReturnSuccess)
            return ret;

        if (hcControl & OHCI_HCControl_LPS) {
            /* Prove the register file is live rather than reading back zeros
             * or all-ones from a dead BAR. */
            uint32_t version = 0;
            ret = RegisterRead(OHCI_Version, &version);
            if (ret != kIOReturnSuccess)
                return ret;

            if (version == 0 || version == 0xffffffff) {
                Log("ERROR: BAR reads back 0x%08x - MMIO is not mapped", version);
                return kIOReturnNotReady;
            }

            Log("LPS stable; OHCI version %u.%u",
                (version >> 16) & 0xff, (version >> 8) & 0xff);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }

    Log("ERROR: LPS never came up - check cable power and the PHY");
    return kIOReturnTimeout;
}

kern_return_t FireWireLivesOHCI::ConfigureSelfIDBuffer(void)
{
    kern_return_t ret;

    /*
     * Allocate the self-ID buffer. The controller DMAs the post-bus-reset
     * self-ID packet burst into it, so it must be physically contiguous and
     * 2KB aligned.
     *
     * VERIFY: on Apple Silicon the device sees IOVAs through the DART IOMMU,
     * not physical addresses. IODMACommand::PrepareForDMA is what produces the
     * address the controller must be given; writing a CPU physical address
     * into ConfigROMmap/SelfIDBuffer will fault the DART.
     */
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           SELFID_BUFFER_SIZE,
                                           SELFID_BUFFER_ALIGN,
                                           &ivars->selfIDBuffer);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: could not allocate the self-ID buffer: 0x%x", ret);
        return ret;
    }

    /* TODO: create an IODMACommand against ivars->pciDevice, call
     * PrepareForDMA() on the buffer, and take the resulting IOVA. The 32-bit
     * SelfIDBuffer register means the mapping must be constrained to the low
     * 4GB via an IODMACommandSpecification with maxAddressBits = 32. */

    ret = RegisterWrite(OHCI_SelfIDBuffer,
                        (uint32_t)ivars->selfIDPhysicalAddress);
    if (ret != kIOReturnSuccess)
        return ret;

    /* Tell the link to accept self-ID packets and PHY packets. */
    return RegisterWrite(OHCI_LinkControlSet,
                         OHCI_LinkControl_rcvSelfID |
                         OHCI_LinkControl_rcvPhyPkt);
}

kern_return_t FireWireLivesOHCI::EnableLink(void)
{
    kern_return_t ret;

    /* Unmask the events we actually handle. Leaving the rest masked keeps a
     * misbehaving controller from livelocking us in the interrupt handler. */
    ret = RegisterWrite(OHCI_IntMaskSet,
                        OHCI_Int_busReset |
                        OHCI_Int_selfIDComplete |
                        OHCI_Int_reqTxComplete |
                        OHCI_Int_respTxComplete |
                        OHCI_Int_RQPkt |
                        OHCI_Int_RSPkt |
                        OHCI_Int_isochTx |
                        OHCI_Int_isochRx |
                        OHCI_Int_postedWriteErr |
                        OHCI_Int_unrecoverableError |
                        OHCI_Int_cycleTooLong |
                        OHCI_Int_regAccessFail);
    if (ret != kIOReturnSuccess)
        return ret;

    ret = RegisterWrite(OHCI_HCControlSet, OHCI_HCControl_linkEnable);
    if (ret != kIOReturnSuccess)
        return ret;

    /* Finally arm the master interrupt enable. */
    return RegisterWrite(OHCI_IntMaskSet, OHCI_Int_masterIntEnable);
}

/* ------------------------------------------------------------------ */
/* PHY register access                                                 */
/* ------------------------------------------------------------------ */

kern_return_t FireWireLivesOHCI::PhyRead(uint8_t address, uint8_t *value)
{
    kern_return_t ret = RegisterWrite(OHCI_PhyControl,
                                      OHCI_PhyControl_Read(address));
    if (ret != kIOReturnSuccess)
        return ret;

    for (int i = 0; i < 100; i++) {
        uint32_t phyControl = 0;
        ret = RegisterRead(OHCI_PhyControl, &phyControl);
        if (ret != kIOReturnSuccess)
            return ret;

        if (phyControl & OHCI_PhyControl_ReadDone) {
            *value = (uint8_t)((phyControl >> OHCI_PhyControl_ReadDataShift) & 0xff);
            return kIOReturnSuccess;
        }
        IOSleep(1);
    }

    return kIOReturnTimeout;
}

kern_return_t FireWireLivesOHCI::PhyWrite(uint8_t address, uint8_t value)
{
    return RegisterWrite(OHCI_PhyControl,
                         OHCI_PhyControl_Write(address, value));
}

/* ------------------------------------------------------------------ */
/* Interrupt handling                                                  */
/* ------------------------------------------------------------------ */

void FireWireLivesOHCI::InterruptOccurred(OSAction *action, uint64_t count,
                                          uint64_t time)
{
    uint32_t events = 0;
    uint32_t mask = 0;

    if (RegisterRead(OHCI_IntEventClear, &events) != kIOReturnSuccess)
        return;
    if (RegisterRead(OHCI_IntMaskClear, &mask) != kIOReturnSuccess)
        return;

    /* Only act on events we asked for; the event register reports everything
     * regardless of the mask. */
    events &= mask;
    if (events == 0)
        return;

    /* Acknowledge up front so we do not lose an event that arrives while we
     * are still processing this one. */
    RegisterWrite(OHCI_IntEventClear, events);

    if (events & OHCI_Int_unrecoverableError) {
        Log("FATAL: controller reported an unrecoverable error; stopping");
        RegisterWrite(OHCI_IntMaskClear, OHCI_Int_masterIntEnable);
        return;
    }

    if (events & OHCI_Int_busReset)
        HandleBusReset();

    if (events & OHCI_Int_selfIDComplete)
        HandleSelfIDComplete();

    /* TODO: async transmit/receive context completion, isochronous contexts. */
}

void FireWireLivesOHCI::HandleBusReset(void)
{
    /*
     * A bus reset invalidates every outstanding transaction and every node ID.
     * The generation counter is what makes that safe: any request issued
     * against a stale generation must be failed rather than retried, or it
     * could land on a different device that has since taken the node ID.
     */
    ivars->generation++;
    Log("bus reset (generation %u)", ivars->generation);

    /* Self-ID packets are still arriving; the real work happens when
     * selfIDComplete fires. */
}

void FireWireLivesOHCI::HandleSelfIDComplete(void)
{
    uint32_t selfIDCount = 0;
    if (RegisterRead(OHCI_SelfIDCount, &selfIDCount) != kIOReturnSuccess)
        return;

    if (selfIDCount & OHCI_SelfIDCount_selfIDError) {
        Log("self-ID error reported; waiting for the next reset");
        return;
    }

    /* Size is a byte count including the one-quadlet header the controller
     * writes at the start of the buffer. */
    uint32_t sizeBytes = (selfIDCount >> OHCI_SelfIDCount_sizeShift) &
                         OHCI_SelfIDCount_sizeMask;
    if (sizeBytes < 8 || sizeBytes > SELFID_BUFFER_SIZE) {
        Log("implausible self-ID size %u", sizeBytes);
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

    uint32_t nodeID = 0;
    if (RegisterRead(OHCI_NodeID, &nodeID) == kIOReturnSuccess &&
        (nodeID & OHCI_NodeID_idValid)) {
        ivars->localNodeID = (uint16_t)(nodeID & 0xffff);
        ivars->isRoot = (nodeID & OHCI_NodeID_root) != 0;
    }

    Log("bus enumerated: %u node(s), root is phy %u, we are phy %u%s",
        ivars->topology.node_count,
        ivars->topology.root_id,
        ivars->localNodeID & OHCI_NodeID_nodeNumberMask,
        ivars->isRoot ? " (root)" : "");

    if (!ivars->topology.gap_count_consistent)
        Log("gap counts disagree; a PHY config packet is needed");

    /* TODO: read each node's config ROM and publish matching nubs so the
     * SBP-2 / AV/C / audio layers can attach. */
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

kern_return_t IMPL(FireWireLivesOHCI, Start)
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
        goto fail;
    }

    /*
     * Enable memory space and bus mastering. Without the bus master bit the
     * controller cannot DMA, and every self-ID and async context silently
     * does nothing - it looks exactly like broken hardware.
     */
    {
        uint16_t command = 0;
        ivars->pciDevice->ConfigurationRead16(PCI_COMMAND_REGISTER, &command);
        command |= PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER;
        ivars->pciDevice->ConfigurationWrite16(PCI_COMMAND_REGISTER, command);
    }

    /* VERIFY: exact call for resolving BAR length in the current SDK. */
    ret = ivars->pciDevice->_CopyDeviceMemoryWithIndex(OHCI_BAR_INDEX,
                                                       &ivars->barMap, this);
    if (ret != kIOReturnSuccess) {
        Log("ERROR: could not map BAR%d: 0x%x", OHCI_BAR_INDEX, ret);
        goto fail;
    }
    ivars->barAddress = ivars->barMap->GetAddress();
    ivars->barLength  = ivars->barMap->GetLength();
    Log("BAR%d mapped: 0x%llx length 0x%llx",
        OHCI_BAR_INDEX, ivars->barAddress, ivars->barLength);

    if (ivars->barLength < 0x800) {
        Log("ERROR: BAR is only 0x%llx bytes; too small for an OHCI register file",
            ivars->barLength);
        ret = kIOReturnNoDevice;
        goto fail;
    }

    if ((ret = ResetController())      != kIOReturnSuccess) goto fail;
    if ((ret = EnableLinkPowerStatus()) != kIOReturnSuccess) goto fail;
    if ((ret = ConfigureSelfIDBuffer()) != kIOReturnSuccess) goto fail;

    /* TODO: create the IOInterruptDispatchSource and its OSAction, bound to a
     * dispatch queue, before enabling the link - otherwise the first bus
     * reset interrupt arrives with nowhere to go. */

    if ((ret = EnableLink()) != kIOReturnSuccess) goto fail;

    RegisterService();
    Log("started");
    return kIOReturnSuccess;

fail:
    Stop(provider, SUPERDISPATCH);
    return ret;
}

kern_return_t IMPL(FireWireLivesOHCI, Stop)
{
    Log("stopping");

    if (ivars->pciDevice) {
        /* Mask everything and quiesce the link before we let go. */
        RegisterWrite(OHCI_IntMaskClear, 0xffffffff);
        RegisterWrite(OHCI_HCControlClear, OHCI_HCControl_linkEnable);
        ivars->pciDevice->Close(this, 0);
        OSSafeReleaseNULL(ivars->pciDevice);
    }

    OSSafeReleaseNULL(ivars->barMap);
    OSSafeReleaseNULL(ivars->selfIDBuffer);
    OSSafeReleaseNULL(ivars->interruptSource);
    OSSafeReleaseNULL(ivars->interruptAction);
    OSSafeReleaseNULL(ivars->workQueue);

    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(FireWireLivesOHCI, NewUserClient)
{
    /* TODO: instantiate FireWireLivesUserClient so the userspace stack can
     * issue async transactions and map isochronous buffers. */
    return kIOReturnUnsupported;
}
