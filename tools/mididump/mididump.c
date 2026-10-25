/*
 * mididump - list the CoreMIDI sources and destinations whose name or device
 * matches a substring, so a script can check that a driver published its MIDI
 * ports.
 *
 * Build: make mididump
 * Usage: mididump [name-substring]   (default substring: "UltraLite")
 * Exit 0 when at least one source and one destination match, 2 otherwise.
 */

#include <stdio.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

static void name_of(MIDIObjectRef obj, CFStringRef property, char *buf, size_t len)
{
    CFStringRef s = NULL;
    buf[0] = '\0';
    if (MIDIObjectGetStringProperty(obj, property, &s) == noErr && s) {
        CFStringGetCString(s, buf, (CFIndex)len, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

static int list(const char *label, ItemCount count, MIDIEndpointRef (*get)(ItemCount),
                const char *needle)
{
    int found = 0;
    for (ItemCount i = 0; i < count; i++) {
        MIDIEndpointRef ep = get(i);
        char name[256], display[256];
        name_of(ep, kMIDIPropertyName, name, sizeof(name));
        name_of(ep, kMIDIPropertyDisplayName, display, sizeof(display));
        if (!strstr(name, needle) && !strstr(display, needle)) continue;
        SInt32 offline = 0;
        MIDIObjectGetIntegerProperty(ep, kMIDIPropertyOffline, &offline);
        printf("%-12s %s%s\n", label, display[0] ? display : name, offline ? "  (offline)" : "");
        if (!offline) found++;
    }
    return found;
}

int main(int argc, char **argv)
{
    const char *needle = argc > 1 ? argv[1] : "UltraLite";
    const int sources = list("source", MIDIGetNumberOfSources(), MIDIGetSource, needle);
    const int destinations = list("destination", MIDIGetNumberOfDestinations(), MIDIGetDestination, needle);
    printf("%d source(s), %d destination(s) matching \"%s\"\n", sources, destinations, needle);
    return sources > 0 && destinations > 0 ? 0 : 2;
}
