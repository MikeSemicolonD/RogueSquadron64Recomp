#include "game/menus/mods_page.h"
#include "check.h"
#include <cstdio>
#include <string>
#include <utility>

using namespace rs64::mods_page;

static List make(int n)
{

    List l;

    for (int i = 0; i < n; ++i) l.mods.push_back({ "m" + std::to_string(i), "Mod " + std::to_string(i), "Desc " + std::to_string(i), true, false });

    return l;
}

int main() {

    // empty_list_has_no_rows
    {
        List l = make(0);

        CHECK(window(l, 5).count == 0);
        CHECK(carry_begin(l) == -1);
        CHECK(description_text(l, Words{}) == "NO MODS INSTALLED");
    }

    // window_arrows_at_capacity
    {
        List l = make(5);
        Window w = window(l, 5);

        CHECK(w.count == 5 && !w.more_above && !w.more_below);

        List m = make(6);
        Window v = window(m, 5);

        CHECK(v.count == 5 && v.more_below && !v.more_above);
    }

    // carry_moves_and_scrolls
    {
        List l = make(8);

        l.pos = { 4, 0 };
        CHECK(carry_begin(l) == 4);
        carry_step(l, 1, 5);
        CHECK(l.mods[5].id == "m4" && l.pos.sel == 5 && l.pos.top == 1);

        Drop d = carry_drop(l);

        CHECK(d.from == 4 && d.to == 5 && l.carrying == -1);
    }

    // carry_stops_at_ends
    {
        List l = make(3);

        l.pos = { 2, 0 };
        carry_begin(l);
        carry_step(l, 1, 5);
        CHECK(l.mods[2].id == "m2" && l.pos.sel == 2);
        carry_drop(l);
        l.pos = { 0, 0 };
        carry_begin(l);
        carry_step(l, -1, 5);
        CHECK(l.mods[0].id == "m0" && l.pos.sel == 0);
    }

    // cancel_restores_order_and_window
    {
        List l = make(8);

        l.pos = { 0, 0 };
        carry_begin(l);
        carry_to(l, 7, 5);
        CHECK(l.mods[7].id == "m0" && l.pos.top == 3);
        carry_cancel(l);
        CHECK(l.mods[0].id == "m0" && l.mods[7].id == "m7" && l.pos.sel == 0 && l.pos.top == 0 && l.carrying == -1);
    }

    // move_mod_steps_one_place_and_stops_at_failed
    {
        List l = make(4);

        l.mods[3].failed = "bad";
        CHECK(move_mod(l, 1, 1) == 2 && l.mods[2].id == "m1" && l.carrying == -1);
        CHECK(move_mod(l, 2, 1) == 2 && l.mods[2].id == "m1");
        CHECK(move_mod(l, 0, -1) == 0 && l.mods[0].id == "m0");
        CHECK(move_mod(l, 3, -1) == 3 && l.mods[3].id == "m3");
    }

    // number_options_step_and_print
    {
        CHECK(step_number(0.5, 0.0, 1.0, 0.25, 1) == 0.75);
        CHECK(step_number(0.9, 0.0, 1.0, 0.25, 1) == 1.0);
        CHECK(step_number(0.1, 0.0, 1.0, 0.25, -1) == 0.0);
        CHECK(step_number(5.0, 0.0, 10.0, 0.0, -1) == 4.0);
        CHECK(number_text(1.5, 2) == "1.50");
        CHECK(number_text(3.0, 0) == "3");
    }

    // rgba5551_tiles_cover_and_order
    {
        // A 2x1 image, red then blue, covering 4x2 in 2x2 tiles: the left tile all red, the right all blue.
        const uint8_t px[] = { 255, 0, 0, 255, 0, 0, 255, 255 };
        const std::vector<uint16_t> t = rgba5551_tiles(px, 2, 1, 4, 2, 2, 2);
        const uint16_t red = 0xF801, blue = 0x003F;
        const std::vector<uint16_t> want = { red, red, red, red, blue, blue, blue, blue };

        CHECK(t == want);
        CHECK(rgba5551_tiles(nullptr, 2, 1, 4, 2, 2, 2).empty());

        // Shrinking averages: a 2x2 of black and white columns into one pixel is mid grey (128 >> 3 = 16 per channel).
        const uint8_t bw[] = { 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255 };
        const std::vector<uint16_t> g = rgba5551_tiles(bw, 2, 2, 1, 1, 1, 1);

        CHECK(g.size() == 1 && g[0] == (uint16_t)(16 << 11 | 16 << 6 | 16 << 1 | 1));
    }

    // info_line_parts
    {
        Mod m{ "x", "X", "", true, false };

        CHECK(info_line(m, "BY").empty());
        m.version = "1.2.0";
        CHECK(info_line(m, "BY") == "V1.2.0");
        m.authors = { "Ann", "Bo" };
        CHECK(info_line(m, "BY") == "V1.2.0 - BY ANN, BO");
        m.version.clear();
        CHECK(info_line(m, "VON") == "VON ANN, BO");
    }

    // text_lines_wraps_and_squeezes_blanks
    {
        auto chars = [](const std::string& s) { return (int)s.size(); };
        auto all = [](char) { return true; };
        const std::vector<std::string> v = text_lines("\nHello world\n\n\n\tSecond  para\r\n", chars, 6, all);
        const std::vector<std::string> want = { "HELLO", "WORLD", "", "SECOND", "PARA" };

        CHECK(v == want);
        CHECK(text_lines("", chars, 6, all).empty());

        // A hard-wrapped long line joins the next; short lines (names) stay apart.
        const std::string prose = std::string(60, 'a') + "\nbb\nAnn\nBo";
        const std::vector<std::string> p = text_lines(prose, chars, 70, all);
        const std::vector<std::string> pw = { std::string(60, 'A') + " BB", "ANN", "BO" };

        CHECK(p == pw);
    }

    // row_text_trims_and_uppercases
    {
        Mod m{ "x", "a very long mod name indeed, really", "", false, false };
        Mod n{ "y", "Caf\xC3\xA9", "", true, false };

        Mod j{ "z", "Japanese (\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E)", "", true, false };

        Mod s{ "s", "Display Settings", "", true, false };
        List d;

        d.mods.push_back({ "d", "D", "Replaces the Controller Settings preset screen with a list of actions", true, false });
        CHECK(row_text(m) == "A VERY LONG M...\tOFF");
        CHECK(row_text(n) == "CAF\tON");
        CHECK(row_text(j) == "JAPANESE\tON");
        CHECK(row_text(s) == "DISPLAY SETTINGS\tON");
        CHECK(description_text(d, Words{}) == "REPLACES THE CONTROLLER SETTINGS PRESET SCREEN WITH A LIST OF ACTIONS");

        d.mods[0].description = std::string(250, 'A');
        CHECK(description_text(d, Words{}) == std::string(197, 'A') + "...");

        // The game's words come in already in its own text (no filtering): German ON / OFF.
        Words de{ "AN", "AUS", "FEHLER", "KEINE MODS INSTALLIERT" };

        CHECK(row_text(m, de) == "A VERY LONG M...\tAUS");
        CHECK(description_text(List{}, de) == "KEINE MODS INSTALLIERT");

        // fit_text: the longest start that fits (with "..."), here one width unit per character.
        auto chars = [](const std::string& t) { return (int)t.size(); };

        CHECK(fit_text("SHORT", chars, 10) == "SHORT");
        CHECK(fit_text("A LONGER LINE OF TEXT", chars, 12) == "A LONGER...");
        CHECK(fit_text("ABCDEFGHIJ", chars, 2) == "");

        // wrap_text: whole words per line, the last line cut with "..." when text is left over.
        CHECK(wrap_text("", chars, 9, 3) == "");
        CHECK(wrap_text("HI", chars, 9, 3) == "HI");
        CHECK(wrap_text("ONE TWO THREE", chars, 7, 3) == "ONE TWO\nTHREE");
        CHECK(wrap_text("AAAA BBBB CCCC DDDD EEEE", chars, 9, 2) == "AAAA BBBB\nCCCC D...");
        CHECK(wrap_text("ABCDEFGHIJKL MN", chars, 5, 3) == "AB...\nMN");
    }

    // restart_notice_clears_when_back_to_start
    {
        List l = make(3);
        Start s = snapshot(l);

        l.mods[1].enabled = false;
        CHECK(needs_restart(s, l));
        l.mods[1].enabled = true;
        CHECK(!needs_restart(s, l));
        std::swap(l.mods[0], l.mods[1]);
        CHECK(needs_restart(s, l));
        std::swap(l.mods[0], l.mods[1]);
        CHECK(!needs_restart(s, l));
        l.mods[2].runtime_toggleable = true;
        l.mods[2].enabled = false;
        CHECK(!needs_restart(s, l));
    }

    // failed_mods_list_last_and_stay_put
    {
        List l = make(3);
        Mod bad{ "broken.nrm", "broken.nrm", "", false, false, "Failed to open mod file" };

        l.mods.push_back(bad);

        const Start s = snapshot(l);

        CHECK(row_text(l.mods[3]) == "BROKEN.NRM\tFAILED");
        CHECK(description_text(l, Words{}) == "DESC 0");
        l.pos = { 3, 0 };
        CHECK(description_text(l, Words{}) == "FAILED TO OPEN MOD FILE");
        CHECK(carry_begin(l) == -1);
        l.pos = { 0, 0 };
        carry_begin(l);
        carry_to(l, 3, 5);
        CHECK(l.mods[2].id == "m0" && l.mods[3].id == "broken.nrm");
        carry_cancel(l);
        CHECK(!needs_restart(s, l));
    }

    std::printf("mods_page_test: ok\n");

    return 0;
}
