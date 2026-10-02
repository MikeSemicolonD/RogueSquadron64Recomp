// Shared state and helpers of the ghost co-op session, split across ghost.cpp (session), ghost_lobby.cpp (front-end lobby) and ghost_hud.cpp (HUD and radar).
#pragma once
#include "lockstep_core.h"
#include "net_core.h"
#include "net_link.h"
#include "rdram_words.h"
#include "mp_host.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

extern "C" {
uint32_t rs64_coop_imposter(void);
void rs64_imposter_reset(uint8_t* rdram);
uint32_t rs64_ghost_mode(void);
uint32_t rs64_ghost_active(void);
void rs64_ghost_quit(void);
uint32_t rs64_ghost_fixed_dt(void);
uint32_t rs64_ghost_is_client(void);
// Registers the multiplayer text sources, actions, the key handler and the cutscene-skip input filter with the host registry.
void rs64_ghost_register_host(void);
uint32_t rs64_lobby_menu_session(void);
void rs64_lobby_host(void);
void rs64_lobby_join(void);
void rs64_lobby_leave(void);
void rs64_lobby_cancel(void);
void rs64_lobby_tick(void);
const char* rs64_lobby_status(void);
const char* rs64_lobby_host_address(void);
// The host page ONLINE row: the router mapping state or the internet address to give a remote player.
const char* rs64_lobby_online_label(void);
const char* rs64_lobby_address_label(void);
// Lobby address keyboard entry, driven from the SDL event loop.
int rs64_lobby_typing(void);
void rs64_lobby_type_text(const char* utf8);
void rs64_lobby_type_key(int key);
int rs64_lobby_state(void);
uint32_t rs64_lobby_hosting(void);
void rs64_lobby_edit_toggle(void);
void rs64_lobby_edit_off(void);
int rs64_ghost_puppet_model(void);
int rs64_ghost_remote_foil(void);
uint32_t rs64_lobby_ready(void);
void rs64_lobby_input(uint8_t* rdram);
int rs64_ghost_puppet(const float seed[9], float dt, float out[12]);
void rs64_ghost_mission_end(uint8_t* rdram);
uint32_t rs64_ghost_block_result(void);
uint32_t rs64_ghost_take_start_offset(void);
uint32_t rs64_ghost_want_puppet(float out_pos[3]);
void rs64_ghost_puppet_spawned(void);
void rs64_imposter_despawn(uint8_t* rdram, recomp_context* ctx);
void rs64_ghost_mission_init(uint8_t*, recomp_context*);
void rs64_ghost_frame(uint8_t*, recomp_context*);
void rs64_ghost_powerup_touch(uint8_t*, recomp_context*);
void rs64_ghost_powerup_collect(uint8_t*, recomp_context*);
uint32_t rs64_ghost_gate_response(uint32_t, uint32_t);
void rs64_imposter_frame(uint8_t*, recomp_context*);
void rs64_imposter_release(uint8_t*, recomp_context*);
uint32_t rs64_ghost_block_transition(uint8_t*, uint32_t);
uint32_t rs64_ghost_unfreeze(uint8_t*);
uint32_t rs64_ghost_objective_event(uint8_t*, recomp_context*);
void rs64_imposter_tick(uint8_t*, recomp_context*);
void rs64_imposter_activation(uint8_t*, recomp_context*, uint32_t);
void rs64_imposter_stream(uint8_t*, recomp_context*);
void rs64_lobby_mission_select_init(uint8_t*, recomp_context*);
void rs64_lobby_mission_confirmed(uint8_t*, recomp_context*);
void rs64_lobby_mission_select_fonts(recomp_context*);
void rs64_lobby_mission_select_tick(uint8_t*, recomp_context*);
void rs64_lobby_craft_select_init(recomp_context*);
void rs64_lobby_craft_select_tick(uint8_t*, recomp_context*);
void rs64_ghost_craft_assets(uint8_t*, recomp_context*);
void rs64_ghost_hud_fonts(recomp_context*);
void rs64_ghost_hud_draw(uint8_t*, recomp_context*);
void rs64_ghost_radar(uint8_t*, recomp_context*);
}

namespace rs64::ghost {

using namespace rs64::ls;

struct Ghost {
    rs64::net::Link link;
    // The front-end lobby (or ROGUESQ_MP at the first mission) owns the link; a session runs only once both sides said a matching HELLO.
    rs64::net::Lobby lobby;
    bool was_connected = false;
    uint32_t seen_connects = 0;
    bool was_ready = false;
    // A failed lobby stops its link at this time (ms, -1 none).
    int64_t stop_at_ms = -1;
    // The other player's craft (LAUNCH on the client, READY from craft select on both), -1 unknown.
    int remote_craft = -1;
    // Client: the host's latest mission pick not yet followed (-1 none). follow_cool spaces the injected presses; status_buf is the RDRAM string for the mission-select status line.
    int host_pick = -1;
    // Client: the host's mission-select cursor and screen state (BROWSE), -1 before the first. Host: what it last sent.
    int host_cursor = -1;
    int host_state = 0;
    int sent_browse = -1;
    // Craft select: nobody leaves until both picked. release_cool spaces the injected confirm until the hangar takes it.
    rs64::ls::CraftBarrier craft;
    int release_cool = 0;
    // In craft select this round; this side left it (told the peer); the peer left, so this side follows them out.
    bool in_hangar = false;
    bool left_sent = false;
    bool follow_out = false;
    // The lives byte last frame (-1 at mission start), to show the counter when the other player's death takes one.
    int lives_seen = -1;
    // Upgrades: the word's upgrade bits on the mission's first frame (-1 before it), bits already shared either way, and received bits waiting for that baseline.
    int64_t upgrades_base = -1;
    uint32_t upgrades_shared = 0;
    uint32_t upgrades_in = 0;
    // Power-ups: DAT records the other player collected (collected here on their next tick), and the DAT indices collected here (sent again to a peer entering late).
    std::vector<uint32_t> pickups_forced;
    std::vector<uint16_t> pickups_mine;
    bool test_pickup_done = false;
    // The other player's pilot name from their HELLO (label over their ship).
    std::string remote_name;
    // The other player's S-foils from their STATE, 0 closed .. 250 open.
    int remote_foil = 250;
    int follow_cool = 0;
    uint32_t status_buf = 0;
    // A handshake completed (menus or ROGUESQ_MP): ghost mode stays on for later missions. from_menu: the menus started it, so ghost mode is on without ROGUESQ_MP.
    bool lobby_session = false;
    bool from_menu = false;
    std::string status_text;
    std::string host_address;
    std::string lan_address;
    std::string address_text;
    rs64::net::AddressEditor editor;
    // While hosting the row holds the host address; join_entry is the join address it replaced (restored on stop). last_host: roguesq_net.json's.
    bool row_hosting = false;
    std::string join_entry;
    std::string last_host;
    uint16_t host_port = 0;
    // With a relay transport (mp.transport service) the address row holds a join code instead; relay_session: this lobby runs over it.
    rs64::net::CodeEditor code;
    bool relay_session = false;
    // SDL text input is on (the address is being typed); the phone keyboard was seen up; frames left to drop the A/START presses after typing ends.
    bool text_on = false;
    bool kb_shown = false;
    int swallow_frames = 0;
    rs64::ls::SkipShare skip;
    // Host: when the client asked to end the briefing (ms, -1 none). Client: asked during this briefing.
    int64_t brief_done_ms = -1;
    bool brief_sent = false;
    uint32_t skips_seen = 0;
    uint16_t skip_prev_buttons = 0;
    // 0 none, 1 hangar launch, 2 cutscene timeline.
    int skip_scene = 0;
    // LAN discovery: the host answers searches while it waits; JOIN searches for kSearchMs before falling back to the typed address.
    rs64::net::DiscoveryResponder responder;
    // Internet hosting: the router's UPnP mapping of the game port while this side hosts.
    rs64::net::PortMapper mapper;
    std::string online_text;
    rs64::net::DiscoveryFinder finder;
    bool searching = false;
    uint32_t search_until_ms = 0;
    uint32_t next_query_ms = 0;
    rs64::net::LinkConfig search_cfg;
    bool started = false;
    bool in_session = false;
    bool peer_left = false;
    uint8_t player = 0;
    uint8_t epoch = 0;
    uint32_t seq = 0;
    int remote_epoch = -1;
    uint32_t last_seq = 0;
    bool remote_in = true;
    bool remote_down = false;
    Vec3 remote_pos{};
    bool start_offset_done = false;
    bool puppet_seen = false;
    bool have_remote = false;
    double remote_rx_s = 0.0;
    uint8_t remote_pad[6] = {0, 0, 0, 0, 1, 0};
    bool have_prev = false;
    Vec3 prev_pos{};
    std::string launch;
    Puppet puppet;
    bool puppet_shown = false;
    bool despawned = false;
    Vec3 puppet_pos{};
    Vec3 puppet_up{0.0f, 1.0f, 0.0f};
    int pending_result = -1;
    int host_result = -1;
    int allow_transition = -1;
    std::vector<FrameInput> frames;
    uint32_t frame = 0;
    int missions = 0;
    FILE* trace = nullptr;
    uint8_t* rdram = nullptr;
    bool result_sent = false;
    // The host's id for the current mission attempt (host: its epoch; client: from the host's MISSION before LAUNCH). DAMAGE, OBJ and LIFE carry it.
    uint8_t mission_id = 0;
    int latest_mission = -1;
    // The peer entered the mission: send our DAT health once in full.
    bool resync = false;
    // A local death was accepted (requestMissionTransitionMode(1)); next frame, a lower lives byte means a life was actually spent.
    bool life_check = false;
    uint8_t lives_before = 0;
    // This player lost the last shared life and spectates: its craft rides on the survivor's pose, cannot steer, fire or die. remote_out: the host's record of the client being out.
    bool out = false;
    bool remote_out = false;
    // Written over the lives byte next frame (-1 none): the game decrements it when accepting a death, so a death that must not spend a life is bumped first and put back after.
    int restore_lives = -1;
    // Host: the objective snapshot last sent. Client: the newest snapshot received but not yet written.
    rs64::net::ObjSnapshot obj_last_sent;
    bool obj_sent_any = false;
    int obj_resend = 0;
    uint32_t obj_seq = 0;
    rs64::net::ObjSnapshot obj_pending;
    bool obj_have = false;
    uint32_t obj_last_seq = 0;
    uint32_t obj_applied = 0;
    // Shared deaths: last health reported (or applied) per DAT item, and the peer's drops waiting to be applied at frame start.
    std::vector<int32_t> dat_last;
    bool dat_baseline = false;
    std::vector<std::pair<uint16_t, int32_t>> dat_apply;
    std::map<uint16_t, int> dat_retry_frames;
    uint32_t dat_sent = 0;
    uint32_t dat_applied = 0;
};

Ghost& g();
uint32_t now_ms32();
double now_s();
void drain();
void lobby_start(const rs64::net::LinkConfig& cfg);
rs64::net::LinkConfig link_config();
// The mp.transport a mod published (mods/steam-relay), unless ROGUESQ_MP_TRANSPORT=enet; looked up on first use, which must be after the registry sealed. Does not initialize it.
const rs64_mp_transport* relay();
// relay() when it can host or join now (available() may initialize it: call only while the MULTIPLAYER page is up or a session runs).
const rs64_mp_transport* relay_ready();
// Releases the relay (Steam shuts down) when the lobby or session ends.
void relay_shutdown();
void load_net_config();
void save_net_config();
bool mp_menus();
bool demo_active(const uint8_t* rdram);
void show_status(uint8_t* rdram, recomp_context* ctx, uint32_t font, uint32_t slot, int y, const std::string& text, std::string* shown);
// The lives pool: numLives, gMissionState+0 (u8). Host-owned and streamed with the objectives.
constexpr uint32_t kLives = 0x80130B10u;

uint8_t rb(const uint8_t* rdram, uint32_t a);
void wb(uint8_t* rdram, uint32_t a, uint8_t v);
void wf(uint8_t* rdram, uint32_t a, float v);

}
