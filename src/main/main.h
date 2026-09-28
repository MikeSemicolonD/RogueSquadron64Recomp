// main.h — the public C-linkage surface of main.cpp consumed by other host
// TUs (the window/fullscreen + quit hooks used by menu_config.cpp).
#ifndef RS64_MAIN_H
#define RS64_MAIN_H

extern "C" {
    int  rs64_get_fullscreen(void);
    void rs64_toggle_fullscreen(void);
    void rs64_menu_request_quit(void);
    void rs64_touch_layout_request(void);
}

#endif // RS64_MAIN_H
