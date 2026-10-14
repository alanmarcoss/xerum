# xerum

**xerum is an independent, from-scratch wavetable synthesiser for macOS (Apple silicon) that loads
Serum 2 presets and owns none of Serum 2's content.**

It is not Serum 2 and it is not a clone: no Xfer code, presets, wavetables, multisamples or audio
renders are in this repository, and none may be added. xerum is **not affiliated with, endorsed by or
sponsored by Xfer Records**; its preset-format knowledge was derived for interoperability, and the
legal posture behind that is recorded in ticket `10`.

It ships as an **AU (v2)**, a **VST3** and a **standalone application** from one build, under the
**GNU Affero General Public License v3.0** — see [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).

v1 is distributed as **unsigned local builds**. Signing and notarization are out of scope (the map
calls them fog) and nothing here does either.

---

## What xerum is

| | |
|---|---|
| **Purpose** | An independent implementation of the Serum 2 architecture: it reads the reference's `.SerumPreset` format and renders it with its own engine, its own interface and its own content. |
| **Formats** | AUv2 (`aumu` / `Xrm2` / `Xerm`), VST3, standalone — one CMake target, `FORMATS AU VST3 Standalone` |
| **Bundle id** | `com.xerum.xerum` |
| **Platform** | macOS 13 or later, arm64 only |
| **Language / build** | C++20, CMake ≥ 3.22 |
| **Framework** | JUCE 9.0.4, fetched by CMake and pinned to the commit of the 9.0.4 tag |
| **Licence** | AGPLv3, with JUCE taken under its AGPLv3 option (ticket `11`) |

xerum's AU identity is **deliberately not the reference's**. Serum 2 registers `aumu` / `Xf2X` /
`Xfer` / `com.xfer.serum2`; registering the same triple would replace or shadow the real plugin on any
machine that has both installed. Ours is `aumu` / `Xrm2` / `Xerm` / `com.xerum.xerum`, so the two
coexist.

## What it implements, and what it defers

The v1 cut was decided from measured factory-preset usage rather than taste (ticket `12`), and it is
the whole of what the engine claims:

**Implemented**

* **Five oscillator slots** — **OSC A, B, C** (wavetable) plus the fixed **NOISE** and **SUB** slots
  (tickets `12`, `40`). Wavetable appears in 529 of the 626 factory presets, NOISE in 296 and SUB in
  267.
* **Warp** — the seven families covering roughly 90 % of use: phase distortion, FM/PM,
  distortion/saturation, bend, sync/PWM, ring and amplitude modulation, filter-as-warp (tickets `12`,
  `25`).
* **Filters** — the underlying models rather than the 83 names: ladder flavours and slopes, the
  state-variable shelf/peak family, comb, flanger/phase, diffuser, and `PZ_SVF` fed by a preset's own
  pole-zero sections (tickets `12`, `26`).
* **Modulation** — the full 64-row matrix with real destination addressing (ticket `27`), with the
  interface's matrix editing rows that reach the engine (ticket `45`). The v1 source list is the head
  that covers 90 % of use (ticket `12`) and is being extended (ticket `48`).
* **FX** — 14 of the reference's 16 effect classes, covering 99 % of use, in the MAIN / BUS 1 / BUS 2
  rack arrangement (tickets `12`, `29`).
* **Voices** — polyphonic with a fixed ceiling, unison with a measured stereo spread, mono/legato,
  and MIDI note, sustain-pedal and pitch-bend handling (tickets `28`, `30`, `41`). Pitch bend is
  ±2 semitones, the reference's init value — stated, not measured (ticket `30`).
* **Preset import** — the **entire schema parses**, versions 3.0 through 6.0, including modules for
  engines v1 cannot synthesise, at the model and across the plugin boundary (tickets `12`, `23`, `42`;
  the normative contract is [docs/preset-import-contract.md](docs/preset-import-contract.md)).
* **An interface of xerum's own design** — device pages derived from the parameter table rather than
  hand-listed, the modulation matrix, the pole/zero plane, and the import report as a bar plus a
  drawer (tickets `43`, `44`). It is *not* a reproduction of Serum 2's.

**Deferred, by name** (ticket `12`): the granular, spectral, multisample and sample oscillator
engines; the clip player, arpeggiator and MIDI clips; MPE; the long warp and filter tails (roughly the
last 10 % of each); two rare FX classes; and the MultiSample asset pipeline.

A deferred engine is **not silently dropped**. The import contract's central rule is that the set of
silently dropped items is **empty**: every engine, warp, filter, effect, module, parameter and asset a
preset asks for and xerum cannot honour appears in the import report with its module path and a
severity. A slot that selects an engine v1 does not synthesise renders silence and says so.

The engine's own subset is bounded by the five slots above — a preset's other oscillators are still
parsed, still reported, and still preserved in the document, they simply do not sound.

## Build

**Requirements**

* macOS 13+ on Apple silicon (the project builds `arm64` only, per the map's cut)
* Xcode **Command Line Tools** (`xcode-select --install`) — a C++20 compiler and the macOS SDK
* **CMake ≥ 3.22** (`brew install cmake`, or the kitware `.dmg`)
* **git**, and a network connection on the **first** configure: CMake fetches **JUCE 9.0.4** (pinned by
  commit, ~130 MB shallow) and **Catch2 v3.16.1**
* Python 3, only for the content-provenance release check (plain `python3` is enough)

```sh
git clone https://github.com/alanmarcoss/xerum.git
cd xerum
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Every target is built: the three plugin bundles, the headless host harness, the offline authoring
tools and the test suites. To build only the plugin:

```sh
cmake --build build --target xerum_All -j8
```

**Where the artefacts land**

```
build/plugin/xerum_artefacts/Release/AU/xerum.component
build/plugin/xerum_artefacts/Release/VST3/xerum.vst3
build/plugin/xerum_artefacts/Release/Standalone/xerum.app
build/host/xerum-host_artefacts/Release/xerum-host        # headless host, see host/README.md
```

Nothing is signed: the linker's ad-hoc signature is all any of the three bundles carries.

Two build facts worth knowing before you look for them:

* **`build/_deps` holds the fetched dependencies.** JUCE and Catch2 are cloned there at configure time
  and are not vendored into the tree; deleting `build/` re-downloads them.
* **`XERUM_CONTENT_DIR` is compiled in as an absolute path to this checkout's `content/`**, so a build
  made here finds xerum's own starter tables with no install step. That path does not exist on another
  machine, which is why the packaged artefact carries `content/` with it (see
  [Packaging](#packaging-an-artefact-for-another-machine)).

**zstd is vendored**, not fetched: macOS ships no zstd to link against and every preset is a zstd
frame. `third_party/zstd` carries the version, commit and generation command; it is BSD 3-Clause,
taken at our option (ticket `11`, [THIRD_PARTY.md](THIRD_PARTY.md)).

## Install

### AU (v2)

```sh
mkdir -p ~/Library/Audio/Plug-Ins/Components
cp -R build/plugin/xerum_artefacts/Release/AU/xerum.component ~/Library/Audio/Plug-Ins/Components/

killall -9 AudioComponentRegistrar 2>/dev/null || true   # the registry is cached; or just reboot
auval -a | grep Xrm2                                     # → aumu Xrm2 Xerm - xerum: xerum
auval -strict -v aumu Xrm2 Xerm                          # → AU VALIDATION SUCCEEDED
```

`/Library/Audio/Plug-Ins/Components/` installs it for every user instead of for you alone.

### VST3

```sh
mkdir -p ~/Library/Audio/Plug-Ins/VST3
cp -R build/plugin/xerum_artefacts/Release/VST3/xerum.vst3 ~/Library/Audio/Plug-Ins/VST3/
```

Then rescan plug-ins in your host. (Or `/Library/Audio/Plug-Ins/VST3/` for all users.)

### Standalone

```sh
cp -R build/plugin/xerum_artefacts/Release/Standalone/xerum.app /Applications/
open /Applications/xerum.app
```

The standalone is a real deliverable rather than a developer harness (ticket `11`): it is the same
instrument with its own window, and it doubles as the tool the DSP and fidelity work renders through.

### Unsigned: what Gatekeeper will do

Nothing here is signed or notarized. A bundle you **build yourself** on the machine you run it on
carries no quarantine attribute and opens normally. A bundle that has travelled as a download gets one,
and macOS will refuse it until you clear it:

```sh
xattr -dr com.apple.quarantine /Applications/xerum.app
```

or right-click the app and choose **Open** the first time. The quarantine attribute travels with a
download and applies to the plugin bundles too: if a host will not show the AU or the VST3 after
copying it in from a download, clear it the same way on the bundle you installed.

The ad-hoc signature a fresh build carries **does not verify**, measured on this tree's own build:
`codesign -v` reports `a sealed resource is missing or invalid` on the VST3, and `code has no resources
but signature indicates they must be present` on the AU and the standalone. Hosts load the bundles and
`auval -strict` passes on them; making the signatures verify belongs to the signing and notarization
work the map keeps as fog.

### Content: one more step, and what happens without it

xerum ships a small content library of its own — `content/Tables/xerum/`: sine, saw, square and
triangle, a fallback table, and five tables generated from formulas we wrote — and resolves a preset's
asset references against, in order:

1. `XERUM_CONTENT_ROOT`, if you set it;
2. the reference instrument's own content tree, if it is installed — **read-only, and never written
   to**;
3. `~/Library/Application Support/xerum/content`;
4. this checkout's `content/`, which only exists in a development build.

So an installed build wants `content/` where root 3 looks:

```sh
mkdir -p ~/Library/Application\ Support/xerum/content
cp -R content/* ~/Library/Application\ Support/xerum/content/
```

Without it, xerum still runs, still imports presets and still reports — a preset naming a table simply
resolves nothing, and the report says so at `high` per occurrence rather than quietly substituting
something else (`docs/preset-import-contract.md` §5, §8). With no reference installed at all, xerum
resolves whatever content you give it from roots 1 and 3, and reports the rest.

**xerum never writes into the reference instrument's content tree.** That directory is another
application's install, with a database over it that the reference maintains.

## Getting a preset in

Open the plugin's interface — the standalone window, or the plugin editor in your host — and press
**Import .SerumPreset…**. A file chooser opens at the first content root that exists, for the file
type `*.SerumPreset`.

What happens after you pick a file: container framing → decompression → CBOR (or the legacy stream) →
`loadPreset` → a document plus an **import report** → the engine's settings and the interface. The
report is on screen the whole time, as a bar that opens a drawer:

* a **clean** import prints **"Nothing to report"** — a clean import's report is *empty, not absent*,
  and the two states must not look alike;
* otherwise every entry appears with its **severity** (`high` / `medium` / `info`), module path, item
  and reason, with counts per severity, and the device strip marks the pages whose modules the report
  named so a half-loaded preset is located where it happened.

Presets stay where you keep them; xerum does not manage a preset library. Your host's own preset
mechanism stores **xerum's state block**, which round-trips byte-identically, including the parameters
of a reference preset that the schema does not describe (`plugin/tests/state_test.cpp`, ticket `30`).
Exporting a `.SerumPreset` is not offered: it needs structure the model deliberately never held.

For scripted use, the headless harness drives the plugin without a GUI —
`build/host/xerum-host_artefacts/Release/xerum-host`; see [host/README.md](host/README.md) for its
flags (`--list-params`, `--set`, `--automate`, `--dump-state`, `--out`, rendering to WAV).

## Tests: what is proved where

```sh
ctest --test-dir build                       # every suite in the tree
ctest --test-dir build -R content-provenance # the content release gate alone
```

**Every case in the tree**, discovered by CTest from five suites — **314** when this was written, and
the number grows with the tree. To read it off the build you have:

```sh
ctest --test-dir build -N | tail -1
```

| Suite | Binary | Needs |
|---|---|---|
| Format — schema, containers, document, report | `build/tests/model/xerum-model-tests` | — |
| Format — content library | `build/tests/content/xerum-content-tests` | — |
| DSP — oscillators, warps, filters, modulation, voices, FX | `build/tests/dsp/xerum-dsp-tests` | — |
| Plugin — bridge, state, audio, import, real-time, interface | `build/plugin/tests/xerum-plugin-tests` | — |
| Release gate — content provenance | `tools/check-content-provenance.py` | python3; the byte-identity half needs the reference |

Assertions are behavioural and quantitative, never "it ran": a filter's slope must **measure** its
stated dB/oct, a modulator must **reach** its destination with the amount and polarity applied, a
compressor must **reduce** measured dynamic range, a missing table must be **reported** rather than
substituted, and no test asserts a parameter value where a render difference is the real claim.
[docs/test-strategy.md](docs/test-strategy.md) is the whole of it.

### Where the reference-independent part ends

> **CI proves the code is sound. Local runs prove it is faithful.**

Everything in that table passes on a machine that has never seen Serum 2, with the exceptions below.
These need the reference instrument (or its captured output) and **skip loudly** where it is absent —
a printed `SKIP`, never a silent pass:

| Reference-dependent | Where | Skips without |
|---|---|---|
| The factory-corpus walk — all 626 `.SerumPreset` files, each asserted to import and to produce a report | `plugin/tests/preset_test.cpp` (`[corpus]`), `tests/model/container_test.cpp` | the reference's content tree |
| The reference round-trip export test | `tests/model/reference_roundtrip_test.cpp` | the reference plugin and the host harness built alongside |
| The **fidelity suite** (ticket `46`) — xerum's own plugin rendered against **golden renders captured from the reference**, reported per fixture as null depth, RMS difference and spectral distance across swept sample rates and block sizes | the fidelity layer of `docs/test-strategy.md`; corpus in [tests/golden/manifest.json](tests/golden/manifest.json) | the reference plugin and the captured WAVs, which live outside the repository |
| The byte-identity half of the content gate — no shipped file may be identical to a reference file | `tools/check-content-provenance.py` | the reference's content tree |

No reference-derived artefact is committed: no factory presets, no wavetables, no captured renders.
The capture **script** ships (its output does not), so re-capturing is one command on a machine that
has a licensed reference — which is the point of the split (ticket `10`).

**The fidelity suite's numbers are a baseline, not a match.** xerum is an independent implementation
and every DSP ticket records its curves as *defensible implementations of documented behaviour, not
measurements of the reference* (tickets `25`, `26`, `29`, `37`). Sound-for-sound identity is not
claimed anywhere in this project, and the fidelity suite exists to measure the distance rather than to
assert it is zero.

### Plugin validation is a separate, manual step

`pluginval` and `auval` are not wired into the suite. Their last recorded results, with the exact
commands, are in [plugin/README.md](plugin/README.md): `pluginval` **SUCCESS at strictness level 10**
(the maximum) on both the VST3 and the AU, and `auval -strict -v aumu Xrm2 Xerm` →
**AU VALIDATION SUCCEEDED**, plus `auval -strict -stress 5`. Strictness is fixed at 10; relaxing it
requires a recorded reason.

A GitHub Actions workflow (`.github/workflows/ci.yml`) runs configure, build, a smoke check that the
host binary starts, and CTest on `macos-26`, and reports in the run summary which layers ran and which
were skipped.

## Limitations

These are where a user meets the honest edges of v1. They are stated here rather than left to be found
in a bug report.

**1. Real-time safety is *unmeasured*.** xerum's own allocation invariant is measured: **0 allocations**
across a 200-block render that includes note-on, more notes than the polyphony so a voice steal
happens, a sustain pedal, a pitch bend, a note-off and a parameter written between blocks — with a
counter whose positive control proves the hook works (`tests/dsp/voice_test.cpp`'s technique, reused by
the plugin suite). The audio thread takes no lock and lets no exception out, and
`auval -strict -stress 5` passes. But `auval -real-time-safety` — the check that would measure
real-time safety as auval means it — requires `sudo` and a DTrace entitlement; it produced no result
and was killed at a deadline. It is **unmeasured, not passed**, and no validator result here should be
read as covering it (ticket `30`, [plugin/README.md](plugin/README.md)).

**2. NOISE's dominant mode is not implemented.** The reference's NOISE oscillator is primarily a
stereo **sample player** — its own manual says so (p. 129) — and the four synthesised colours xerum
implements are the minority of its use. **Most presets that enable the NOISE slot name a sample file
instead** (208 of the 296 — ticket `40`). Those presets render **exact silence** in that slot and
report the file by name at `high`; substituting a colour would be precisely the silent substitution
the import contract forbids. `sampleFromAudioInput` is reported too, and is **impossible** in v1 for a
structural reason rather than an unfilled gap: the plugin is an instrument with no input bus, so there
is nothing to sample. The asset half of the slot belongs to the deferred sample pipeline (ticket `12`).

**3. Coverage is partial, and the gap is on screen.** The interface header prints the bound-control
count against the schema's leaf count, both computed at runtime from the parameter table and the
schema, so the number cannot drift from what is on screen (ticket `44`). It is not a large fraction.
The parameters xerum does not bind are **not** lost — they stay in the document, survive a state
round-trip, and an unknown parameter is reported at `medium` — but a host will not show a control for
them, and this is a v1 surface rather than the model.

**4. Defaults are only established where the machine could establish them.** The rule is
*absence means default* (ADR 0001). 118 of the schema's 776 leaves have an established default, read
from the reference's own init state on a machine that has it (ticket `38`); the table is generated
locally and is **not committed**, because it is derived data. Where nothing is established, the
resolved value is **xerum's own authored init**, never a plausible-looking zero, and `defaultFor`
returns `nullopt` rather than hiding the difference. The test suite asserts the *rule* (established →
use it, otherwise → xerum's init) rather than values, which is what lets it pass on a fresh clone.

**5. Named behaviours that are reported rather than reproduced.** A modulation row's *destination name*
is not editable — the address triple is what the engine reads, and inventing a name would be inventing
the address — and neither are the matrix's two curve knobs, though the engine does evaluate a preset's
own curves. A **warp whose name encodes a source or variant the engine does not route** is reported at
`info`: the family is applied, the source is not. `Global Polyphony` is applied at the next prepare or
preset load rather than on the knob turn, because re-preparing the voice pool allocates and that cannot
happen inside `processBlock`. And there is **no wavetable browser** — a slot's table is whatever the
preset names, or xerum's own sine when it names none. Each is reported, not hidden; ticket `30` and
[plugin/README.md](plugin/README.md) carry the list with its reason.

**6. It is not a reproduction of Serum 2.** The interface is xerum's own design (tickets `43`, `44`),
the AU identity is ours, and the project deliberately keeps every reference-derived artefact out of the
tree. Presets that lean on deferred engines will sound wrong, and the report is where you find out
which ones and why.

**7. v1 is unsigned and local.** No installer, no update path, no notarization, no code-signing
identity — the map holds all of that as fog. The package in the next section is a directory or a
tarball you copy yourself.

## Packaging an artefact for another machine

```sh
tools/package.sh --build-dir build --out dist
```

It stages `dist/xerum-<version>-macos-arm64/` containing:

```
AU/xerum.component            the AU, as built
VST3/xerum.vst3               the VST3, as built
Standalone/xerum.app          the standalone, as built
content/                      xerum's own tables + provenance manifest
LICENSE                       AGPLv3
THIRD_PARTY.md                what is linked, and under which licence
README.md                     this file
MANIFEST.sha256               sha256 of every shipped file, verified before the script exits
```

and, unless `--no-tar` is given, `dist/xerum-<version>-macos-arm64.tar.gz` with its sha256 printed.
The version comes from `CMakeLists.txt`'s `project(...)`, so the artefact is named after the build it
came from.

**The script checks what it stages rather than asserting it**, and exits non-zero if any check fails:

* each of the three bundles exists, has an `Info.plist` and an executable where a host looks for one;
  **a missing one is reported with the command that builds it, never worked around**;
* every bundle carries an **arm64** slice (`lipo -archs`);
* `LICENSE`, `THIRD_PARTY.md` and `README.md` are present and non-empty;
* the staged `content/` passes `tools/check-content-provenance.py`: every shipped file is declared in
  `content/manifest.json` with an origin and a licence, and — where the reference is installed — no
  shipped file is byte-identical to a reference file;
* **nothing reference-shaped is anywhere in the package**: the provenance gate walks the content root,
  so the script additionally scans the whole staged tree and refuses any `.SerumPreset`, `.fxp`,
  `.fxb`, `.flac`, `.sfz` or audio file outside `content/Tables/xerum/`;
* `MANIFEST.sha256` verifies against the tree it describes.

That is how "the packaged artefact contains the licences and everything needed to run, and no reference
content of any kind" (ticket `10`) is a check rather than a promise.

**The bundles link only system libraries and frameworks** — CoreAudio, CoreMIDI, AudioToolbox,
Accelerate, AppKit, Metal and the rest of the system frameworks, plus `libc++`, `libz` and
`libSystem` — because JUCE, the VST3 SDK, the AudioUnitSDK and the vendored zstd are all compiled into
the binary. There is no dylib to install alongside, which is why the artefact needs nothing but the
bundles and the content.

The build produces everything the package needs — all three bundles come out of `cmake --build`
complete, and the standalone needed nothing the build does not make. What it does **not** produce is
anything signed: the package is unsigned by decision, so a copy that travels as a download meets
Gatekeeper as described in [Install](#unsigned-what-gatekeeper-will-do).

## Repository layout

| Path | What it is |
|---|---|
| `src/model/` | Preset decode: containers, CBOR, the document, the import report. Pure C++, no JUCE — which is what keeps the format suite runnable anywhere |
| `src/dsp/` | The engine: oscillators, warps, filters, modulation, voices, the FX rack. No JUCE either |
| `src/content/` | Asset resolution: content roots, the case-insensitive rule, table loading |
| `plugin/` | The product plugin — AU, VST3 and standalone — and its own suite. [plugin/README.md](plugin/README.md) is its record |
| `host/` | The headless harness. [host/README.md](host/README.md) |
| `content/` | xerum's authored content library and its provenance manifest |
| `tools/` | Offline authoring tools and the release gates: `wavetable-author`, `check-content-provenance.py`, `render-preset.sh`, `package.sh` |
| `tests/` | The model, DSP and content suites, their fixtures, and the golden-render manifest |
| `docs/` | The normative contract ([preset-import-contract.md](docs/preset-import-contract.md)), the content policy, the test strategy, the ADRs |
| `third_party/zstd/` | Vendored zstd, decode-only |
| `GLOSSARY.md` | The project's own vocabulary, where a term is used in a precise sense |

## Licence and provenance

xerum is **AGPLv3**. Anyone who receives a binary is entitled to the corresponding source, including
users interacting with it over a network (AGPL §13). Contributions are accepted under the same terms.
[THIRD_PARTY.md](THIRD_PARTY.md) records what is linked and under which licence: JUCE 9.0.4 (AGPLv3 or
commercial — xerum takes the AGPLv3 option), the VST3 SDK (MIT) and the AudioUnitSDK (Apache-2.0) as
vendored inside JUCE, and Zstandard (BSD 3-Clause, at our option, vendored and decode-only). The
development tooling — REA, Ghidra, `zstd`, `lipo`, LLDB — is used and not linked into the product.

**The no-reference-content rule is enforced, not promised.** `docs/content-policy.md` states it;
`tools/check-content-provenance.py` implements it as a CTest gate (`content-provenance`), and
`tools/package.sh` runs the same gate against the packaged copy. Adding content without saying where it
came from fails the build, and a shipped file byte-identical to a reference file fails it too — the
check that would actually catch a mistake, run wherever the reference is installed and skipped loudly
where it is not.

Serum 2 and Serum are commercial products of Xfer Records. If you are looking for anything of Xfer's,
it is not here.
