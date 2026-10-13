#!/bin/bash
# Capture a reference render of a named Serum 2 preset, headlessly.
#
# Route (ticket 20): the plugin loads <contentRoot>/Presets/User/Default.SerumPreset as its
# init patch when that file exists (the string is in the binary next to DefaultFX.SerumPreset,
# and the path is absent from the shipped content root). Installing the wanted preset there and
# instantiating the plugin makes it load that preset with no GUI and no host preset surface.
#
# The factory content root is world-writable, so no sudo is needed; this script restores it on
# every exit path (including interrupts). Nothing derived from the reference is written outside
# the output paths the caller names.
#
# usage:
#   render-preset.sh --preset FILE --out OUT.wav [--state OUT.state] [--seconds N]
#                    [--plugin PATH] [--bin PATH] [--root DIR] [--keep] [--verify]
#                    [-- extra args...]
#
# --verify renders twice and requires the two WAVs to be byte-identical. Serum 2 loads a
# preset's assets asynchronously, so a render that starts too soon races the loader and varies
# run to run; the defaults below (pre-roll 128 blocks, then a 1.5 s wall-clock wait) are the
# smallest configuration measured to be reproducible in this session. Pass --pre-roll/--settle-ms
# yourself to override, and raise them for multisample-heavy presets.
#
# Examples:
#   render-preset.sh --preset "$P/Organ/OR - Organ Flute.SerumPreset" \
#                    --out /tmp/org.wav --state /tmp/org.state --seconds 2
#
# Exit codes: 0 ok, 1 bad usage, 2 restore conflict (left the file in place), 3 render failed.

set -u

PRESET=""; OUT=""; STATE=""; SECONDS_=2
PLUGIN="/Library/Audio/Plug-Ins/VST3/Serum2.vst3"
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/host/xerum-host_artefacts/Release/xerum-host"
ROOT="${XERUM_SERUM_CONTENT_ROOT:-/Library/Audio/Presets/Xfer Records/Serum 2 Presets}"
KEEP=0
VERIFY=0
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2 ;;
        --out)    OUT="$2"; shift 2 ;;
        --state)  STATE="$2"; shift 2 ;;
        --seconds) SECONDS_="$2"; shift 2 ;;
        --plugin) PLUGIN="$2"; shift 2 ;;
        --bin)    BIN="$2"; shift 2 ;;
        --root)   ROOT="$2"; shift 2 ;;
        --keep)   KEEP=1; shift ;;
        --verify) VERIFY=1; shift ;;
        --) shift; EXTRA=("$@"); break ;;
        *) echo "unrecognised argument: $1" >&2; exit 1 ;;
    esac
done

[ -n "$PRESET" ] && [ -f "$PRESET" ] || { echo "--preset FILE is required and must exist" >&2; exit 1; }
[ -n "$OUT" ] || { echo "--out FILE is required" >&2; exit 1; }
[ -x "$BIN" ] || { echo "host binary not found: $BIN (build it, or pass --bin)" >&2; exit 1; }

TARGET_DIR="$ROOT/Presets/User"
TARGET="$TARGET_DIR/Default.SerumPreset"

# Never clobber a file we did not put there: back it up and put it back verbatim.
BACKUP=""
if [ -e "$TARGET" ]; then
    BACKUP="$(mktemp /tmp/xerum-default-preset-backup.XXXXXX)"
    cp -p "$TARGET" "$BACKUP"
fi

restore() {
    if [ $KEEP -eq 1 ]; then
        echo "leaving $TARGET in place (--keep)"
        return
    fi
    if [ -n "$BACKUP" ]; then
        cp -p "$BACKUP" "$TARGET" && rm -f "$BACKUP"
    else
        rm -f "$TARGET"
    fi
    echo "restored: $(ls -A "$TARGET_DIR" | tr '\n' ' ')"
}
trap restore EXIT INT TERM

mkdir -p "$OUT" 2>/dev/null
OUT_DIR="$(dirname "$OUT")"; [ -d "$OUT_DIR" ] || { echo "output directory missing: $OUT_DIR" >&2; exit 1; }

cp "$PRESET" "$TARGET" || { echo "cannot install preset at $TARGET" >&2; exit 2; }
echo "installed $(basename "$PRESET") as $TARGET"

# Settling defaults, unless the caller overrode them.
SETTLE=()
has_settle=0
for a in ${EXTRA+"${EXTRA[@]}"}; do
    case "$a" in --pre-roll|--settle-ms) has_settle=1 ;; esac
done
[ $has_settle -eq 0 ] && SETTLE=(--pre-roll 128 --settle-ms 1500)

render() {
    local out="$1"; shift
    local args=(--plugin "$PLUGIN" --out "$out" --seconds "$SECONDS_")
    [ -n "$STATE" ] && args+=(--dump-state "$STATE")
    args+=(${SETTLE+"${SETTLE[@]}"})
    [ ${#EXTRA[@]} -gt 0 ] && args+=("${EXTRA[@]}")
    "$BIN" "${args[@]}"
}

render "$OUT"
rc=$?
[ $rc -eq 0 ] || exit 3

if [ $VERIFY -eq 1 ]; then
    render "$OUT.verify" >/dev/null || { echo "verify render failed" >&2; exit 3; }
    if cmp -s "$OUT" "$OUT.verify"; then
        rm -f "$OUT.verify"
        echo "verified: two renders are byte-identical"
    else
        echo "NOT REPRODUCIBLE: $OUT and $OUT.verify differ - raise --pre-roll/--settle-ms" >&2
        exit 4
    fi
fi

exit 0
