#include "shim.h"

// The single include that pulls in the whole decode-only library. Everything below this line is
// vendored code; nothing of ours runs inside it.
#include "zstddeclib.c"

static char g_lastError[256] = "ok";

size_t xerum_zstd_decompress (void* dst, size_t dstCapacity, const void* src, size_t srcSize)
{
    const size_t got = ZSTD_decompress (dst, dstCapacity, src, srcSize);

    if (ZSTD_isError (got))
    {
        const char* const name = ZSTD_getErrorName (got);
        size_t i = 0;

        for (; name[i] != '\0' && i < sizeof (g_lastError) - 1; ++i)
            g_lastError[i] = name[i];

        g_lastError[i] = '\0';
        return 0;
    }

    return got;
}

const char* xerum_zstd_error (void) { return g_lastError; }

const char* xerum_zstd_version (void) { return ZSTD_versionString(); }
