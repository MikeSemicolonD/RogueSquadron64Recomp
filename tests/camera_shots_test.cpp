#include "game/camera_shots.h"
#include "check.h"
#include <cstdio>

using namespace rs64shots;

static std::vector<Link> one(const char* text) {
    std::vector<Link> links;
    addLinks(links, text);
    return links;
}

static std::string cam(const std::optional<Shot>& s) {
    return s ? s->camera : std::string();
}

int main() {
    // parses_hand_written_files
    {
        const auto links = one("# c\r\nlv_0 canyon_entry cam_a\r\n\n  lv_1\tevt_x\tcam_b  \nlv_2 only_two\n");
        CHECK(links.size() == 2);
        CHECK(links[0].level == "lv_0" && links[0].trigger == "canyon_entry" && links[0].camera == "cam_a");
        CHECK(links[1].level == "lv_1" && links[1].trigger == "evt_x" && links[1].camera == "cam_b");
    }
    // personal_is_an_optional_fourth_word: shared unless it says personal
    {
        const auto links = one("lv_0 a cam personal\r\nlv_0 b cam shared\nlv_0 c cam\nlv_0 d cam other\n");
        CHECK(links.size() == 4);
        CHECK(links[0].personal && !links[1].personal && !links[2].personal && !links[3].personal);
    }
    // later_link_wins
    {
        std::vector<Link> links;
        CHECK(addLinks(links, "lv_0 t cam_a personal").empty());
        const auto replaced = addLinks(links, "lv_0 t cam_b\nlv_0 u cam_c\n");
        CHECK(links.size() == 2);
        CHECK(links[0].camera == "cam_b" && !links[0].personal);
        // later_link_reports_what_it_replaced: the replaced link, for the log
        CHECK(replaced.size() == 1 && replaced[0].trigger == "t" && replaced[0].camera == "cam_a" && replaced[0].personal);
    }
    const auto links = one("lv_0 t cam_a\n");
    // fires_once_per_load
    {
        Queue q;
        q.reset("lv_0");
        CHECK(q.fired(links, "t", false));
        CHECK(!q.fired(links, "t", false));
        CHECK(!q.fired(links, "t", true));
        CHECK(cam(q.take(false)) == "cam_a");
        CHECK(!q.take(false));
        q.reset("lv_0");
        q.fired(links, "t", false);
        CHECK(cam(q.take(false)) == "cam_a");
    }
    // waits_while_busy
    {
        Queue q;
        q.reset("lv_0");
        q.fired(links, "t", false);
        CHECK(!q.take(true));
        CHECK(cam(q.take(false)) == "cam_a");
    }
    // ignores_other_levels_and_unlinked
    {
        Queue q;
        q.reset("lv_1");
        q.fired(links, "t", false);
        q.fired(links, "nope", false);
        CHECK(!q.take(false));
    }
    // personal_shots_belong_to_whoever_set_them_off: shared plays here and tells the other player; personal plays only for the player
    // who triggered it (here, or handed to the other player when this game applied the trigger for them)
    {
        const auto mixed = one("lv_0 s cam_s\nlv_0 p cam_p personal\n");
        Queue q;
        q.reset("lv_0");
        q.fired(mixed, "s", true);
        const auto shared = q.take(false);
        CHECK(shared && playsHere(*shared) && tellsPeer(*shared));
        q.fired(mixed, "p", false);
        const auto mine = q.take(false);
        CHECK(mine && playsHere(*mine) && !tellsPeer(*mine));
        q.reset("lv_0");
        q.fired(mixed, "p", true);
        const auto theirs = q.take(false);
        CHECK(theirs && !playsHere(*theirs) && tellsPeer(*theirs));
    }
    // remote_shot_plays_once_with_local: a shot from the other player counts as its trigger firing here, and is ignored after a local one
    {
        Queue q;
        q.reset("lv_0");
        CHECK(q.remote(links, "t"));
        CHECK(!q.fired(links, "t", false));
        CHECK(!q.remote(links, "t"));
        q.reset("lv_0");
        CHECK(q.fired(links, "t", false));
        CHECK(!q.remote(links, "t"));
        CHECK(q.remote(links, "unlinked_here"));
    }
    // shots_sharing_a_camera_play_per_trigger: two triggers linked to one camera are two shots on both sides
    {
        const auto two = one("lv_0 t1 cam\nlv_0 t2 cam\n");
        Queue q;
        q.reset("lv_0");
        CHECK(q.remote(two, "t1") && q.remote(two, "t2") && !q.remote(two, "t1"));
        q.reset("lv_0");
        CHECK(q.fired(two, "t2", false) && cam(q.take(false)) == "cam");
        q.fired(two, "t1", false);
        const auto s = q.take(false);
        CHECK(s && s->trigger == "t1");
    }
    // parses_destroy_lines: "<level> <trigger> destroy <object>" adds to the trigger's link without touching its camera
    const auto staged = one("lv_0 t destroy tower_1\nlv_0 t cam_a\nlv_0 t destroy tower_2\nlv_0 u destroy gate\nlv_0 u destroy gate\nlv_0 v destroy\n");
    {
        CHECK(staged.size() == 2);
        CHECK(staged[0].trigger == "t" && staged[0].camera == "cam_a" && staged[0].destroy == std::vector<std::string>({ "tower_1", "tower_2" }));
        CHECK(staged[1].trigger == "u" && staged[1].camera.empty() && staged[1].destroy == std::vector<std::string>({ "gate" }));
        std::vector<Link> l;
        CHECK(addLinks(l, "lv_0 t destroy a\nlv_0 t cam\n").empty());
    }
    // destroy_queue_once_per_trigger: the destroy batch first, then the camera kShotDelayFrames non-busy frames later
    {
        Queue q;
        q.reset("lv_0");
        CHECK(q.fired(staged, "t", false));
        CHECK(!q.fired(staged, "t", false));
        const auto d = q.take(false);
        CHECK(d && d->camera.empty() && d->destroy.size() == 2);
        for (int i = 0; i < kShotDelayFrames; i++) {
            CHECK(!q.take(false));
            CHECK(!q.take(true));
        }
        const auto c = q.take(false);
        CHECK(c && c->camera == "cam_a" && c->destroy.empty());
        CHECK(!q.take(false));
        CHECK(q.fired(staged, "u", false));
        const auto g = q.take(false);
        CHECK(g && g->camera.empty() && g->destroy == std::vector<std::string>({ "gate" }));
        CHECK(!q.take(false));
    }
    // destroy_skips_dead_or_inconsistent: a spawned unit takes the HIT only through a slot whose context points back at it
    CHECK(destroyKind(10, 0x12, true, false) == Destroy::Hit);
    CHECK(destroyKind(10, 0xFFFF, false, false) == Destroy::CutHealth);
    CHECK(destroyKind(0, 0x12, true, false) == Destroy::Skip);
    CHECK(destroyKind(-5, 0xFFFF, false, false) == Destroy::Skip);
    CHECK(destroyKind(10, 0x12, false, false) == Destroy::Skip);
    CHECK(destroyKind(10, 0x12, true, true) == Destroy::Skip);
    // busy_while_shot_transition_or_paused: HUD flag word bits 0x40 (shot) / 0x80 (transition), pause freeze word bit 1
    CHECK(busy(0x40u, 0u) && busy(0x80u, 0u) && busy(0u, 1u));
    CHECK(!busy(0u, 0u) && !busy(0x20u, 2u));
    // only_camera_items_play: the item's u16 subtype (high half of its first word) must be 0x26
    CHECK(isCameraItem(0x00260000u));
    CHECK(!isCameraItem(0x00180700u) && !isCameraItem(0x00000026u));
    std::printf("camera_shots_test: ok\n");
    return 0;
}
