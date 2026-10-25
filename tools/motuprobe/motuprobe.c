/*
 * motuprobe - read the control registers of a MOTU FireWire interface.
 *
 * MOTU's current macOS driver (1.7) has no FireWire transport at all: only
 * its USB and Thunderbolt-PCI providers remain, so a FireWire-only unit such
 * as the UltraLite mk3 enumerates on the bus and is then claimed by nothing.
 * This tool is the first step towards a MOTU backend: it proves we can talk
 * to the unit by issuing read-only quadlet reads through Apple's own
 * IOFireWireLib, which still works on macOS 15 and earlier.
 *
 * By default nothing is written to the device. With --rate it changes the
 * sample rate the way the Linux driver does: rewrite only the rate field of
 * the clock status register, then wait for the unit to report the new rate.
 * Register offsets and field layouts come from the Linux ALSA driver
 * (sound/firewire/motu/), the only public description of the protocol.
 *
 * Build: make motu
 * Usage: motuprobe [--rate 44100|48000|88200|96000|176400|192000]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libkern/OSByteOrder.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>

#define OUI_MOTU            0x0001f2u

/* All MOTU registers live at 0xffff'f000'0000 + offset. */
#define MOTU_ADDR_HI        0xffffu
#define MOTU_ADDR_LO_BASE   0xf0000000u

/* Shared by every protocol version (motu-stream.c, motu-transaction.c). */
#define ISOC_COMM_CONTROL   0x0b00u
#define ASYNC_ADDR_HI       0x0b04u
#define ASYNC_ADDR_LO       0x0b08u
#define PACKET_FORMAT       0x0b10u

/* Protocol v3 (motu-protocol-v3.c). */
#define V3_CLOCK_STATUS     0x0b14u
#define  V3_FETCH_PCM_FRAMES 0x02000000u
#define  V3_CLOCK_RATE_MASK  0x0000ff00u
#define  V3_CLOCK_RATE_SHIFT 8
#define V3_CLK_WAIT_MSEC    4000
#define V3_OPT_IFACE_MODE   0x0c94u
/* Command-DSP models: where the unit sends its mixer state (snd-firewire-ctl-services
 * protocols/motu/src/command_dsp.rs). */
#define V3_DSP_MSG_DST_HI   0x0b38u
#define V3_DSP_MSG_DST_LO   0x0b3cu

static const unsigned clock_rates[] = { 44100, 48000, 88200, 96000,
                                        176400, 192000 };
#define CLOCK_RATE_COUNT (sizeof(clock_rates) / sizeof(clock_rates[0]))

struct motu_model {
    uint32_t version;
    const char *name;
    int protocol;
};

/* From the device table in motu.c. */
static const struct motu_model models[] = {
    { 0x000001, "828",                   1 },
    { 0x000002, "896",                   1 },
    { 0x000003, "828mk2",                2 },
    { 0x000005, "896HD",                 2 },
    { 0x000009, "Traveler",              2 },
    { 0x00000d, "UltraLite",             2 },
    { 0x00000f, "8pre",                  2 },
    { 0x000015, "828mk3 (FireWire)",     3 },
    { 0x000017, "896mk3 (FireWire)",     3 },
    { 0x000019, "UltraLite mk3 (FireWire)", 3 },
    { 0x00001b, "Traveler mk3",          3 },
    { 0x000030, "UltraLite mk3 Hybrid",  3 },
    { 0x000033, "Audio Express",         3 },
    { 0x000035, "828mk3 Hybrid",         3 },
    { 0x000037, "896mk3 Hybrid",         3 },
    { 0x000039, "Track 16",              3 },
    { 0x000045, "4pre",                  3 },
};

static const struct motu_model *find_model(uint32_t version)
{
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++)
        if (models[i].version == version)
            return &models[i];
    return NULL;
}

static uint32_t io_prop_u32(io_registry_entry_t entry, const char *key,
                            uint32_t fallback)
{
    CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                  kCFStringEncodingUTF8);
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, cfkey,
                                                    kCFAllocatorDefault, 0);
    CFRelease(cfkey);

    uint32_t value = fallback;
    if (ref && CFGetTypeID(ref) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
    if (ref)
        CFRelease(ref);
    return value;
}

static IOFireWireLibDeviceRef open_interface(io_service_t unit)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    IOFireWireLibDeviceRef dev = NULL;

    kern_return_t kr = IOCreatePlugInInterfaceForService(
        unit, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || plugin == NULL) {
        fprintf(stderr, "  cannot create FireWire plugin: 0x%08x\n", kr);
        return NULL;
    }

    HRESULT hr = (*plugin)->QueryInterface(
        plugin, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID),
        (LPVOID *)&dev);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK || dev == NULL) {
        fprintf(stderr, "  cannot get device interface: 0x%08x\n", (unsigned)hr);
        return NULL;
    }

    IOReturn ret = (*dev)->Open(dev);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "  cannot open device: 0x%08x%s\n", ret,
                ret == kIOReturnExclusiveAccess
                    ? " (another driver holds it)" : "");
        (*dev)->Release(dev);
        return NULL;
    }
    return dev;
}

static IOReturn read_reg(IOFireWireLibDeviceRef dev, io_service_t unit,
                         uint32_t offset, uint32_t *value)
{
    UInt32 generation = 0;
    IOReturn ret = (*dev)->GetBusGeneration(dev, &generation);
    if (ret != kIOReturnSuccess)
        return ret;

    FWAddress addr = { 0 };
    addr.addressHi = MOTU_ADDR_HI;
    addr.addressLo = MOTU_ADDR_LO_BASE + offset;

    /*
     * IOFireWireLib hands back the quadlet in bus order, big-endian, not host
     * order: on arm64 the "1394" signature reads as 0x34393331 unswapped.
     */
    UInt32 val = 0;
    ret = (*dev)->ReadQuadlet(dev, unit, &addr, &val, true, generation);
    if (ret == kIOReturnSuccess)
        *value = OSSwapBigToHostInt32(val);
    return ret;
}

static IOReturn write_reg(IOFireWireLibDeviceRef dev, io_service_t unit,
                          uint32_t offset, uint32_t value)
{
    UInt32 generation = 0;
    IOReturn ret = (*dev)->GetBusGeneration(dev, &generation);
    if (ret != kIOReturnSuccess)
        return ret;

    FWAddress addr = { 0 };
    addr.addressHi = MOTU_ADDR_HI;
    addr.addressLo = MOTU_ADDR_LO_BASE + offset;

    return (*dev)->WriteQuadlet(dev, unit, &addr, OSSwapHostToBigInt32(value),
                                true, generation);
}

static void print_reg(const char *name, uint32_t offset, IOReturn ret,
                      uint32_t value)
{
    if (ret == kIOReturnSuccess)
        printf("  %-18s 0x%04x  0x%08x\n", name, offset, value);
    else
        printf("  %-18s 0x%04x  read failed: 0x%08x\n", name, offset, ret);
}

static void decode_isoc_comm(uint32_t v)
{
    printf("    rx stream: %s, channel %u\n",
           (v & 0x40000000u) ? "active" : "idle", (v >> 24) & 0x3fu);
    printf("    tx stream: %s, channel %u\n",
           (v & 0x00400000u) ? "active" : "idle", (v >> 16) & 0x3fu);
}

static void decode_v3_clock(uint32_t v)
{
    uint32_t rate = (v >> 8) & 0xffu;
    uint32_t src = v & 0xffu;
    const char *src_name;

    switch (src) {
    case 0x00: src_name = "internal"; break;
    case 0x01: src_name = "word clock (BNC)"; break;
    case 0x02: src_name = "SMPTE (SPH)"; break;
    case 0x08: src_name = "AES/EBU (XLR)"; break;
    case 0x10: src_name = "S/PDIF (coax)"; break;
    case 0x18: src_name = "optical A"; break;
    case 0x19: src_name = "optical B"; break;
    default:   src_name = "unknown"; break;
    }

    if (rate < CLOCK_RATE_COUNT)
        printf("    sample rate: %u Hz\n", clock_rates[rate]);
    else
        printf("    sample rate: unknown index %u\n", rate);
    printf("    clock source: %s (0x%02x)\n", src_name, src);
    printf("    fetch PCM frames: %s\n", (v & 0x02000000u) ? "yes" : "no");
}

/*
 * Mirrors snd_motu_protocol_v3_set_clock_rate(). Linux waits for the unit's
 * CLK_CHANGED notification; we have not registered a notification address
 * (0x0b04/0x0b08), so poll the register until the new rate reads back.
 */
static int set_v3_rate(IOFireWireLibDeviceRef dev, io_service_t unit,
                       unsigned rate)
{
    uint32_t index;
    for (index = 0; index < CLOCK_RATE_COUNT; index++)
        if (clock_rates[index] == rate)
            break;
    if (index == CLOCK_RATE_COUNT) {
        printf("  unsupported rate %u\n", rate);
        return -1;
    }

    uint32_t before = 0;
    IOReturn ret = read_reg(dev, unit, V3_CLOCK_STATUS, &before);
    if (ret != kIOReturnSuccess) {
        printf("  clock status read failed: 0x%08x\n", ret);
        return -1;
    }

    uint32_t data = before & ~(V3_CLOCK_RATE_MASK | V3_FETCH_PCM_FRAMES);
    data |= index << V3_CLOCK_RATE_SHIFT;
    if (data == before) {
        printf("\n  already at %u Hz, nothing written\n", rate);
        return 0;
    }

    printf("\n  writing clock status 0x%08x -> 0x%08x\n", before, data);
    ret = write_reg(dev, unit, V3_CLOCK_STATUS, data);
    if (ret != kIOReturnSuccess) {
        printf("  write failed: 0x%08x\n", ret);
        return -1;
    }

    const struct timespec step = { 0, 50 * 1000 * 1000 };
    for (int waited = 0; waited <= V3_CLK_WAIT_MSEC; waited += 50) {
        uint32_t now = 0;
        if (read_reg(dev, unit, V3_CLOCK_STATUS, &now) == kIOReturnSuccess &&
            ((now & V3_CLOCK_RATE_MASK) >> V3_CLOCK_RATE_SHIFT) == index) {
            printf("  unit reports %u Hz after %d ms (0x%08x)\n", rate,
                   waited, now);
            return 0;
        }
        nanosleep(&step, NULL);
    }
    printf("  timed out: rate did not read back within %d ms\n",
           V3_CLK_WAIT_MSEC);
    return -1;
}

static void probe_unit(io_service_t unit, unsigned set_rate)
{
    uint32_t version = io_prop_u32(unit, "Unit_SW_Version", 0);
    const struct motu_model *model = find_model(version);

    printf("MOTU unit, version 0x%06x: %s\n", version,
           model ? model->name : "unknown model");
    if (model)
        printf("  protocol v%d\n", model->protocol);

    IOFireWireLibDeviceRef dev = open_interface(unit);
    if (dev == NULL)
        return;

    UInt16 node = 0;
    UInt32 generation = 0;
    (*dev)->GetBusGeneration(dev, &generation);
    if ((*dev)->GetRemoteNodeID(dev, generation, &node) == kIOReturnSuccess)
        printf("  node 0x%04x, bus generation %u\n", node, generation);

    /*
     * Prove the reads reach this unit and not some other node: the bus info
     * block at 0x400 must carry the "1394" signature and a GUID whose top 24
     * bits are MOTU's OUI.
     */
    uint32_t bus_name = 0, guid_hi = 0, guid_lo = 0;
    if (read_reg(dev, unit, 0x0404, &bus_name) != kIOReturnSuccess ||
        read_reg(dev, unit, 0x040c, &guid_hi) != kIOReturnSuccess ||
        read_reg(dev, unit, 0x0410, &guid_lo) != kIOReturnSuccess ||
        bus_name != 0x31333934u || (guid_hi >> 8) != OUI_MOTU) {
        printf("  config ROM check failed (0x%08x 0x%08x): not talking to "
               "the MOTU, stopping\n", bus_name, guid_hi);
        (*dev)->Close(dev);
        (*dev)->Release(dev);
        return;
    }
    printf("  config ROM verified, GUID %08x%08x\n", guid_hi, guid_lo);

    struct { const char *name; uint32_t offset; } regs[] = {
        { "isoc comm control", ISOC_COMM_CONTROL },
        { "async addr hi",     ASYNC_ADDR_HI },
        { "async addr lo",     ASYNC_ADDR_LO },
        { "packet format",     PACKET_FORMAT },
        { "v3 clock status",   V3_CLOCK_STATUS },
        { "v3 opt iface mode", V3_OPT_IFACE_MODE },
        { "dsp msg dst hi",    V3_DSP_MSG_DST_HI },
        { "dsp msg dst lo",    V3_DSP_MSG_DST_LO },
    };

    printf("\n  register           offset  value\n");
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (regs[i].offset >= V3_CLOCK_STATUS && model && model->protocol != 3)
            continue;

        uint32_t value = 0;
        IOReturn ret = read_reg(dev, unit, regs[i].offset, &value);
        print_reg(regs[i].name, regs[i].offset, ret, value);
        if (ret != kIOReturnSuccess)
            continue;

        if (regs[i].offset == ISOC_COMM_CONTROL)
            decode_isoc_comm(value);
        else if (regs[i].offset == V3_CLOCK_STATUS)
            decode_v3_clock(value);
    }

    if (set_rate) {
        if (model && model->protocol == 3)
            set_v3_rate(dev, unit, set_rate);
        else
            printf("\n  --rate is only implemented for protocol v3\n");
    }

    (*dev)->Close(dev);
    (*dev)->Release(dev);
}

int main(int argc, char **argv)
{
    unsigned set_rate = 0;
    if (argc == 3 && strcmp(argv[1], "--rate") == 0) {
        set_rate = (unsigned)strtoul(argv[2], NULL, 10);
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--rate HZ]\n", argv[0]);
        return 1;
    }

    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("IOFireWireUnit"), &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed: 0x%08x\n", kr);
        return 1;
    }

    int found = 0;
    io_service_t unit;
    while ((unit = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        if (io_prop_u32(unit, "Unit_Spec_ID", 0) == OUI_MOTU) {
            if (found++)
                printf("\n");
            probe_unit(unit, set_rate);
        }
        IOObjectRelease(unit);
    }
    IOObjectRelease(iter);

    if (!found) {
        printf("no MOTU FireWire unit found\n");
        return 2;
    }
    return 0;
}
