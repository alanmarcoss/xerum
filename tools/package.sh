#!/usr/bin/env bash
#
# package.sh — assemble the unsigned distribution artefact (ticket 47).
#
# Stages what a person needs to take xerum to another machine, and **checks** what it staged
# rather than asserting it: the three bundles are present and build for arm64, the licences are in
# the tree, xerum's own content passes the provenance gate, and nothing reference-shaped has leaked
# in (ticket `10`).
#
#     tools/package.sh [--build-dir DIR] [--out DIR] [--no-tar]
#
# Defaults are `--build-dir build` and `--out dist`, producing
#
#     <out>/xerum-<version>-macos-arm64/          the staged tree
#     <out>/xerum-<version>-macos-arm64.tar.gz    unless --no-tar, plus its sha256
#
# and `MANIFEST.sha256` inside the staged tree, which is also verified before the script exits.
#
# **Unsigned, by decision.** Ticket `11` cut v1 as unsigned local builds and the map keeps signing
# and notarization as fog; nothing here signs, notarizes or staples anything, and a build downloaded
# through a browser will be quarantined by Gatekeeper (README.md says what to do about it).
#
# Exit status: 0 staged and verified, 1 a check failed, 2 bad usage.

set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build_dir=build
out_dir=dist
make_tar=yes

usage() {
    sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build_dir=${2:-}; shift 2 ;;
        --out)       out_dir=${2:-};   shift 2 ;;
        --no-tar)    make_tar=no;      shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "package.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$build_dir" ] && [ -n "$out_dir" ] || { echo "package.sh: --build-dir and --out need a value" >&2; exit 2; }

# Relative paths resolve against the repository, not the caller's cwd, so the script can be run from
# anywhere — which is how a person packaging a build they made elsewhere will run it.
case "$build_dir" in /*) ;; *) build_dir="$repo/$build_dir" ;; esac
case "$out_dir"   in /*) ;; *) out_dir="$repo/$out_dir"   ;; esac

version=$(sed -n 's/^project(xerum VERSION \([0-9][0-9.]*\).*/\1/p' "$repo/CMakeLists.txt")
[ -n "$version" ] || { echo "package.sh: cannot read the version from CMakeLists.txt" >&2; exit 1; }

name="xerum-$version-macos-arm64"
stage="$out_dir/$name"

problems=0
fail() { echo "FAIL: $*" >&2; problems=$((problems + 1)); }
note() { echo "check: $*"; }

# ---------------------------------------------------------------------------------------------
# 1. The build's own output. A missing bundle is reported with the command that produces it, not
#    worked around: packaging cannot invent an artefact the build does not make.
# ---------------------------------------------------------------------------------------------
artefacts="$build_dir/plugin/xerum_artefacts/Release"

bundles="AU/xerum.component VST3/xerum.vst3 Standalone/xerum.app"

for bundle in $bundles; do
    path="$artefacts/$bundle"

    if [ ! -d "$path" ]; then
        fail "no $bundle in $artefacts — build it first: cmake --build $build_dir --target xerum_All"
    fi
done

[ "$problems" -eq 0 ] || { echo "package.sh: nothing staged" >&2; exit 1; }

# ---------------------------------------------------------------------------------------------
# 2. Stage the tree.
# ---------------------------------------------------------------------------------------------
case "$stage" in /|"$repo"/|"$repo") echo "package.sh: refusing to stage into '$stage'" >&2; exit 2 ;; esac
[ "$stage" != "$build_dir" ] || { echo "package.sh: refusing to stage onto the build directory" >&2; exit 2; }

rm -rf "$stage"
mkdir -p "$stage/AU" "$stage/VST3" "$stage/Standalone"

cp -R "$artefacts/AU/xerum.component"       "$stage/AU/xerum.component"
cp -R "$artefacts/VST3/xerum.vst3"          "$stage/VST3/xerum.vst3"
cp -R "$artefacts/Standalone/xerum.app"     "$stage/Standalone/xerum.app"

# xerum's own content — the starter tables the authoring tool generated, and their provenance
# manifest. Without it a preset naming a table resolves nothing (`docs/content-policy.md`), so it is
# part of "everything needed to run", not a sample pack bolted on.
cp -R "$repo/content" "$stage/content"

for required in LICENSE THIRD_PARTY.md README.md; do
    [ -s "$repo/$required" ] || { echo "package.sh: $required is missing from the repository" >&2; exit 1; }
done

cp "$repo/LICENSE"        "$stage/LICENSE"
cp "$repo/THIRD_PARTY.md" "$stage/THIRD_PARTY.md"
cp "$repo/README.md"      "$stage/README.md"

# ---------------------------------------------------------------------------------------------
# 3. Check the staged tree.
# ---------------------------------------------------------------------------------------------

# Every bundle is a bundle: a plist and an executable at the path a host looks in.
for bundle in $bundles; do
    executable="$stage/$bundle/Contents/MacOS/$(basename "${bundle%.*}")"

    [ -f "$stage/$bundle/Contents/Info.plist" ] || fail "$bundle has no Contents/Info.plist"
    [ -x "$executable" ] || fail "$bundle has no executable at Contents/MacOS/$(basename "${bundle%.*}")"
done

for document in LICENSE THIRD_PARTY.md README.md; do
    [ -s "$stage/$document" ] || fail "$document is missing or empty in the package"
done

# arm64 only, per the map's cut (CMakeLists.txt). A wrong-architecture bundle is a package that
# cannot run, and it is cheap to catch here.
for bundle in $bundles; do
    architectures=$(lipo -archs "$stage/$bundle/Contents/MacOS/$(basename "${bundle%.*}")" 2>/dev/null || echo unknown)

    case "$architectures" in
        *arm64*) note "$bundle: $architectures" ;;
        *)       fail "$bundle does not contain an arm64 slice (lipo says '$architectures')" ;;
    esac
done

# The provenance gate, run against the *shipped* copy rather than the working tree: every file under
# content/ must be declared in the manifest, and — where the reference is installed — none may be
# byte-identical to a reference file. It skips loudly where the reference is absent, which is a pass
# with a printed reason, exactly as `docs/content-policy.md` specifies.
if command -v python3 >/dev/null 2>&1; then
    if python3 "$repo/tools/check-content-provenance.py" --content "$stage/content"; then
        note "content provenance: ok"
    else
        fail "the packaged content did not pass tools/check-content-provenance.py"
    fi
else
    fail "python3 is not on PATH, so the packaged content could not be checked for provenance"
fi

# And the second guard, for what the provenance gate structurally cannot see: it walks the content
# root, so a reference-shaped file placed *outside* it — inside a bundle, say — would never be
# hashed by it. Any preset, sample pack or audio file outside xerum's own table directory is
# reference content by shape (ticket `10`), and it does not ship.
reference_shaped=0
while IFS= read -r -d '' file; do
    relative=${file#"$stage"/}

    case "$relative" in
        content/Tables/xerum/*.wav) continue ;;
    esac

    case "$relative" in
        *.SerumPreset|*.fxp|*.fxb|*.flac|*.sfz|*.aif|*.aiff|*.mp3|*.wav|*.xferpreset|*.XferShape)
            echo "FAIL: reference-shaped file in the package: $relative" >&2
            reference_shaped=1
            ;;
    esac
done < <(find "$stage" -type f -print0)

[ "$reference_shaped" -eq 0 ] || problems=$((problems + 1))
[ "$reference_shaped" -ne 0 ] || note "no reference-shaped file outside content/Tables/xerum/"

[ "$problems" -eq 0 ] || { echo "package.sh: FAILED ($problems problem(s)); the staged tree is at $stage" >&2; exit 1; }

# ---------------------------------------------------------------------------------------------
# 4. Hash what shipped, and check the hashes can be checked.
# ---------------------------------------------------------------------------------------------
(
    cd "$stage"
    find . -type f ! -name MANIFEST.sha256 -print0 | sort -z | xargs -0 shasum -a 256
) > "$stage/MANIFEST.sha256"

if (cd "$stage" && shasum -a 256 -c MANIFEST.sha256 > /dev/null); then
    note "MANIFEST.sha256: $(wc -l < "$stage/MANIFEST.sha256" | tr -d ' ') file(s), verified"
else
    fail "MANIFEST.sha256 does not verify against the staged tree"
    exit 1
fi

file_count=$(find "$stage" -type f | wc -l | tr -d ' ')
echo "staged: $stage ($file_count file(s), $(du -sh "$stage" | cut -f1))"

# ---------------------------------------------------------------------------------------------
# 5. The tarball, which is the thing that actually travels to another machine.
# ---------------------------------------------------------------------------------------------
if [ "$make_tar" = yes ]; then
    tarball="$out_dir/$name.tar.gz"
    rm -f "$tarball"
    tar -czf "$tarball" -C "$out_dir" "$name"

    echo "archive: $tarball ($(du -sh "$tarball" | cut -f1))"
    shasum -a 256 "$tarball"
fi

echo "package: ok (unsigned; signing and notarization are out of scope for v1)"
