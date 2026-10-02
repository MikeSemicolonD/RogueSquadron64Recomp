#include "../src/main/nav_sequencer.h"
#include "check.h"
#include <cstdio>

int main() {
    RsNavTarget t;
    t = rs64_nav_parse("level:3");    CHECK(t.kind == NAV_LEVEL && t.a == 3 && t.b == -1);
    t = rs64_nav_parse("level:14,0"); CHECK(t.kind == NAV_LEVEL && t.a == 14 && t.b == 0);
    t = rs64_nav_parse("cutscene:5"); CHECK(t.kind == NAV_CUTSCENE && t.a == 5);
    t = rs64_nav_parse("cutscene:15,1"); CHECK(t.kind == NAV_CUTSCENE && t.a == 15 && t.b == 1);
    t = rs64_nav_parse("cutscene:3"); CHECK(t.b == 0);
    t = rs64_nav_parse("demo:2");     CHECK(t.kind == NAV_DEMO && t.a == 2);
    t = rs64_nav_parse("demo");       CHECK(t.kind == NAV_DEMO && t.a == 0);
    t = rs64_nav_parse("abort:0");    CHECK(t.kind == NAV_ABORT && t.a == 0 && t.b == -1);
    t = rs64_nav_parse("abort:2,1");  CHECK(t.kind == NAV_ABORT && t.a == 2 && t.b == 1);
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
