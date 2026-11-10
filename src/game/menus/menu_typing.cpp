// Keyboard typing on the letter wheels (ENTER NAME, PASSCODES): text queued on the SDL thread, entered by the wheel hooks through the game's own A/B path.
#include "game/menus/menu_config.h"
#include "game/menus/menu_config_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>

#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

#include "recomp.h"
#include "rs64_toml_hooks.h"
#include "util/mips_call.h"

using namespace rs64::menucfg;

static std::mutex s_typed_mtx;
static std::string s_typed;
// 0 = no wheel, 1 = ENTER NAME, 2 = PASSCODES (Enter submits the code).
static std::atomic<int> s_typing{0};
static int s_wheel_seen = 0;

// A wheel hook ran on the previous pass: typing stays on (on a phone this raises the keyboard).
void rs64::menucfg::typing_pass()
{

    const int typing = s_wheel_seen;

    s_wheel_seen = 0;

    if (typing == s_typing.load())
    {
        return;
    }

    s_typing.store(typing);

    if (typing) {
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }

    if (!typing) {
        std::lock_guard<std::mutex> lk(s_typed_mtx);
        s_typed.clear();
    }
}

extern "C" int rs64_menu_typing(void) {
    return s_typing.load();
}

extern "C" int rs64_menu_type(const char* utf8, int key)
{

    const int typing = s_typing.load();

    if (!typing || (!utf8 && key != '\b' && !(key == '\r' && typing == 2)))
    {
        return 0;
    }

    std::lock_guard<std::mutex> lk(s_typed_mtx);

    if (!utf8) {
        s_typed.push_back((char)key);
        return 1;
    }

    for (const char* p = utf8; *p && s_typed.size() < 16; ++p) {
        s_typed.push_back((char)std::toupper((unsigned char)*p));
    }

    return 1;
}

// Next typed key: its index in the wheel's alphabet (0x800CCBF0 -> "ABC...Z!?&"), -2 for Backspace, -1 for none; other characters are dropped.
static int pop_typed(uint8_t* rdram)
{

    const uint32_t abc = (uint32_t)rd_w(rdram, 0x800CCBF0u - 0x80000000u) - 0x80000000u;
    std::lock_guard<std::mutex> lk(s_typed_mtx);

    while (!s_typed.empty()) {
        const char c = s_typed.front();

        // Enter stays queued for rs64_passcode_row.
        if (c == '\r')
        {
            return -1;
        }

        s_typed.erase(0, 1);

        if (c == '\b')
        {
            return -2;
        }

        for (int i = 0; abc < 0x800000u && i < 32 && rd_b(rdram, abc + i); ++i) {
            if (rd_b(rdram, abc + i) == (uint8_t)c)
            {
                return i;
            }
        }
    }

    return -1;
}

// Right after the wheel call ($v0 = highlighted letter, $fp = port): a typed letter replaces $v0 and presses A; Backspace presses B.
extern "C" void rs64_wheel_typed(uint8_t* rdram, recomp_context* ctx)
{

    s_wheel_seen = std::max(s_wheel_seen, 1);

    const uint32_t wheel =(uint32_t)rd_w(rdram, 0x800CF258u - 0x80000000u) - 0x80000000u;
    const int32_t spin = rd_w(rdram, wheel + 0x14EC);

    // The game enters a letter only with the wheel at rest (spin speed 0.0f); the key waits.
    if (wheel >= 0x800000u || (spin & 0x7FFFFFFF) != 0) return;

    const int c = pop_typed(rdram);

    if (c == -1)
    {
        return;
    }

    const uint32_t pressed = 0x8013A960u - 0x80000000u + (uint32_t)(ctx->r30 & 0xFF) * 4;

    if (c >= 0) {
        ctx->r2 = c;
    }

    wr_w(rdram, pressed, rd_w(rdram, pressed) | (c >= 0 ? 0x8000 : 0x4000));
}

// PASSCODES row check ($s4 = menu, +0x94 = cursor, the wheel is row 0, ENTER CODE row 1): a typed key moves the cursor to the wheel, as an up/down move would; Enter, once the letters before it are in, selects ENTER CODE and presses A.
extern "C" void rs64_passcode_row(uint8_t* rdram, recomp_context* ctx)
{

    s_wheel_seen = 2;

    const uint32_t menu = (uint32_t)ctx->r20 - 0x80000000u;
    bool submit = false;
    std::unique_lock<std::mutex> lk(s_typed_mtx);

    if (!s_typed.empty() && s_typed.front() == '\r') {
        s_typed.erase(0, 1);
        submit = true;
    }

    const bool pending = !s_typed.empty();

    lk.unlock();

    if (submit) {
        const uint32_t pressed = 0x8013A960u - 0x80000000u + (uint32_t)(ctx->r30 & 0xFF) * 4;

        wr_b(rdram, menu + 0x94, 1);
        wr_w(rdram, pressed, rd_w(rdram, pressed) | 0x8000);

        return;
    }

    if (!pending || rd_b(rdram, menu + 0x94) == 0) return;

    wr_b(rdram, menu + 0x94, 0);
    rs64::mips::mips_call(rdram, ctx, 0x80087EE8u, { 0x17, 0x7F, 0x40 });
    wr_w(rdram, menu + 0x9C, 0);
    wr_w(rdram, menu + 0xA0, rd_w(rdram, 0x800A66F0u - 0x80000000u));
}
