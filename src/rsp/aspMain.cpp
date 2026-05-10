#include "librecomp/rsp.hpp"
#include "ultramodern/ultra64.h"

// aspMain — software implementation of the N64 audio RSP microcode.
// Stubbed (returns Broke). Audio bring-up is a separate project; this stub
// keeps the link clean for any code that still references the symbol.
RspExitReason aspMain(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}
