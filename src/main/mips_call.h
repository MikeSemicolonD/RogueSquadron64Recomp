#pragma once
// Host-side calls into recompiled functions from inside a hook.
#include "rdram_words.h"
#include "recomp.h"
#include <cstdint>
#include <initializer_list>

namespace rs64::mips {

struct Call {
    uint32_t v0 = 0;
    float f0 = 0.0f;
};

// Calls the recompiled function at vram with a0-a3 = args and stack words at sp+0x10.., below the hook site's frame; every register is restored after.
inline Call mips_call_n(uint8_t* rdram, recomp_context* ctx, uint32_t vram, const uint32_t* args, uint32_t nargs, const uint32_t* stack, uint32_t nstack, float f12, float f14) {
    const recomp_context saved = *ctx;
    ctx->r29 = (gpr)(int32_t)((uint32_t)ctx->r29 - 0x40);
    gpr* regs[4] = {&ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7};
    for (uint32_t i = 0; i < nargs && i < 4; ++i) {
        *regs[i] = (gpr)(int32_t)args[i];
    }
    for (uint32_t i = 0; i < nstack && i < 12; ++i) {
        ww(rdram, (uint32_t)ctx->r29 + 0x10 + 4 * i, stack[i]);
    }
    ctx->f12.fl = f12;
    ctx->f14.fl = f14;
    get_function((int32_t)vram)(rdram, ctx);
    Call out;
    out.v0 = (uint32_t)ctx->r2;
    out.f0 = ctx->f0.fl;
    *ctx = saved;
    return out;
}

inline Call mips_call(uint8_t* rdram, recomp_context* ctx, uint32_t vram, std::initializer_list<uint32_t> args, std::initializer_list<uint32_t> stack = {}, float f12 = 0.0f, float f14 = 0.0f) {
    return mips_call_n(rdram, ctx, vram, args.begin(), (uint32_t)args.size(), stack.begin(), (uint32_t)stack.size(), f12, f14);
}

}
