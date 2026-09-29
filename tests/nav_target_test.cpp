#include "../src/main/nav_sequencer.h"
#include <cassert>
#include <cstdio>

int main() {
    RsNavTarget t;
    t = rs64_nav_parse("level:3");    assert(t.kind == NAV_LEVEL && t.a == 3 && t.b == -1);
    t = rs64_nav_parse("level:14,0"); assert(t.kind == NAV_LEVEL && t.a == 14 && t.b == 0);
    t = rs64_nav_parse("cutscene:5"); assert(t.kind == NAV_CUTSCENE && t.a == 5);
    t = rs64_nav_parse("demo:2");     assert(t.kind == NAV_DEMO && t.a == 2);
    t = rs64_nav_parse("demo");       assert(t.kind == NAV_DEMO && t.a == 0);
    t = rs64_nav_parse("abort:0");    assert(t.kind == NAV_ABORT && t.a == 0 && t.b == -1);
    t = rs64_nav_parse("abort:2,1");  assert(t.kind == NAV_ABORT && t.a == 2 && t.b == 1);
    t = rs64_nav_parse("menu");      assert(t.kind == NAV_NONE);
    t = rs64_nav_parse(nullptr);      assert(t.kind == NAV_NONE);
    printf("nav_target_test OK\n");
    return 0;
}
