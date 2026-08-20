/*
 * fwprobe - look for a 1394 OHCI controller and report what macOS thinks of it.
 *
 * This is the ground-truth tool for the project. On macOS 26 there is no
 * FireWire driver left, so an attached Thunderbolt-to-FireWire adapter shows
 * up as an unclaimed PCI device with class code 0x0c0010. Finding it here
 * proves the hardware enumerates and tells us the BAR layout and IDs the dext
 * will need to match on.
 *
 * Build: make probe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "fw/ohci_regs.h"
#include "fw/fw_selfid.h"
#include "fw/fw_configrom.h"

static uint32_t cf_number(CFTypeRef ref, uint32_t fallback)
{
    uint32_t value = fallback;

    if (ref == NULL)
        return fallback;

    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &value);
    } else if (CFGetTypeID(ref) == CFDataGetTypeID()) {
        /* IOKit stores PCI config properties as little-endian CFData. */
        CFDataRef data = (CFDataRef)ref;
        CFIndex len = CFDataGetLength(data);
        if (len >= 4) {
            const UInt8 *bytes = CFDataGetBytePtr(data);
            value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
                    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
        }
    }

    return value;
}

static uint32_t io_prop_u32(io_registry_entry_t entry, const char *key,
                            uint32_t fallback)
{
    CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                  kCFStringEncodingUTF8);
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, cfkey,
                                                    kCFAllocatorDefault, 0);
    CFRelease(cfkey);

    uint32_t value = cf_number(ref, fallback);
    if (ref)
        CFRelease(ref);
    return value;
}

static void io_entry_name(io_registry_entry_t entry, char *out, size_t out_size)
{
    io_name_t name;
    if (IORegistryEntryGetName(entry, name) == KERN_SUCCESS)
        snprintf(out, out_size, "%s", name);
    else
        snprintf(out, out_size, "<unnamed>");
}

/* Is anything driving this device, or is it sitting unclaimed? */
static bool has_driver_attached(io_registry_entry_t entry)
{
    io_iterator_t children = IO_OBJECT_NULL;
    if (IORegistryEntryGetChildIterator(entry, kIOServicePlane, &children)
            != KERN_SUCCESS)
        return false;

    io_object_t child = IOIteratorNext(children);
    bool found = (child != IO_OBJECT_NULL);
    if (child)
        IOObjectRelease(child);
    IOObjectRelease(children);

    return found;
}


/* Read a boolean IORegistry property. */
static bool io_prop_bool(io_registry_entry_t entry, const char *key)
{
    CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                  kCFStringEncodingUTF8);
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, cfkey,
                                                    kCFAllocatorDefault, 0);
    CFRelease(cfkey);

    bool value = false;
    if (ref) {
        if (CFGetTypeID(ref) == CFBooleanGetTypeID())
            value = CFBooleanGetValue((CFBooleanRef)ref);
        CFRelease(ref);
    }
    return value;
}

/* True if the property exists at all, whatever its type. */
static bool io_prop_exists(io_registry_entry_t entry, const char *key)
{
    CFStringRef cfkey = CFStringCreateWithCString(kCFAllocatorDefault, key,
                                                  kCFStringEncodingUTF8);
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, cfkey,
                                                    kCFAllocatorDefault, 0);
    CFRelease(cfkey);

    bool present = (ref != NULL);
    if (ref)
        CFRelease(ref);
    return present;
}

/*
 * Decode BAR0 from the "assigned-addresses" property.
 *
 * This is the OpenFirmware PCI binding: five 32-bit cells per BAR, laid out as
 * phys.hi, address low, address high, size low, size high. It is preferred
 * over IODeviceMemory because that property is an array of IOMemoryDescriptor
 * objects which do not serialise to a CFDictionary through the CF property
 * API, even though ioreg renders them as one.
 *
 * phys.hi packs the space code, bus, device/function and the config-space
 * register offset of the BAR (0x10 = BAR0).
 */
static bool io_bar0(io_registry_entry_t entry, uint64_t *address,
                    uint64_t *length, uint8_t *bar_offset)
{
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        entry, CFSTR("assigned-addresses"), kCFAllocatorDefault, 0);
    if (ref == NULL)
        return false;

    bool found = false;

    if (CFGetTypeID(ref) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength((CFDataRef)ref);
        const UInt8 *bytes = CFDataGetBytePtr((CFDataRef)ref);

        /* Each entry is 5 cells of 4 bytes. Walk them and take the first
         * memory-space BAR. */
        for (CFIndex off = 0; off + 20 <= len; off += 20) {
            uint32_t cell[5];
            for (int c = 0; c < 5; c++) {
                const UInt8 *p = bytes + off + (c * 4);
                cell[c] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            }

            uint32_t space = (cell[0] >> 24) & 0x3u;
            if (space != 2 && space != 3)   /* 2 = 32-bit mem, 3 = 64-bit mem */
                continue;

            *address     = ((uint64_t)cell[2] << 32) | cell[1];
            *length      = ((uint64_t)cell[4] << 32) | cell[3];
            *bar_offset  = (uint8_t)(cell[0] & 0xffu);
            found = true;
            break;
        }
    }

    CFRelease(ref);
    return found;
}

/* Print the entitlements macOS says a driver must hold to claim this device. */
static void print_required_entitlements(io_registry_entry_t entry)
{
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        entry, CFSTR("IOServiceDEXTEntitlements"), kCFAllocatorDefault, 0);
    if (ref == NULL)
        return;

    if (CFGetTypeID(ref) == CFArrayGetTypeID()) {
        CFIndex outer = CFArrayGetCount((CFArrayRef)ref);
        for (CFIndex i = 0; i < outer; i++) {
            CFTypeRef inner = CFArrayGetValueAtIndex((CFArrayRef)ref, i);
            if (inner == NULL)
                continue;

            /* Entries are either a string or an array of strings. */
            if (CFGetTypeID(inner) == CFStringGetTypeID()) {
                char buf[256];
                if (CFStringGetCString((CFStringRef)inner, buf, sizeof(buf),
                                       kCFStringEncodingUTF8))
                    printf("      %s\n", buf);
            } else if (CFGetTypeID(inner) == CFArrayGetTypeID()) {
                CFIndex n = CFArrayGetCount((CFArrayRef)inner);
                for (CFIndex j = 0; j < n; j++) {
                    CFStringRef s = (CFStringRef)CFArrayGetValueAtIndex(
                        (CFArrayRef)inner, j);
                    char buf[256];
                    if (s && CFGetTypeID(s) == CFStringGetTypeID() &&
                        CFStringGetCString(s, buf, sizeof(buf),
                                           kCFStringEncodingUTF8))
                        printf("      %s\n", buf);
                }
            }
        }
    }

    CFRelease(ref);
}

static void describe_ohci(io_registry_entry_t entry, uint32_t class_code)
{
    char name[128];
    io_entry_name(entry, name, sizeof(name));

    uint32_t vendor  = io_prop_u32(entry, "vendor-id", 0) & 0xffffu;
    uint32_t device  = io_prop_u32(entry, "device-id", 0) & 0xffffu;
    uint32_t revision = io_prop_u32(entry, "revision-id", 0) & 0xffu;
    uint32_t subven  = io_prop_u32(entry, "subsystem-vendor-id", 0) & 0xffffu;
    uint32_t subdev  = io_prop_u32(entry, "subsystem-id", 0) & 0xffffu;

    printf("\n  \033[1;32mFOUND 1394 OHCI CONTROLLER\033[0m\n");
    printf("    IORegistry name : %s\n", name);
    printf("    class code      : 0x%06x (serial bus / FireWire / OHCI)\n",
           class_code);
    printf("    vendor:device   : %04x:%04x rev %02x\n", vendor, device, revision);
    printf("    subsystem       : %04x:%04x\n", subven, subdev);

    const char *vendor_name = "unknown";
    switch (vendor) {
    case 0x104c: vendor_name = "Texas Instruments"; break;
    case 0x11c1: vendor_name = "LSI / Agere"; break;
    case 0x1106: vendor_name = "VIA Technologies"; break;
    case 0x1033: vendor_name = "NEC"; break;
    case 0x1180: vendor_name = "Ricoh"; break;
    case 0x106b: vendor_name = "Apple"; break;
    }
    printf("    vendor name     : %s\n", vendor_name);

    bool claimed = has_driver_attached(entry);
    printf("    driver attached : %s\n",
           claimed ? "yes" : "\033[33mNO - unclaimed, available to our dext\033[0m");

    printf("    tunnelled       : %s\n",
           io_prop_bool(entry, "IOPCITunnelled") ? "yes (behind Thunderbolt)" : "no");

    /* BAR0 is the OHCI register file. The dext maps this and every register
     * access in ohci_regs.h is an offset into it. */
    uint64_t bar_addr = 0, bar_len = 0;
    uint8_t  bar_off = 0;
    if (io_bar0(entry, &bar_addr, &bar_len, &bar_off)) {
        printf("    BAR%d (cfg 0x%02x) : bus address 0x%llx, %llu bytes\n",
               (bar_off - 0x10) / 4, bar_off, bar_addr, bar_len);

        /* The register file runs to 0x400 + 32 iso contexts * 0x20 = 0x800. */
        if (bar_len < 0x800)
            printf("      \033[31mWARNING: smaller than the 0x800 OHCI register file\033[0m\n");
        else
            printf("      large enough for the full OHCI register file (needs 0x800)\n");
    } else {
        printf("    BAR0            : not reported\n");
    }

    /*
     * DMA is the part of this project most likely to be hard. On Apple
     * Silicon the controller reaches memory through the DART IOMMU, and OHCI's
     * DMA address registers are only 32 bits wide, so every mapping must be
     * both DART-translated and constrained to the low 4GB.
     */
    bool dart = io_prop_exists(entry, "iommu-parent");
    uint32_t page_size = io_prop_u32(entry, "IOPCIDeviceMapperPageSize", 0);
    printf("    IOMMU           : %s\n",
           dart ? "\033[33mDART present - DMA needs IOVAs, not physical addresses\033[0m"
                : "none reported");
    if (page_size)
        printf("    mapper page size: %u bytes\n", page_size);

    printf("\n    macOS says a driver must hold these entitlements to claim it:\n");
    print_required_entitlements(entry);

    printf("\n    Matches our Info.plist personality:\n");
    printf("      IOPCIClassMatch = \"0x0c001000&0xffffff00\"\n");
}

int main(void)
{
    printf("fwprobe - FireWire_Lives hardware survey\n");
    printf("========================================\n");

    /* Confirm the OS state this project exists to work around. */
    printf("\nKernel FireWire support:\n");
    io_service_t fw = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("IOFireWireController"));
    if (fw != IO_OBJECT_NULL) {
        printf("  IOFireWireController is present - a native stack is running.\n");
        IOObjectRelease(fw);
    } else {
        printf("  IOFireWireController: \033[33mabsent\033[0m "
               "(expected on macOS 26 - the kexts were removed)\n");
    }

    /* Walk every PCI device looking for the OHCI class code. */
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceGetMatchingServices(
        kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &iter);

    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Could not enumerate IOPCIDevice: 0x%08x\n", kr);
        return 1;
    }

    printf("\nPCI devices:\n");

    int total = 0, found = 0;
    io_object_t dev;

    while ((dev = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        total++;

        uint32_t class_code = io_prop_u32(dev, "class-code", 0xffffffffu);

        /* Compare the upper 24 bits: base class, subclass, prog-if. */
        if ((class_code & OHCI_PCI_CLASS_MASK) == OHCI_PCI_CLASS_CODE) {
            found++;
            describe_ohci(dev, class_code & OHCI_PCI_CLASS_MASK);
        }

        IOObjectRelease(dev);
    }
    IOObjectRelease(iter);

    printf("  scanned %d PCI device(s)\n", total);

    if (found == 0) {
        printf("\n  \033[33mNo 1394 OHCI controller found.\033[0m\n");
        printf("  Plug in the Apple Thunderbolt-to-FireWire adapter and re-run.\n");
        printf("  An Apple Silicon Mac has no built-in FireWire port, so the\n");
        printf("  controller only appears once the adapter is attached and the\n");
        printf("  Thunderbolt device has been authorised.\n");
    } else {
        printf("\n  %d OHCI controller(s) found.\n", found);
    }

    /* Report Thunderbolt state, since that is the path the adapter arrives on. */
    printf("\nThunderbolt:\n");
    io_iterator_t tb = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOThunderboltPort"), &tb) == KERN_SUCCESS) {
        int ports = 0;
        io_object_t p;
        while ((p = IOIteratorNext(tb)) != IO_OBJECT_NULL) {
            ports++;
            IOObjectRelease(p);
        }
        IOObjectRelease(tb);
        printf("  %d Thunderbolt port(s) present\n", ports);
    }

    return found > 0 ? 0 : 2;
}
