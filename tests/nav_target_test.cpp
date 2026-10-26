#include "../src/game/nav_sequencer.h"
#include "check.h"
#include <cstdio>
#include <cstdint>

// menu_config.cpp is not linked here; the menu: target only reads its press count at run time.
extern "C" uint32_t rs64_menu_confirms() { return 0; }
extern "C" void rs64_menu_request_quit() {}
extern "C" void rs64_lang_original(const char* shown, char* out, size_t cap) { snprintf(out, cap, "%s", shown); }

int main() {
    RsNavTarget t;
    t = rs64_nav_parse("level:3");    CHECK(t.kind == NAV_LEVEL && t.a == 3 && t.b == -1);
    t = rs64_nav_parse("level:14,0"); CHECK(t.kind == NAV_LEVEL && t.a == 14 && t.b == 0);
    t = rs64_nav_parse("level:3");    CHECK(t.c == -1);
    t = rs64_nav_parse("hangar:3,2"); CHECK(t.kind == NAV_ABORT && t.a == 3 && t.b == 2 && t.c == 1);
    t = rs64_nav_parse("hangar:1");   CHECK(t.kind == NAV_ABORT && t.a == 1 && t.b == -1 && t.c == 1);
    t = rs64_nav_parse("menu:OPTIONS/MENU PROBE/RUN ACTION"); CHECK(t.kind == NAV_MENU && t.a == 3);
    t = rs64_nav_parse("menu");       CHECK(t.kind == NAV_NONE);
    t = rs64_nav_parse("menu:");      CHECK(t.kind == NAV_NONE);
    t = rs64_nav_parse("cutscene:5"); CHECK(t.kind == NAV_CUTSCENE && t.a == 5);
    t = rs64_nav_parse("cutscene:15,1"); CHECK(t.kind == NAV_CUTSCENE && t.a == 15 && t.b == 1 && t.c == -1);
    t = rs64_nav_parse("cutscene:3,2,3"); CHECK(t.kind == NAV_CUTSCENE && t.a == 3 && t.b == 2 && t.c == 3);
    t = rs64_nav_parse("cutscene:3"); CHECK(t.b == 0);
    t = rs64_nav_parse("demo:2");     CHECK(t.kind == NAV_DEMO && t.a == 2);
    t = rs64_nav_parse("demo");       CHECK(t.kind == NAV_DEMO && t.a == 0);
    t = rs64_nav_parse("abort:0");    CHECK(t.kind == NAV_ABORT && t.a == 0 && t.b == -1);
    t = rs64_nav_parse("abort:2,1");  CHECK(t.kind == NAV_ABORT && t.a == 2 && t.b == 1);
    t = rs64_nav_parse("pause:0");    CHECK(t.kind == NAV_ABORT && t.a == 0 && t.c == 10);
    t = rs64_nav_parse("pause:3,1");  CHECK(t.kind == NAV_ABORT && t.a == 3 && t.b == -1 && t.c == 11);
    t = rs64_nav_parse("lobby:host,0");  CHECK(t.kind == NAV_LOBBY && t.b == 0 && t.a == 0);
    t = rs64_nav_parse("lobby:join,3");  CHECK(t.kind == NAV_LOBBY && t.b == 1 && t.a == 3);
    t = rs64_nav_parse("lobby:page");    CHECK(t.kind == NAV_LOBBY && t.b == 2);
    t = rs64_nav_parse("lobby:join");    CHECK(t.kind == NAV_LOBBY && t.b == 1 && t.a == 0);
    t = rs64_nav_parse("lobby:spy,1");   CHECK(t.kind == NAV_NONE);
    t = rs64_nav_parse("lobby:join,2,1"); CHECK(t.kind == NAV_LOBBY && t.a == 2 && t.b == 1 && t.c == 1);
    t = rs64_nav_parse("lobby:host,2");  CHECK(t.c == -1);
    t = rs64_nav_parse("menu");      CHECK(t.kind == NAV_NONE);
    t = rs64_nav_parse(nullptr);      CHECK(t.kind == NAV_NONE);
    printf("nav_target_test OK\n");
    return 0;
}
