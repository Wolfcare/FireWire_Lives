#!/bin/bash
# macos26-ultralite-check.sh - one command to check a MOTU UltraLite mk3 under
# ASFireWire on macOS 26+, against what MOTU's own driver did on macOS 15.8
# (docs/reference/ultralite-mk3/).
#
# Run it on macOS 26 with ASFireWire installed and the UltraLite powered on:
#
#   ./scripts/macos26-ultralite-check.sh
#
# It checks the driver extension, the device in Core Audio at every sample rate
# (channel counts and names), that capture really streams, the clock source
# menu, and the MIDI ports, then collects the driver's logs. Everything goes to
# a folder under reports/ in this repository (git-ignored) with a summary in
# report.md; attach that folder to an ASFireWire issue.
#
# It changes the sample rate and the clock source while it runs, and puts both
# back to 44.1 kHz / Internal at the end. Nothing else is written.

set -u

here="$(cd "$(dirname "$0")/.." && pwd)"
cadump="$here/build/cadump"
mididump="$here/build/mididump"
ref="$here/docs/reference/ultralite-mk3"
out="$here/reports/ASFW-UltraLite-mk3-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/raw"
report="$out/report.md"
device="UltraLite"
pass=0; fail=0; warn=0

result() {  # status, check, detail
    printf '| %s | %s | %s |\n' "$1" "$2" "$3" >> "$report"
    case "$1" in PASS) pass=$((pass + 1));; FAIL) fail=$((fail + 1));; *) warn=$((warn + 1));; esac
    printf '%-5s %s: %s\n' "$1" "$2" "$3"
}

# Channel names as MOTU's driver publishes them; ASFW's spelling is mapped to them.
normalize() {
    sed -e 's/Main L$/Main Out 1/' -e 's/Main R$/Main Out 2/' \
        -e 's/Return L$/Return 1/' -e 's/Return R$/Return 2/' \
        -e 's/Reverb L$/Reverb 1/' -e 's/Reverb R$/Reverb 2/' \
        -e 's/Phones L$/Phones 1/' -e 's/Phones R$/Phones 2/'
}
channels() {  # dump file, "in " or "out" -> one name per line
    grep -E "^    $2 +[0-9]+  " "$1" | sed -E 's/^ +(in |out) +[0-9]+  //' | normalize
}

if [ ! -x "$cadump" ] || [ ! -x "$mididump" ]; then
    (cd "$here" && make build/cadump build/mididump >/dev/null) || { echo "cannot build the tools"; exit 1; }
fi

{
    echo "# MOTU UltraLite mk3 under ASFireWire"
    echo
    echo "$(date)"
    echo
    echo "| Result | Check | Detail |"
    echo "|---|---|---|"
} > "$report"

# --- system and driver -------------------------------------------------------
os="$(sw_vers -productVersion)"
if [ "${os%%.*}" -ge 26 ]; then result PASS "macOS" "$os ($(sw_vers -buildVersion))"
else result WARN "macOS" "$os: this check is meant for macOS 26 or later"; fi

systemextensionsctl list > "$out/raw/systemextensions.txt" 2>&1
asfw="$(grep -E 'net\.asfw\.driver' "$out/raw/systemextensions.txt" | head -1)"
if echo "$asfw" | grep -q "activated enabled"; then
    result PASS "ASFireWire driver" "$(echo "$asfw" | grep -oE 'net\.asfw\.driver \([^)]*\)')"
else
    result FAIL "ASFireWire driver" "not active: ${asfw:-not installed}. Install it from https://www.asfw.net/install/, approve it, restart."
fi

system_profiler SPThunderboltDataType > "$out/raw/thunderbolt.txt" 2>&1
if grep -q "Thunderbolt to FireWire Adapter" "$out/raw/thunderbolt.txt"; then
    result PASS "FireWire adapter" "Apple Thunderbolt to FireWire Adapter connected"
else
    result FAIL "FireWire adapter" "not on the Thunderbolt bus"
fi

# --- the device at every rate --------------------------------------------------
if ! "$cadump" "$device" > "$out/raw/coreaudio-start.txt" 2>&1; then
    result FAIL "Audio device" "no Core Audio device named *$device*. Is the UltraLite on, and was the driver approved?"
else
    result PASS "Audio device" "$(head -1 "$out/raw/coreaudio-start.txt" | sed 's/^device: //')"
    for rate in 44100 48000 88200 96000 176400 192000; do
        dump="$out/raw/coreaudio-$rate.txt"
        if ! "$cadump" --rate "$rate" "$device" > "$dump" 2>&1; then
            result FAIL "$rate Hz" "the device did not switch: $(tail -1 "$dump")"
            continue
        fi
        problems=""
        for dir in "in " "out"; do
            if ! diff <(channels "$ref/coreaudio-$rate.txt" "$dir") <(channels "$dump" "$dir") > "$out/raw/diff-$rate-${dir// /}.txt"; then
                problems="$problems ${dir// /}puts differ from MOTU's (raw/diff-$rate-${dir// /}.txt);"
            fi
        done
        if [ -z "$problems" ]; then
            result PASS "$rate Hz" "$(grep -E '^  in  streams|^  out streams' "$dump" | grep -oE '\([0-9]+ channels\)' | tr '\n' ' ')same channels as MOTU's driver"
        else
            result FAIL "$rate Hz" "$problems"
        fi
    done
    "$cadump" --rate 44100 "$device" > /dev/null 2>&1
    sleep 3  # let the unit settle at 44.1 kHz before measuring

    # --- capture streams -------------------------------------------------------
    # A unit coming back down from 192 kHz can take several seconds to run at
    # speed again (MOTU's own driver did), so one slow first measurement is retried.
    for attempt in 1 2; do
        "$cadump" --capture 3 "$device" > "$out/raw/capture-44100.txt" 2>&1
        rate_line="$(grep '^capture:' "$out/raw/capture-44100.txt")"
        fps="$(echo "$rate_line" | grep -oE '\(([0-9]+) frames/s\)' | grep -oE '[0-9]+')"
        if [ -n "$fps" ] && [ "$fps" -gt 43000 ] && [ "$fps" -lt 45500 ]; then break; fi
        sleep 5
    done
    if [ -n "$fps" ] && [ "$fps" -gt 43000 ] && [ "$fps" -lt 45500 ]; then
        result PASS "Capture rate" "$rate_line"
    else
        result FAIL "Capture rate" "${rate_line:-capture did not run}; expected about 44100 frames/s"
    fi
    # Unconnected analog inputs still show a little noise (MOTU's driver:
    # 0.00001-0.00005). All zeros means no samples arrive from the unit.
    live="$(sed -n '/^capture:/,$p' "$out/raw/capture-44100.txt" | grep -E 'Analog [0-9].*peak' | awk '$NF > 0' | wc -l | tr -d ' ')"
    if [ "$live" -gt 0 ]; then
        result PASS "Capture content" "$live analog inputs carry signal or noise"
    else
        result FAIL "Capture content" "every analog input is exact zero: capture packets carry no samples"
    fi

    # --- clock source ----------------------------------------------------------
    sources="$(sed -n '/clock sources:/,/^  in /p' "$out/raw/coreaudio-start.txt" | grep -E '^    0x' || true)"
    if echo "$sources" | grep -q "S/PDIF" && echo "$sources" | grep -q "SMPTE"; then
        spdif="$(echo "$sources" | grep 'S/PDIF' | awk '{print $1}')"
        internal="$(echo "$sources" | grep 'Internal' | awk '{print $1}')"
        if "$cadump" --clock "$spdif" "$device" > "$out/raw/clock-spdif.txt" 2>&1 &&
           "$cadump" --clock "$internal" "$device" > "$out/raw/clock-internal.txt" 2>&1; then
            result PASS "Clock source" "Internal / S/PDIF / SMPTE listed; switched to S/PDIF and back"
        else
            result FAIL "Clock source" "listed, but switching failed (raw/clock-*.txt)"
        fi
    elif [ -n "$sources" ]; then
        result WARN "Clock source" "listed but not as MOTU's: $(echo "$sources" | tr '\n' ' ')"
    else
        result WARN "Clock source" "no clock source menu (expected only with the motu-ultralite-mk3-fixes changes)"
    fi
fi

# --- MIDI ----------------------------------------------------------------------
if "$mididump" > "$out/raw/midi.txt" 2>&1; then
    result PASS "MIDI ports" "$(tail -1 "$out/raw/midi.txt")"
else
    result WARN "MIDI ports" "$(tail -1 "$out/raw/midi.txt") (expected only with the motu-ultralite-mk3-fixes changes)"
fi

# --- logs ------------------------------------------------------------------------
/usr/bin/log show --last 20m --info --debug --style compact --predicate \
    'eventMessage CONTAINS "[MOTU" OR eventMessage CONTAINS "[Midi" OR eventMessage CONTAINS "[Controls]" OR eventMessage CONTAINS "[AudioHost]" OR eventMessage CONTAINS "[MotuNotify]" OR eventMessage CONTAINS "LogConfig initialized"' \
    > "$out/raw/asfw-log.txt" 2>&1
result INFO "Driver log" "$(wc -l < "$out/raw/asfw-log.txt" | tr -d ' ') lines in raw/asfw-log.txt"
ls /Library/Logs/DiagnosticReports/net.asfw.driver-* > "$out/raw/crash-reports.txt" 2>/dev/null &&
    result WARN "Crash reports" "the driver crashed at some point: see raw/crash-reports.txt" ||
    result PASS "Crash reports" "none"

{
    echo
    echo "$pass passed, $fail failed, $warn to look at."
    echo
    echo "Not checked by this script (needs ears or cables): sound out of each output, sending and"
    echo "receiving MIDI, and the mixer in the ASFW app following the front panel."
} >> "$report"

echo
echo "$pass passed, $fail failed, $warn to look at. Report: $report"
[ -z "${NO_OPEN:-}" ] && open "$out" 2>/dev/null || true
[ "$fail" -eq 0 ]
