#!/bin/bash
# Does the reference instrument open a preset xerum wrote? Ticket 32's method, as one command.
#
# The whole check lives in the model suite — `tests/model/reference_roundtrip_test.cpp` — because the
# xerum half has to run in-process (decode -> PresetDocument -> writeContainer). This script is the
# entry point that records the method, finds the pieces, and reports where the artefacts went:
#
#   * the test writes a preset with xerum's own writer and installs it where the reference reads it,
#     `<contentRoot>/Presets/User/Default.SerumPreset` (ticket 20's load path);
#   * the reference is instantiated through `host/` and its 2623 parameters are read back and compared
#     against the reference's own reading of the factory file;
#   * a render of both is compared byte for byte under the settle protocol (`--pre-roll 128
#     --settle-ms 1500`);
#   * the slot is restored on every exit path, including a failed assertion, by a guard inside the test
#     — this script never touches the reference's tree itself, which is deliberate: a `cp`/`rm` pair in a
#     shell script is how the slot gets clobbered when something dies halfway.
#
# Everything it produces is written outside the repository, to `~/.xerum-fixtures/roundtrip/`, because
# nothing derived from the reference is committed (ticket 10).
#
# usage:
#   reference-roundtrip.sh [--bin PATH] [--build DIR] [--corpus [--limit N]] [-- extra catch2 args]
#
#   --bin PATH      the model test binary. Default: the first build*/tests/model/xerum-model-tests.
#   --build DIR     configure and build DIR first (cmake -S . -B DIR && cmake --build DIR ...).
#   --corpus        also walk the bank through the reference (a stride sample by default; --limit N
#                   widens it, 626 being the whole factory bank).
#   --              everything after this goes to the test binary verbatim.
#
# Without the reference installed the suite skips loudly and prints why; that is a pass with a visible
# reason, never a silent one (docs/test-strategy.md).
#
# Exit codes: 0 ok (or skipped), 1 bad usage, 2 the test binary is missing, 3 a test failed.

set -u

BIN=""
BUILD=""
CORPUS=0
LIMIT=""
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --bin)      BIN="${2:-}"; shift 2 ;;
        --build)    BUILD="${2:-}"; shift 2 ;;
        --corpus)   CORPUS=1; shift ;;
        --limit)    LIMIT="${2:-}"; shift 2 ;;
        --)         shift; EXTRA=("$@"); break ;;
        -h|--help)  sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

REPO="$(cd "$(dirname "$0")/.." && pwd)"

if [ -n "$BUILD" ]; then
    cmake -S "$REPO" -B "$REPO/$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
    cmake --build "$REPO/$BUILD" --target xerum-model-tests -j"$(sysctl -n hw.ncpu)" || exit 2
fi

if [ -z "$BIN" ]; then
    for candidate in "$REPO"/build*/tests/model/xerum-model-tests; do
        [ -x "$candidate" ] && BIN="$candidate" && break
    done
fi

[ -n "$BIN" ] && [ -x "$BIN" ] || {
    echo "model test binary not found; build it first, e.g." >&2
    echo "  $0 --build build-32" >&2
    exit 2
}

FILTER="[roundtrip]"
if [ "$CORPUS" -eq 0 ]; then
    FILTER='[roundtrip]~[corpus]'
fi

if [ -n "$LIMIT" ]; then
    export XERUM_ROUNDTRIP_LIMIT="$LIMIT"
fi

echo "binary : $BIN"
echo "filter : $FILTER"
[ -n "$LIMIT" ] && echo "limit  : $LIMIT presets"
echo "artefacts: $HOME/.xerum-fixtures/roundtrip/"
echo

"$BIN" "$FILTER" "${EXTRA[@]+"${EXTRA[@]}"}"
rc=$?

echo
echo "artefacts left in $HOME/.xerum-fixtures/roundtrip/ (nothing derived from the reference is committed)"
exit $rc
