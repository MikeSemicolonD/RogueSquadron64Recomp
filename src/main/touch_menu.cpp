#include "touch_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace rs64::touch {
    namespace {
        constexpr uint32_t kGcmd = 0x800CE730u - 0x80000000u;
        constexpr uint32_t kPauseWord = 0x8010CA20u - 0x80000000u;
        constexpr uint32_t kSettingsFlags = 0x80130B50u - 0x80000000u;
        constexpr uint32_t kVolumes = 0x80130B60u - 0x80000000u;   // music, sfx, speech (u8, 0..128)
        constexpr uint8_t kMenuSoundSettings = 6;
        constexpr uint8_t kMenuAccount = 1;
        constexpr uint8_t kMenuMediaHub = 10;
        constexpr uint32_t kHudSlotId = 0x8010BFD0u - 0x80000000u;
        constexpr uint32_t kSlotTable = 0x80130BB0u - 0x80000000u;
        constexpr uint32_t kHandleHud = 0x800C0084u;
        // PauseMenuStuff arrays by current_menu: root, game settings, audio settings, abort confirm.
        constexpr uint32_t kPauseArrays[4] = { 0x80109DD4u, 0x80109E24u, 0x80109E68u, 0x80109EECu };
        // The mission's presented image is narrower than the front end's (measured 1290x923 vs 1335x923 on the S24+).
        constexpr float kMissionBoxAspect = 1.3976f;
        // Pause text advance per glyph in game units (spaces are not glyphs), fit from the S24+ screenshots.
        constexpr float kPauseGlyphAdvance = 9.6f;
        constexpr uint32_t kFrontScreen = 0x800CFF50u - 0x80000000u;
        constexpr uint8_t kTypeSaveSlot = 3;
        constexpr uint32_t kRamSize = 0x800000u;
        constexpr uint32_t kModHeap = 0x81000000u;
        constexpr uint32_t kLabelMemSize = 0x20000000u;

        uint8_t rb(const uint8_t* r, uint32_t off) {
            return r[off ^ 3];
        }

        uint16_t rh(const uint8_t* r, uint32_t off) {
            uint16_t v;
            std::memcpy(&v, r + (off ^ 2), 2);
            return v;
        }

        uint32_t rw(const uint8_t* r, uint32_t off) {
            uint32_t v;
            std::memcpy(&v, r + off, 4);
            return v;
        }

        bool in_ram(uint32_t p) {
            return p >= 0x80000000u && p < 0x80000000u + kRamSize;
        }

        // Labels live in game RAM or, for mod-added entries, on the mod heap (0x81000000+) inside the always-mapped 512 MB kseg0.
        bool str_len(const uint8_t* r, uint32_t p, int* len) {
            if (!in_ram(p) && !(p >= kModHeap && p < 0x80000000u + kLabelMemSize)) {
                return false;
            }
            const uint32_t off = p - 0x80000000u;
            int n = 0;
            while (n < 40 && off + n < kLabelMemSize && rb(r, off + n) != 0) {
                ++n;
            }
            *len = n;
            return true;
        }

        // The 320x240 2D space is presented as a centered box of the given aspect: fit inside the window, or (fill) scaled
        // to the window width and cropped top and bottom, as the front end does outside its menu tree (e.g. the media hub).
        void to_window_aspect(float px, float py, float w, float h, float aspect, float* nx, float* ny, bool fill = false) {
            float box_w = h * aspect;
            float box_h = h;
            if (box_w > w || fill) {
                box_w = w;
                box_h = w / aspect;
            }
            *nx = ((w - box_w) / 2.0f + px / 320.0f * box_w) / w;
            *ny = ((h - box_h) / 2.0f + py / 240.0f * box_h) / h;
        }

        // The media hub (menu id 10) and its pages show the image filled to the window width; the menu tree fits it.
        bool fill_screen(const uint8_t* r) {
            return rb(r, kGcmd + 0x04) >= kMenuMediaHub;
        }

        void to_window(float px, float py, float w, float h, float* nx, float* ny, bool fill = false) {
            to_window_aspect(px, py, w, h, g_menu_layout.box_aspect, nx, ny, fill);
        }
    }

    // Fit on a Galaxy S24+ (2340x1080) across MAIN MENU, OPTIONS, GAME/SOUND/CONTROLLER SETTINGS.
    MenuLayout g_menu_layout{ 1.446f, 160.0f, 0.625f, 121.87f, 0.5408f, 8.5f, 15.6f };

    Context classify(int overlay, const char* id, bool paused, bool demo) {
        if (overlay == 0) {
            if (demo) {
                return Context::Cutscene;
            }
            return paused ? Context::PauseMenu : Context::Flight;
        }
        if (overlay != 1) {
            return Context::Cutscene;
        }
        const std::string s = id ? id : "";
        if (s == "menu.passcodes") {
            return Context::Wheel;
        }
        if (s == "menu" || s == "menu.main" || s == "menu.account" || s == "menu.options" || s == "menu.game_settings" ||
            s == "menu.sound_settings" || s == "menu.controller_settings" || s == "menu.biographies" || s == "menu.elite_rogues" ||
            s == "menu.media") {
            return Context::ListMenu;
        }
        return Context::Carousel;
    }

    // The pause flag alone misses frames around the pause HUD's open/close animation, where tilt would otherwise move the cursor.
    bool read_paused(const uint8_t* r) {
        if (rw(r, kPauseWord) == 5) {
            return true;
        }
        const uint32_t hud = find_pause_hud(r);
        const uint8_t phase = hud ? rb(r, hud - 0x80000000u + 0x258) : 0;
        return phase >= 1 && phase <= 3;
    }

    bool read_demo(const uint8_t* r) {
        return (rw(r, kSettingsFlags) & 0x20u) != 0;
    }

    bool read_menu(const uint8_t* r, MenuSnapshot* snap, std::vector<Box>* boxes, float w, float h) {
        boxes->clear();
        const uint32_t ptr = rw(r, kGcmd);
        const uint8_t count = rb(r, kGcmd + 0x95);
        if (!in_ram(ptr) || count < 1 || count > 8 || w <= 0.0f || h <= 0.0f) {
            return false;
        }
        snap->menu_ptr = ptr;
        snap->menu_id = rb(r, kGcmd + 0x04);
        snap->count = count;
        snap->current = rb(r, kGcmd + 0x94);
        snap->active = rh(r, kGcmd + 0x98);
        const int list_base = (int16_t)rh(r, kGcmd + 0x96);
        const bool fill = fill_screen(r);
        const MenuLayout& L = g_menu_layout;
        for (int i = 0; i < count; ++i) {
            int len = 0;
            if (!str_len(r, rw(r, kGcmd + 0x08 + 4 * i), &len)) {
                return false;
            }
            if (len == 0 || !(snap->active & (1u << i))) {
                continue;
            }
            float scale;
            const uint32_t bits = rw(r, kGcmd + 0x54 + 4 * i);
            std::memcpy(&scale, &bits, 4);
            if (!(scale > 0.1f && scale < 4.0f)) {
                scale = 1.0f;
            }
            const float xo = (int16_t)rh(r, kGcmd + 0x30 + 4 * i);
            const float yo = (int16_t)rh(r, kGcmd + 0x32 + 4 * i);
            const float half_w = len * L.glyph_w * scale / 2.0f;
            const float th = L.line_h * scale;
            const float pad = th * 0.2f;
            const float game_y = (float)(int16_t)(list_base + 36 * i - 18 * count) + yo;
            const float cx = L.center_x + xo * L.x_scale;
            const float ty = L.y_origin + game_y * L.y_scale;
            Box b{ i, 0, 0, 0, 0 };
            to_window(cx - half_w, ty - pad, w, h, &b.x0, &b.y0, fill);
            to_window(cx + half_w, ty + th + pad, w, h, &b.x1, &b.y1, fill);
            boxes->push_back(b);
        }
        // Trim the padding between stacked neighbours to their midpoint so boxes never overlap.
        std::vector<Box*> order;
        for (Box& b : *boxes) {
            order.push_back(&b);
        }
        std::sort(order.begin(), order.end(), [](const Box* a, const Box* b) { return a->y0 < b->y0; });
        for (size_t i = 0; i + 1 < order.size(); ++i) {
            Box& a = *order[i];
            Box& b = *order[i + 1];
            const bool stacked = a.x0 < b.x1 && b.x0 < a.x1;
            if (stacked && a.y1 > b.y0) {
                const float mid = (a.y1 + b.y0) / 2.0f;
                a.y1 = mid;
                b.y0 = mid;
            }
        }
        return true;
    }

    void n64_to_window(float px, float py, float w, float h, float* nx, float* ny) {
        to_window(px, py, w, h, nx, ny);
    }

    TapAction select_game_tap(float x, float y, float w, float h) {
        auto inside = [&](float px0, float px1) {
            float x0, y0, x1, y1;
            to_window(px0, 165.0f, w, h, &x0, &y0);
            to_window(px1, 215.0f, w, h, &x1, &y1);
            return x >= x0 && x <= x1 && y >= y0 && y <= y1;
        };
        // Each side slot: its logo plus its "---" name line.
        if (inside(35.0f, 130.0f)) {
            return TapAction::Left;
        }
        if (inside(190.0f, 287.0f)) {
            return TapAction::Right;
        }
        return TapAction::None;
    }

    TapAction level_select_tap(float x, float y) {
        if (y < 0.80f) {
            return TapAction::None;
        }
        if (x < 0.26f) {
            return TapAction::Left;
        }
        if (x > 0.74f) {
            return TapAction::Right;
        }
        if (y > 0.94f) {
            return TapAction::CButton;
        }
        return TapAction::None;
    }

    bool on_wheel(float y) {
        return y >= 0.50f && y <= 0.65f;
    }

    TapAction carousel_arrow_tap(float x, float y) {
        if (y < 0.80f) {
            return TapAction::None;
        }
        if (x < 0.20f) {
            return TapAction::Left;
        }
        if (x > 0.80f) {
            return TapAction::Right;
        }
        return TapAction::None;
    }

    TapAction craft_select_tap(float x, float y) {
        if (y < 0.18f && x > 0.30f && x < 0.70f) {
            return TapAction::CButton;
        }
        if (x < 0.20f) {
            return TapAction::Left;
        }
        if (x > 0.80f) {
            return TapAction::Right;
        }
        return TapAction::None;
    }

    AccountScreen account_screen(const uint8_t* r) {
        // The screen var is stale outside the pilot flow (e.g. 0 in OPTIONS after a demo), so it only counts under the account menu.
        if (!in_ram(rw(r, kGcmd)) || rb(r, kGcmd + 0x04) != kMenuAccount) {
            return AccountScreen::NotAccount;
        }
        const uint16_t screen = rh(r, kFrontScreen);
        if (screen == 1) {
            return AccountScreen::Levels;
        }
        if (screen == 0) {
            return AccountScreen::Craft;
        }
        if (screen == 2 && rb(r, kGcmd + 0x04) == kMenuAccount && rb(r, kGcmd + 0x28) == kTypeSaveSlot) {
            return AccountScreen::SelectGame;
        }
        return AccountScreen::NotAccount;
    }

    bool sound_slider(const uint8_t* r, int entry, float w, float h, SliderBar* bar) {
        const uint32_t ptr = rw(r, kGcmd);
        const uint8_t count = rb(r, kGcmd + 0x95);
        if (!in_ram(ptr) || rb(r, kGcmd + 0x04) != kMenuSoundSettings || entry < 0 || entry >= count || count > 8) {
            return false;
        }
        const uint8_t sub = rb(r, kGcmd + 0x28 + entry);
        if (sub < 21 || sub > 23) {
            return false;
        }
        // Bar sprite: 128 game units wide centered on x 0 (320x240: 120..200), y = entry_y + 7, about 9 units tall.
        const int list_base = (int16_t)rh(r, kGcmd + 0x96);
        const float entry_y = (float)(int16_t)(list_base + 36 * entry - 18 * count) + (int16_t)rh(r, kGcmd + 0x32 + 4 * entry);
        const float top = g_menu_layout.y_origin + (entry_y + 7.0f) * g_menu_layout.y_scale;
        bar->channel = sub - 21;
        to_window(120.0f, top - 12.0f, w, h, &bar->x0, &bar->y0);
        to_window(200.0f, top + 21.0f, w, h, &bar->x1, &bar->y1);
        return true;
    }

    int slider_value_at(const SliderBar& bar, float x) {
        const float t = (x - bar.x0) / (bar.x1 - bar.x0);
        const int v = (int)(t * 128.0f + 0.5f);
        return v < 0 ? 0 : (v > 128 ? 128 : v);
    }

    uint8_t read_volume(const uint8_t* r, int channel) {
        return rb(r, kVolumes + (uint32_t)channel);
    }

    void write_volume(uint8_t* r, int channel, uint8_t value) {
        r[(kVolumes + (uint32_t)channel) ^ 3] = value;
    }

    uint32_t find_pause_hud(const uint8_t* r) {
        const uint16_t id = rh(r, kHudSlotId);
        const uint32_t tbl = rw(r, kSlotTable);
        if (!in_ram(tbl) || id > 512) {
            return 0;
        }
        const uint32_t obj = rw(r, tbl - 0x80000000u + id * 8u);
        if (!in_ram(obj) || rw(r, obj - 0x80000000u) != kHandleHud) {
            return 0;
        }
        const uint32_t hud = rw(r, obj - 0x80000000u + 4u);
        return in_ram(hud) ? hud : 0;
    }

    bool read_pause_lines(const uint8_t* r, float w, float h, std::vector<PauseLine>* lines) {
        lines->clear();
        const uint32_t hud = find_pause_hud(r);
        if (!hud || rw(r, kPauseWord) != 5) {
            return false;
        }
        const uint32_t hh = hud - 0x80000000u;
        const uint8_t menu = rb(r, hh + 0xD6A);
        if (rb(r, hh + 0x258) != 3 || menu > 3) {
            return false;
        }
        // Drawn elements are chained from element 0; the last links outside the array.
        const uint32_t first = hud + 0x264;
        const uint32_t end = first + 16u * 0x30u;
        int drawn = 0;
        for (uint32_t e = first; drawn < 16 && e >= first && e < end; e = rw(r, e - 0x80000000u)) {
            ++drawn;
        }
        // Records: every non-terminator record takes the next element except spacers (0x1000). The root menu can hide
        // objective lines (0x0010) without taking an element, so skip that many objective records first.
        const uint32_t recs = kPauseArrays[menu] - 0x80000000u;
        int non_spacer = 0;
        for (int i = 0; i < 64; ++i) {
            const uint16_t f = rh(r, recs + 6u * i);
            if (f == 0xFFFF) {
                break;
            }
            if (!(f & 0x1000)) {
                ++non_spacer;
            }
        }
        int hidden_objectives = std::max(0, non_spacer - drawn);
        struct Geo { float ex, ey, width; bool wide; };
        std::vector<Geo> geo;
        int element = 0;
        int selectable = -1;
        int channel = -1;
        int k = 0;
        for (int i = 0; i < 64 && element < drawn; ++i) {
            const uint32_t rec = recs + 6u * i;
            const uint16_t f = rh(r, rec);
            if (f == 0xFFFF) {
                break;
            }
            if (f & 0x1000) {
                continue;
            }
            if ((f & 0x0010) && hidden_objectives > 0) {
                --hidden_objectives;
                continue;
            }
            if (f & 0x0001) {
                selectable = k++;
                // Slider title (0x4201): nextMenu holds the volume channel.
                channel = (f & 0x4000) ? (int)rh(r, rec + 4) : -1;
            } else if (!(f & 0x4000)) {
                selectable = -1;   // plain text line; slider parts (0x4xxx) keep their title's index
                channel = -1;
            }
            const uint32_t e = hh + 0x264 + (uint32_t)element * 0x30u;
            float ex, ey, sx;
            uint32_t bits = rw(r, e + 0x18);
            std::memcpy(&ex, &bits, 4);
            bits = rw(r, e + 0x1C);
            std::memcpy(&ey, &bits, 4);
            bits = rw(r, e + 0x24);
            std::memcpy(&sx, &bits, 4);
            if (!(sx > 0.01f && sx < 20.0f)) {
                sx = 1.0f;
            }
            // Text starts at x and runs kGlyphAdvance per glyph; a bar is one 16-unit glyph scaled wide.
            const uint16_t glyphs = rh(r, e + 0x08);
            const bool wide = sx > 2.0f;
            const float width = wide ? 16.0f * sx : glyphs * kPauseGlyphAdvance * sx;
            const float top = g_menu_layout.y_origin + ey * g_menu_layout.y_scale;
            PauseLine line{ element, selectable, 0, 0, 0, 0 };
            // The backing (0x4800 with nextMenu 1) spans the full value range; the fill's width follows the value.
            line.bar = wide && (f & 0x0800) && rh(r, rec + 4) == 1;
            line.channel = channel;
            pause_to_window(160.0f + ex * g_menu_layout.x_scale, top - 2.0f, w, h, &line.x0, &line.y0);
            pause_to_window(160.0f + (ex + width) * g_menu_layout.x_scale, top + 16.0f * g_menu_layout.y_scale + 2.0f, w, h, &line.x1, &line.y1);
            lines->push_back(line);
            geo.push_back({ ex, ey, width, wide });
            ++element;
        }
        // A text line alone on its row is laid out centered across the menu, so it spans x..-x (padded value columns
        // like "STEREO      ON" and the OFF..MAX ends line are wider than their glyphs). Shared rows (YES / NO) keep glyph extents.
        for (size_t i = 0; i < lines->size(); ++i) {
            if (geo[i].wide || geo[i].ex >= 0.0f) {
                continue;
            }
            bool shared = false;
            for (size_t j = 0; j < lines->size(); ++j) {
                if (j != i && !geo[j].wide && std::fabs(geo[j].ey - geo[i].ey) < 2.0f) {
                    shared = true;
                }
            }
            if (!shared) {
                float unused;
                const float right = std::max(-geo[i].ex, geo[i].ex + geo[i].width);
                pause_to_window(160.0f + right * g_menu_layout.x_scale, 0.0f, w, h, &(*lines)[i].x1, &unused);
            }
        }
        return !lines->empty();
    }

    void picture_extent(const uint8_t* r, int overlay, float w, float h, float* x0, float* x1) {
        float aspect = g_menu_layout.box_aspect;
        if (overlay == 0) {
            aspect = kMissionBoxAspect;
        } else if (overlay == 1 && r && (fill_screen(r) || account_screen(r) == AccountScreen::Levels || account_screen(r) == AccountScreen::Craft)) {
            *x0 = 0.0f;
            *x1 = 1.0f;
            return;
        }
        const float frac = (w > 0.0f) ? std::min(1.0f, h * aspect / w) : 1.0f;
        *x0 = (1.0f - frac) / 2.0f;
        *x1 = 1.0f - *x0;
    }

    void pause_to_window(float px, float py, float w, float h, float* nx, float* ny) {
        to_window_aspect(px, py, w, h, kMissionBoxAspect, nx, ny);
    }

    int pause_slider_value(const PauseLine& bar, float x) {
        const float t = (x - bar.x0) / (bar.x1 - bar.x0);
        const int v = (int)(t * 127.0f + 0.5f);
        return v < 0 ? 0 : (v > 127 ? 127 : v);
    }

    std::vector<PauseRow> pause_rows(const std::vector<PauseLine>& lines) {
        std::vector<PauseRow> rows;
        for (const PauseLine& l : lines) {
            if (l.selectable < 0) {
                continue;
            }
            auto it = std::find_if(rows.begin(), rows.end(), [&](const PauseRow& row) { return row.selectable == l.selectable; });
            if (it == rows.end()) {
                rows.push_back({ l.selectable, l.x0, l.x1, l.y0, l.y1 });
            } else {
                it->x0 = std::min(it->x0, l.x0);
                it->x1 = std::max(it->x1, l.x1);
                it->y0 = std::min(it->y0, l.y0);
                it->y1 = std::max(it->y1, l.y1);
            }
        }
        std::sort(rows.begin(), rows.end(), [](const PauseRow& a, const PauseRow& b) { return a.y0 < b.y0; });
        // Split stacked neighbours at the midpoint; rows side by side (no horizontal overlap, e.g. YES / NO) keep theirs.
        for (size_t i = 0; i + 1 < rows.size(); ++i) {
            const bool stacked = rows[i].x0 < rows[i + 1].x1 && rows[i + 1].x0 < rows[i].x1;
            if (stacked) {
                const float mid = (rows[i].y1 + rows[i + 1].y0) / 2.0f;
                rows[i].y1 = mid;
                rows[i + 1].y0 = mid;
            }
        }
        for (PauseRow& row : rows) {
            const float pad = (row.x1 - row.x0) * 0.05f;
            row.x0 -= pad;
            row.x1 += pad;
        }
        return rows;
    }

    bool pause_set_entry(uint8_t* r, int k) {
        const uint32_t hud = find_pause_hud(r);
        if (!hud || k < 0) {
            return false;
        }
        const uint16_t v = (uint16_t)k;
        std::memcpy(r + ((hud - 0x80000000u + 0xD68) ^ 2), &v, 2);
        return true;
    }

    int pause_submenu(const uint8_t* r) {
        const uint32_t hud = find_pause_hud(r);
        if (!hud) {
            return -1;
        }
        const uint32_t hh = hud - 0x80000000u;
        const uint8_t menu = rb(r, hh + 0xD6A);
        return (rb(r, hh + 0x258) == 3 && menu <= 3) ? menu : -1;
    }

    int pause_entry(const uint8_t* r) {
        const uint32_t hud = find_pause_hud(r);
        return hud ? rh(r, hud - 0x80000000u + 0xD68) : -1;
    }

    int pause_find_entry(const uint8_t* r, int menu, uint16_t next) {
        if (menu < 0 || menu > 3) {
            return -1;
        }
        const uint32_t recs = kPauseArrays[menu] - 0x80000000u;
        int k = 0;
        for (int i = 0; i < 64; ++i) {
            const uint16_t f = rh(r, recs + 6u * i);
            if (f == 0xFFFF) {
                break;
            }
            if (f & 0x0001) {
                if (rh(r, recs + 6u * i + 4) == next) {
                    return k;
                }
                ++k;
            }
        }
        return -1;
    }

    std::string describe_pause_records(const uint8_t* r, int menu) {
        std::string s;
        if (menu < 0 || menu > 3) {
            return s;
        }
        const uint32_t recs = kPauseArrays[menu] - 0x80000000u;
        char buf[48];
        for (int i = 0; i < 64; ++i) {
            const uint16_t f = rh(r, recs + 6u * i);
            if (f == 0xFFFF) {
                break;
            }
            std::snprintf(buf, sizeof(buf), " [%04X %04X %04X]", f, rh(r, recs + 6u * i + 2), rh(r, recs + 6u * i + 4));
            s += buf;
        }
        return s;
    }

    bool pause_tap_select(uint8_t* r, float x, float y, float w, float h) {
        return pause_hover(r, x, y, w, h) >= 0;
    }

    int pause_hover(uint8_t* r, float x, float y, float w, float h) {
        std::vector<PauseLine> lines;
        if (!read_pause_lines(r, w, h, &lines)) {
            return -1;
        }
        for (const PauseRow& row : pause_rows(lines)) {
            if (x >= row.x0 && x <= row.x1 && y >= row.y0 && y <= row.y1) {
                return pause_set_entry(r, row.selectable) ? row.selectable : -1;
            }
        }
        return -1;
    }

    std::string describe_pause(const uint8_t* r) {
        const uint32_t hud = find_pause_hud(r);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "gate=%u hud=0x%08X", rw(r, kPauseWord), hud);
        std::string s = buf;
        if (!hud) {
            return s;
        }
        const uint32_t h = hud - 0x80000000u;
        std::snprintf(buf, sizeof(buf), " phase=0x%08X menu=%u entry=%u", rw(r, h + 0x258), rb(r, h + 0xD6A), rh(r, h + 0xD68));
        s += buf;
        for (int i = 0; i < 16; ++i) {
            const uint32_t e = h + 0x264 + (uint32_t)i * 0x30u;
            const uint16_t n = rh(r, e + 0x08);
            if (n == 0 || n > 64) {
                continue;
            }
            const uint32_t pos = rw(r, e + 0x10);
            int width = -1;
            if (in_ram(pos)) {
                width = (int16_t)rh(r, pos - 0x80000000u + n * 4u);
            }
            float fx, fy, sx, sy;
            uint32_t bits = rw(r, e + 0x18);
            std::memcpy(&fx, &bits, 4);
            bits = rw(r, e + 0x1C);
            std::memcpy(&fy, &bits, 4);
            bits = rw(r, e + 0x24);
            std::memcpy(&sx, &bits, 4);
            bits = rw(r, e + 0x28);
            std::memcpy(&sy, &bits, 4);
            std::snprintf(buf, sizeof(buf), " [%d n=%u x=%.1f y=%.1f w=%d s=%.2f/%.2f c=%08X]", i, n, fx, fy, width, sx, sy, rw(r, e + 0x2C));
            s += buf;
        }
        return s;
    }

    std::string describe_menu(const uint8_t* r) {
        const uint32_t ptr = rw(r, kGcmd);
        const uint8_t count = rb(r, kGcmd + 0x95);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "ptr=0x%08X id=%u count=%u base=%d cur=%u active=0x%04X", ptr, rb(r, kGcmd + 0x04), count, (int16_t)rh(r, kGcmd + 0x96), rb(r, kGcmd + 0x94), rh(r, kGcmd + 0x98));
        std::string s = buf;
        if (!in_ram(ptr) || count < 1 || count > 8) {
            return s;
        }
        for (int i = 0; i < count; ++i) {
            int len = 0;
            str_len(r, rw(r, kGcmd + 0x08 + 4 * i), &len);
            float scale;
            const uint32_t bits = rw(r, kGcmd + 0x54 + 4 * i);
            std::memcpy(&scale, &bits, 4);
            std::snprintf(buf, sizeof(buf), " [%d len=%d x=%d y=%d s=%.3f]", i, len, (int16_t)rh(r, kGcmd + 0x30 + 4 * i), (int16_t)rh(r, kGcmd + 0x32 + 4 * i), scale);
            s += buf;
        }
        return s;
    }

    bool tap_select(uint8_t* r, const MenuSnapshot& at_down, float x, float y, float w, float h) {
        MenuSnapshot now{};
        std::vector<Box> boxes;
        if (!read_menu(r, &now, &boxes, w, h)) {
            return false;
        }
        if (now.menu_ptr != at_down.menu_ptr || now.menu_id != at_down.menu_id) {
            return false;
        }
        for (const Box& b : boxes) {
            if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) {
                r[(kGcmd + 0x94) ^ 3] = (uint8_t)b.entry;
                return true;
            }
        }
        return false;
    }
}
