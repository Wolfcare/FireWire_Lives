/*
 * cadump - dump how Core Audio presents an audio device.
 *
 * Records the reference a FireWire driver on macOS 26 must reproduce: what
 * the vendor's own driver publishes on macOS 15 for the same hardware.
 * Channel names, stream layout, sample rates, clock sources and latencies.
 *
 * Read-only unless --rate or --clock is given. Those set the nominal sample
 * rate or the clock source, the way a user would in Audio MIDI Setup, so the
 * per-rate layout and the vendor driver's register writes can be recorded.
 *
 * Build: make cadump
 * Usage: cadump [--rate HZ] [--clock ID] [name-substring]
 *        (default substring: "MOTU"; clock IDs are listed in the dump)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <CoreAudio/CoreAudio.h>

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                       AudioObjectPropertyScope scope,
                                       AudioObjectPropertyElement elem)
{
    AudioObjectPropertyAddress a = { sel, scope, elem };
    return a;
}

static int get_string(AudioObjectID obj, AudioObjectPropertyAddress a,
                      char *buf, size_t len)
{
    CFStringRef s = NULL;
    UInt32 size = sizeof(s);
    buf[0] = '\0';
    if (!AudioObjectHasProperty(obj, &a) ||
        AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &s) != noErr ||
        s == NULL)
        return -1;
    CFStringGetCString(s, buf, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(s);
    return 0;
}

static UInt32 get_u32(AudioObjectID obj, AudioObjectPropertyAddress a,
                      UInt32 fallback)
{
    UInt32 v = fallback, size = sizeof(v);
    if (!AudioObjectHasProperty(obj, &a) ||
        AudioObjectGetPropertyData(obj, &a, 0, NULL, &size, &v) != noErr)
        return fallback;
    return v;
}

static Float64 get_rate(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain);
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &rate);
    return rate;
}

static AudioObjectID find_device(const char *needle)
{
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL,
                                       &size) != noErr)
        return kAudioObjectUnknown;

    AudioObjectID *ids = malloc(size);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids);

    AudioObjectID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
        char name[256];
        get_string(ids[i], addr(kAudioObjectPropertyName,
                                kAudioObjectPropertyScopeGlobal,
                                kAudioObjectPropertyElementMain),
                   name, sizeof(name));
        if (strstr(name, needle)) {
            found = ids[i];
            break;
        }
    }
    free(ids);
    return found;
}

static int set_rate(AudioObjectID dev, Float64 rate)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain);
    if (AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(rate), &rate) != noErr)
        return -1;

    /* The change is asynchronous; wait up to 5 s for it to land. */
    const struct timespec step = { 0, 100 * 1000 * 1000 };
    for (int i = 0; i < 50; i++) {
        if (get_rate(dev) == rate)
            return 0;
        nanosleep(&step, NULL);
    }
    return -1;
}

static int set_clock(AudioObjectID dev, UInt32 id)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyClockSource,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain);
    if (AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(id), &id) != noErr)
        return -1;

    const struct timespec step = { 0, 100 * 1000 * 1000 };
    for (int i = 0; i < 50; i++) {
        if (get_u32(dev, a, ~0u) == id)
            return 0;
        nanosleep(&step, NULL);
    }
    return -1;
}

static void dump_scope(AudioObjectID dev, AudioObjectPropertyScope scope,
                       const char *label)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreamConfiguration,
                                        scope, kAudioObjectPropertyElementMain);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr)
        return;
    AudioBufferList *bl = malloc(size);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, bl);

    UInt32 total = 0;
    printf("  %s streams:", label);
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) {
        printf(" %u", bl->mBuffers[i].mNumberChannels);
        total += bl->mBuffers[i].mNumberChannels;
    }
    printf("  (%u channels)\n", total);
    free(bl);

    printf("  %s latency %u, safety offset %u frames\n", label,
           get_u32(dev, addr(kAudioDevicePropertyLatency, scope,
                             kAudioObjectPropertyElementMain), 0),
           get_u32(dev, addr(kAudioDevicePropertySafetyOffset, scope,
                             kAudioObjectPropertyElementMain), 0));

    for (UInt32 ch = 1; ch <= total; ch++) {
        char name[256];
        if (get_string(dev, addr(kAudioObjectPropertyElementName, scope, ch),
                       name, sizeof(name)) != 0 || name[0] == '\0')
            strcpy(name, "(unnamed)");
        printf("    %s %2u  %s\n", label, ch, name);
    }
}

static void dump_clock_sources(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyClockSources,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain);
    UInt32 size = 0;
    if (!AudioObjectHasProperty(dev, &a) ||
        AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr ||
        size == 0) {
        printf("  clock sources: none published\n");
        return;
    }
    UInt32 *ids = malloc(size);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, ids);
    UInt32 current = get_u32(dev, addr(kAudioDevicePropertyClockSource,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain), ~0u);

    printf("  clock sources:\n");
    for (UInt32 i = 0; i < size / sizeof(UInt32); i++) {
        UInt32 id = ids[i];
        CFStringRef s = NULL;
        AudioValueTranslation t = { &id, sizeof(id), &s, sizeof(s) };
        AudioObjectPropertyAddress na = addr(
            kAudioDevicePropertyClockSourceNameForIDCFString,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain);
        UInt32 tsize = sizeof(t);
        char name[256] = "?";
        if (AudioObjectGetPropertyData(dev, &na, 0, NULL, &tsize, &t) == noErr && s) {
            CFStringGetCString(s, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(s);
        }
        printf("    0x%08x  %s%s\n", id, name, id == current ? "  (current)" : "");
    }
    free(ids);
}

static void dump_device(AudioObjectID dev)
{
    AudioObjectPropertyAddress g = addr(0, kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain);
    char buf[256];

    g.mSelector = kAudioObjectPropertyName;
    get_string(dev, g, buf, sizeof(buf));
    printf("device: %s\n", buf);
    g.mSelector = kAudioDevicePropertyDeviceUID;
    get_string(dev, g, buf, sizeof(buf));
    printf("  uid: %s\n", buf);

    UInt32 transport = get_u32(dev, addr(kAudioDevicePropertyTransportType,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain), 0);
    printf("  transport: '%c%c%c%c'\n", (transport >> 24) & 0xff,
           (transport >> 16) & 0xff, (transport >> 8) & 0xff, transport & 0xff);
    printf("  nominal rate: %.0f Hz\n", get_rate(dev));
    printf("  buffer size: %u frames\n",
           get_u32(dev, addr(kAudioDevicePropertyBufferFrameSize,
                             kAudioObjectPropertyScopeGlobal,
                             kAudioObjectPropertyElementMain), 0));

    AudioObjectPropertyAddress ra = addr(kAudioDevicePropertyAvailableNominalSampleRates,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &ra, 0, NULL, &size) == noErr && size) {
        AudioValueRange *r = malloc(size);
        AudioObjectGetPropertyData(dev, &ra, 0, NULL, &size, r);
        printf("  available rates:");
        for (UInt32 i = 0; i < size / sizeof(*r); i++)
            printf(" %.0f", r[i].mMinimum);
        printf("\n");
        free(r);
    }

    dump_clock_sources(dev);
    dump_scope(dev, kAudioObjectPropertyScopeInput, "in ");
    dump_scope(dev, kAudioObjectPropertyScopeOutput, "out");
}

int main(int argc, char **argv)
{
    const char *needle = "MOTU";
    Float64 rate = 0;
    long clock = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
            rate = strtod(argv[++i], NULL);
        else if (strcmp(argv[i], "--clock") == 0 && i + 1 < argc)
            clock = strtol(argv[++i], NULL, 0);
        else
            needle = argv[i];
    }

    AudioObjectID dev = find_device(needle);
    if (dev == kAudioObjectUnknown) {
        printf("no audio device matching \"%s\"\n", needle);
        return 2;
    }

    if (rate > 0 && set_rate(dev, rate) != 0) {
        printf("could not set %.0f Hz\n", rate);
        return 1;
    }

    if (clock >= 0 && set_clock(dev, (UInt32)clock) != 0) {
        printf("could not select clock source 0x%lx\n", clock);
        return 1;
    }

    dump_device(dev);
    return 0;
}
