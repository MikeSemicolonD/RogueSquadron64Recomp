// The multiplayer code's only route to the game: the host API table, the same in the exe and in the multiplayer-native mod.
#pragma once
#include "rs64/host_api.h"
#include "recomp.h"
#include <cstdint>
#include <initializer_list>

namespace rs64::mp {

const rs64_host_api* api();

struct Call {
    uint32_t v0 = 0;
    float f0 = 0.0f;
};

// As rs64::mips::mips_call: a0-a3 = args, stack words at sp+0x10.., f12 / f14; every register is restored after.
// Unlisted a0-a3 are passed as zero (unlike rs64::mips::mips_call, which leaves them as the hook site had them).
inline Call call(uint8_t* rdram, recomp_context* ctx, uint32_t vram, std::initializer_list<uint32_t> args, std::initializer_list<uint32_t> stack = {}, float f12 = 0.0f, float f14 = 0.0f) {
    Call c;
    c.v0 = api()->call(rdram, ctx, vram, args.begin(), (uint32_t)args.size(), stack.begin(), (uint32_t)stack.size(), f12, f14, &c.f0);
    return c;
}
inline uint32_t alloc(uint8_t* rdram, uint32_t size) { return api()->alloc(rdram, size); }
inline const char* state_id() { return api()->state_id(); }
inline int in_cutscene() { return api()->in_cutscene(); }
inline uint32_t cutscene_skips() { return api()->cutscene_skips(); }
inline void yield(uint8_t* rdram, recomp_context* ctx) { api()->yield(rdram, ctx); }
inline void request_quit() { api()->request_quit(); }
inline void text_input(bool on) { api()->text_input(on ? 1 : 0); }
inline int screen_keyboard_shown() { return api()->screen_keyboard_shown(); }
inline bool any_key_held() { return api()->any_key_held() != 0; }
inline const char* base_path() { return api()->base_path(); }
inline void log(const char* line) { api()->log(line); }
inline const void* service(const char* name) { return api()->get_service(name); }

inline void set_flag(const char* name, int value) { api()->set_flag(name, value); }
inline int flag(const char* name) { return api()->get_flag(name); }
inline int add_hook(uint32_t hook, rs64_hook_fn fn, void* user = nullptr) { return api()->add_hook(hook, fn, user); }
inline int add_text_source(const char* name, rs64_text_fn fn, void* user = nullptr) { return api()->add_text_source(name, fn, user); }
inline int add_input_filter(rs64_input_fn fn, void* user = nullptr) { return api()->add_input_filter(fn, user); }
inline int add_action(const char* name, rs64_action_fn fn, void* user = nullptr) { return api()->add_action(name, fn, user); }
inline int add_condition(const char* name, rs64_condition_fn fn, void* user = nullptr) { return api()->add_condition(name, fn, user); }
inline int add_key_handler(rs64_key_fn fn, void* user = nullptr) { return api()->add_key_handler(fn, user); }
inline int add_quit_handler(rs64_event_fn fn, void* user = nullptr) { return api()->add_quit_handler(fn, user); }

}
