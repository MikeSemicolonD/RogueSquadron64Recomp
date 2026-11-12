#ifndef RS64_RECOMP_IMPORTS_H
#define RS64_RECOMP_IMPORTS_H

// Force-included into RecompiledFuncs and PatchesLib: prototypes for functions the generated C calls without declaring.
// Implicit declarations return int; WebAssembly checks call signatures, so those calls to void functions trap there.

#include "recomp.h"
#include "../RecompiledFuncs/funcs.h"

#ifdef __cplusplus
extern "C" {
#endif

void osYieldThread_recomp(uint8_t* rdram, recomp_context* ctx);
void osViGetCurrentField_recomp(uint8_t* rdram, recomp_context* ctx);
void osDpGetCounters_recomp(uint8_t* rdram, recomp_context* ctx);
void osDpSetStatus_recomp(uint8_t* rdram, recomp_context* ctx);
void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx);
void __osPfsSelectBank_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
