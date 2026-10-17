// MH-Forever: logging hook for the extracted FFmpeg decoders (replaces PPSSPP's compat.cpp).
// Only errors are printed, to stderr.

#include <cstdarg>
#include <cstdio>

#include "compat.h"

void av_log(int level, const char *fmt, ...) {
    if (level > AV_LOG_ERROR) return;
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    std::fprintf(stderr, "[atrac] %s", buffer);
}
