// The multiplayer-native mod: online co-op as a library that reaches the game only through the rs64 host API.
#include "rs64/host_api.h"
#include "recomp.h"
#include <cstdint>

#ifdef _WIN32
#define MOD_VISIBLE __declspec(dllexport)
#else
#define MOD_VISIBLE __attribute__((visibility("default")))
#endif
#define MOD_EXPORT extern "C" MOD_VISIBLE

extern "C" void rs64_mp_register(const rs64_host_api* api);

// librecomp checks this on load; must be 1.
extern "C" {
MOD_VISIBLE uint32_t recomp_api_version = 1;
}

// r4 = the host API table, r5 = the version the host implements; r2 = 0 accepts, r3 = the version this mod was built against.
MOD_EXPORT void rs64_mod_init(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const rs64_host_api* api = (const rs64_host_api*)(uintptr_t)ctx->r4;
    ctx->r3 = RS64_HOST_API_VERSION;
    if ((uint32_t)ctx->r5 < RS64_HOST_API_VERSION || !api || api->size < sizeof(rs64_host_api)) {
        ctx->r2 = 1;
        return;
    }
    // A game with multiplayer built in has registered it already; a second copy would answer every hook twice.
    if (api->get_flag("mp_builtin")) {
        api->log("[multiplayer] built into this game; the multiplayer-native mod stays off");
        ctx->r2 = 1;
        return;
    }
    rs64_mp_register(api);
    api->log("[multiplayer] native mod active");
    ctx->r2 = 0;
}
