// Proves the patches pipeline end to end: mods can import it from "*" and get the constant back.
#include "rs64_api.h"

RECOMP_EXPORT unsigned int rs64_patches_sanity(void) {
    return 0x52533634;
}
