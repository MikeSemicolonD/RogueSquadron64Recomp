// recomp_printf for mods (recomputils.h): formats with the game's own libultra _Printf into a buffer, then the host prints it.
#include "rs64_api.h"

typedef unsigned int size_t;

// libultra: _Printf calls prout(arg, chunk, len) for each piece and returns the full length, or a negative value on error; a NULL return from prout aborts.
int _Printf(char* (*prout)(char*, const char*, size_t), char* arg, const char* fmt, __builtin_va_list ap);

// Host side (register_patches.cpp), reached through the manual patch symbol in patches/syms.ld.
void rs64_host_puts(const char* text);

static char print_buffer[2048];
static unsigned int print_len;

// Appends what fits and drops the rest, so a long message is truncated instead of overrunning the buffer. The game's _Printf calls this through the func_map, which holds the patch functions.
static char* prout_bounded(char* arg, const char* src, size_t count) {
    for (size_t i = 0; i < count && print_len < sizeof(print_buffer) - 1; i++) {
        print_buffer[print_len++] = src[i];
    }
    return arg;
}

RECOMP_EXPORT int recomp_printf(const char* fmt, ...) {
    __builtin_va_list ap;
    print_len = 0;
    __builtin_va_start(ap, fmt);
    int len = _Printf(prout_bounded, print_buffer, fmt, ap);
    __builtin_va_end(ap);
    if (len >= 0) {
        print_buffer[print_len] = '\0';
        rs64_host_puts(print_buffer);
    }
    return len;
}
