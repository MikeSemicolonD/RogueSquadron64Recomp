#ifndef RECOMP_GAME_H
#define RECOMP_GAME_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_GAME_H */
