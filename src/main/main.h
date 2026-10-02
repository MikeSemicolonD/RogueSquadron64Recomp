// main.h — the public C-linkage surface of main.cpp consumed by other host
// TUs (window/fullscreen, quit, touch layout, throttle and the reloc guard).
#ifndef RS64_MAIN_H
#define RS64_MAIN_H

#include <cstdint>

extern "C" {
    int  rs64_get_fullscreen(void);
    void rs64_toggle_fullscreen(void);
    void rs64_menu_request_quit(void);
    void rs64_touch_layout_request(void);
    float rs64_throttle_live(void);
    float rs64_throttle_cruise_live(void);
    float rs64_ls_throttle(float live);
    float rs64_ls_cruise(float live);
    void rs64_reloc_guard_tick(uint8_t* rdram);
}

#endif // RS64_MAIN_H
