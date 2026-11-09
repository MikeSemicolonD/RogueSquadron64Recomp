// A web page writes these links, so anything but what the Explorer sends must be refused.
#include "main/cli.h"

#include <cstdio>

using rs64::cli::play_link_target;

static int s_failed = 0;

static void expect(const char* url, bool valid, const char* wanted = "")
{

    std::string target = "untouched";
    const bool got = play_link_target(url, target);

    if (got != valid || (valid && target != wanted))
    {
        fprintf(stderr, "FAIL %s: got %d \"%s\"\n", url, got, target.c_str());
        ++s_failed;
    }
}

int main()
{

    expect("rs64recomp:", true);
    expect("rs64recomp:level:5", true, "level:5");
    expect("rs64recomp:level:5/", true, "level:5");
    expect("rs64recomp:level:new_level_2", true, "level:new_level_2");
    expect("rs64recomp:cutscene:0,1", true, "cutscene:0,1");
    expect("rs64recomp:cutscene:new_level,0", true, "cutscene:new_level,0");

    expect("", false);
    expect("rs64recomp", false);
    expect("http://rs64recomp:level:5", false);
    expect("rs64recomp:level:", false);
    expect("rs64recomp:level:5,2", false);
    expect("rs64recomp:level:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", false);
    expect("rs64recomp:level:-1", false);
    expect("rs64recomp:level:a/b", false);
    expect("rs64recomp:level:a%20b", false);
    expect("rs64recomp:cutscene:new_level,x", false);
    expect("rs64recomp:level:5 --set", false);
    expect("rs64recomp:level:5\" --set ROGUESQ_X=1", false);
    expect("rs64recomp:cutscene:1", false);
    expect("rs64recomp:cutscene:1,", false);
    expect("rs64recomp:cutscene:1,2,3", false);
    expect("rs64recomp:menu:A/B", false);
    expect("rs64recomp:demo:1", false);
    expect("rs64recomp:--set", false);

    if (s_failed)
    {
        return 1;
    }

    fprintf(stderr, "play_link_test ok\n");

    return 0;
}
