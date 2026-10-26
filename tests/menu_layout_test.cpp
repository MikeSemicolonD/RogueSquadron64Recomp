#include "game/menu_layout.h"
#include "check.h"
#include <cstdio>
#include <vector>

int main() {

    // resolve_index_clamps_and_counts_from_end
    CHECK(rs64::menu::resolve_index(0, 5) == 0);
    CHECK(rs64::menu::resolve_index(-1, 5) == 4);
    CHECK(rs64::menu::resolve_index(9, 5) == 4);
    CHECK(rs64::menu::resolve_index(-20, 5) == 0);

    // move_to_index_reorders_in_place
    std::vector<int> v{10, 20, 30, 40};

    rs64::menu::move_to_index(v, 3, 1);
    CHECK((v == std::vector<int>{10, 40, 20, 30}));
    rs64::menu::move_to_index(v, 0, -1);
    CHECK((v == std::vector<int>{40, 20, 30, 10}));
    rs64::menu::move_to_index(v, 2, 99);
    CHECK((v == std::vector<int>{40, 20, 10, 30}));

    // to_latin1_maps_utf8_to_font_bytes
    CHECK(rs64::menu::to_latin1("BACK") == "BACK");
    CHECK(rs64::menu::to_latin1("ZUR\xC3\x9C" "CK") == "ZUR\xDC" "CK");
    CHECK(rs64::menu::to_latin1("A\xE2\x82\xAC" "B") == "A?B");
    CHECK(rs64::menu::to_latin1("\xC3") == "?");

    // Scroll window over 22 rows, 7 visible; sel == total is BACK.
    using rs64::menu::ScrollPos;
    using rs64::menu::scroll_step;
    auto eq = [](ScrollPos p, int sel, int top) { return p.sel == sel && p.top == top; };

    // scroll_down_at_window_edge_moves_top
    CHECK(eq(scroll_step({5, 0}, 1, 22, 7), 6, 0));
    CHECK(eq(scroll_step({6, 0}, 1, 22, 7), 7, 1));
    CHECK(eq(scroll_step({7, 1}, -1, 22, 7), 6, 1));
    CHECK(eq(scroll_step({1, 1}, -1, 22, 7), 0, 0));

    // scroll_down_from_last_row_reaches_back
    CHECK(eq(scroll_step({21, 15}, 1, 22, 7), 22, 15));

    // scroll_wraps_back_to_top
    CHECK(eq(scroll_step({22, 15}, 1, 22, 7), 0, 0));

    // scroll_up_from_first_reaches_back
    CHECK(eq(scroll_step({0, 0}, -1, 22, 7), 22, 0));

    // scroll_up_from_back_shows_end
    CHECK(eq(scroll_step({22, 0}, -1, 22, 7), 21, 15));

    // scroll_short_list_never_scrolls
    CHECK(eq(scroll_step({4, 0}, 1, 5, 7), 5, 0));
    CHECK(eq(scroll_step({5, 0}, -1, 5, 7), 4, 0));
    CHECK(eq(scroll_step({5, 0}, 1, 5, 7), 0, 0));

    // native_aliases_match_the_captured_menus
    {
        using rs64::menu::native_alias;

        auto is = [](const rs64::menu::NativeAlias* a, uint8_t sub, int32_t param, bool match) { return a && a->sub == sub && a->param == param && a->match_param == match; };

        CHECK(is(native_alias("main_menu", "start"), 1, 1, true));
        CHECK(is(native_alias("options", "biographies"), 1, 8, true));
        CHECK(is(native_alias("options", "elite_rogues"), 1, 4, true));
        CHECK(is(native_alias("options", "passcodes"), 1, 7, true));
        CHECK(is(native_alias("options", "game_settings"), 1, 3, true));
        CHECK(is(native_alias("options", "sound_settings"), 1, 6, true));
        CHECK(is(native_alias("options", "controller_settings"), 1, 5, true));
        CHECK(is(native_alias("options", "back"), 25, 0, false));
        CHECK(is(native_alias("game_settings", "high_resolution"), 5, 4, true));
        CHECK(is(native_alias("controller_settings", "back"), 25, 0, false));
        CHECK(native_alias("options", "nope") == nullptr && native_alias("pause", "back") == nullptr);
    }

    // pause_aliases_name_text_ids
    {
        using rs64::menu::pause_native_alias;

        CHECK(pause_native_alias("auto_roll") == 0x25 && pause_native_alias("auto_level") == 0x26 && pause_native_alias("free_camera") == 0x28);
        CHECK(pause_native_alias("crosshairs") == 0x27 && pause_native_alias("displays") == 0x2B && pause_native_alias("cockpit") == 0x2A);
        CHECK(pause_native_alias("back") == 0x39 && pause_native_alias("nope") == -1);
        CHECK(pause_native_alias("continue") == 0x21 && pause_native_alias("abort_level") == 0x22);
        CHECK(pause_native_alias("game_settings") == 0x2E && pause_native_alias("sound_settings") == 0x2F);
    }

    // conditions_name_the_buttons_own_mod_and_negate
    {
        using rs64::menu::condition_key;

        CHECK(condition_key("config:fast", "mymod") == "config:mymod:fast");
        CHECK(condition_key("!config:fast", "mymod") == "!config:mymod:fast");
        CHECK(condition_key("config:other:opt", "mymod") == "config:other:opt");
        CHECK(condition_key("!desktop", "mymod") == "!desktop" && condition_key("desktop", "") == "desktop");
        CHECK(condition_key("config:fast", "") == "config:fast");
    }

    std::printf("menu_layout_test: ok\n");

    return 0;
}
