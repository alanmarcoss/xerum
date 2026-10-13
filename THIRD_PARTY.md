# Third-party notices

xerum is licensed under the **GNU Affero General Public License v3.0** — see [LICENSE](LICENSE).
This file records what we link and what we merely use, as ticket `11` requires.

## Linked at build time

| Component | Version | Licence | Notes |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.4, pinned by commit | AGPLv3 **or** commercial | xerum takes the AGPLv3 option. Fetched by CMake at configure time, not vendored into the tree. |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) | 3.8.0, vendored inside JUCE | MIT | © Steinberg Media Technologies GmbH. |
| Apple AudioUnitSDK | 1.1.0, vendored inside JUCE | Apache-2.0 | macOS AU support. |

Because xerum is AGPLv3 and JUCE is used under its AGPLv3 option, the combined work is AGPLv3:
**anyone who receives a binary is entitled to the corresponding source**, including users interacting
with it over a network (AGPL §13). Contributions are accepted under the same terms.

## Development tooling — used, not linked into the product

| Tool | Licence | Used for |
|---|---|---|
| [REA](https://rea.tools/) (`rea-agents`) | MIT | reverse-engineering harness and evidence capture (ticket `07`) |
| [Ghidra](https://ghidra-sre.org/) | Apache-2.0 | static-analysis provider for REA |
| `zstd`, `lipo`, LLDB, Homebrew JDK 21 | BSD / Apple / GPL+CE | local analysis and builds |

## Reverse-engineering note

Serum 2 and Serum are commercial products of Xfer Records. **This project is not affiliated with,
endorsed by, or sponsored by Xfer Records.** xerum is an independent implementation; its
preset-format knowledge was derived for interoperability, and the legal posture behind that is
recorded in `.scratch/xerum/issues/10-licensed-reference-install.md`.

This repository contains **no Xfer code, presets, wavetables, multisamples or audio renders**, and
none may be added: reference artefacts stay outside the repo, and tests that need them resolve them
by path and skip when absent. If you are looking for anything of Xfer's, it is not here.
