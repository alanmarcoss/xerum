# zstd — vendored decoder

A single-file, decode-only build of [Zstandard](https://github.com/facebook/zstd), vendored because
**macOS has no zstd to link against**: the SDK ships zlib, LZMA, LZ4 and LZFSE through the Compression
framework, but not zstd, and every `.SerumPreset` in existence is a zstd frame. Without this, xerum
cannot read a single preset.

## Provenance

| | |
|---|---|
| Upstream | `facebook/zstd` |
| Version | **1.5.7** |
| Tag | `v1.5.7` |
| Commit | `f8745da6ff1ad1e7bab384bd1f9d742439278e99` |
| Generator | `build/single_file_libs/create_single_file_decoder.sh`, run inside that checkout |
| Output | `zstddeclib.c` |
| SHA-256 | `5eb38abe0a7eea13674b312a006df72002d4192f0f0b4d09e6f03fb927a1f3d0` |
| Licence | **BSD 3-Clause**, or GPLv2 at your option — see [LICENSE](LICENSE) |

Generated files are not committed upstream, so this is the output of their own generator rather than a
hand-assembled tree. To update: check out the new tag, run that script, copy the result, and record the
new hash and commit here. Anything hand-edited would be silently lost at the next update, so **regenerate
rather than patch**.

Only the decoder is included. `create_single_file_decoder.sh` produces a build without the compressor,
which is the half we need and roughly half the size of the full library.

## Why a generated single file

The alternative was vendoring `lib/common/*.c` and `lib/decompress/*.c` — a dozen files plus headers —
or depending on a Homebrew install, which would break the rule that a fresh clone builds. One generated
file with a recorded hash and generator command is the smallest thing that keeps the build hermetic and
the review surface honest: everything in it came from one upstream commit, verifiable by re-running the
generator.

## Verified

Decoding a real factory preset's payload with this file reproduces exactly the right bytes — 16,892
bytes out of a 3,358-byte frame, beginning `b8 af` (a CBOR map of 175 entries) — matching an
independent decode done with the `zstd` command-line tool. The test suite asserts the same thing on a
committed fixture, so it stays verified on a machine with no reference install.
