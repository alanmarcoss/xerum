// A shim so xerum never guesses zstd's ABI.
//
// The vendored `zstddeclib.c` is self-contained: it carries its own declarations and is meant to be
// included, not linked against a header we wrote by hand. Declaring `ZSTD_decompress` ourselves would
// work until the next regeneration changed a signature, and the failure would be a link error or worse.
//
// So the one translation unit that includes the vendored file also exposes the tiny surface xerum
// actually uses. If zstd is updated, `shim.c` fails to compile and says so, which is the right place
// for that to happen.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Decompress `srcSize` bytes into `dstCapacity`. Returns the number of bytes written, or 0 on error.
/// On error, `xerum_zstd_error` describes it.
size_t xerum_zstd_decompress (void* dst, size_t dstCapacity, const void* src, size_t srcSize);

/// A human-readable description of the last `xerum_zstd_decompress` failure. Never null.
const char* xerum_zstd_error (void);

/// The vendored library's own version string, handy in diagnostics and asserted by a test so an
/// accidental regeneration from a different release is noticed rather than assumed harmless.
const char* xerum_zstd_version (void);

#ifdef __cplusplus
}
#endif
