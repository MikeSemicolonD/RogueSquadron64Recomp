#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "net/crypto_session.h"

namespace rs64::net {

constexpr uint8_t kProtocol = 1;
constexpr size_t kMaxPacket = 1200;

// 1 was the lockstep frame packet and 8 a client objective-write event (both retired); keep them unused.
enum class Msg : uint8_t { Frame = 1, Launch = 2, Bye = 3, State = 4, Result = 5, Presence = 6, Obj = 7, Damage = 9, Life = 10, Mission = 11, Out = 12, Hello = 13, Pick = 14, Ready = 15, Browse = 16, Skip = 17, BriefingDone = 18, Upgrades = 19, Pickup = 20, Trigger = 21, TowTrip = 22, Shot = 23, Auth = 24, Accept = 25, Continue = 26, TowSeg = 27, Pake1 = 28, Pake2 = 29, Pake3 = 30, Name = 31 };
enum class ByeReason : uint8_t { Finished = 0, Desync = 1, Quit = 2, Mismatch = 3, WrongPassword = 4 };

std::vector<uint8_t> encode_launch(const std::string& header_text);
bool decode_launch(const uint8_t* data, size_t n, std::string* header_text);
std::vector<uint8_t> encode_bye(ByeReason reason, uint32_t frame);

// What one lobby poll does about the link: connected = a new peer connected and is still up (say HELLO); lost = the previous peer is gone. A connect that dropped before the poll is neither.
struct LinkEdges { bool connected = false; bool lost = false; };
LinkEdges link_poll_edges(bool up, bool was_up, uint32_t connects, uint32_t seen_connects);
bool decode_bye(const uint8_t* data, size_t n, ByeReason* reason, uint32_t* frame);

// StatePacket flags: the sender's player is down (respawning); the receiver hides the puppet until it's back.
constexpr uint8_t kStateDown = 1u << 0;

// Ghost co-op: a player's craft state and pad buttons, sent unreliably every frame; vel is in units per second.
struct StatePacket {
    uint32_t seq = 0;

    // The sender's mission counter: a new mission restarts seq.
    uint8_t epoch = 0;
    float pos[3] = {};
    float fwd[3] = {};
    float down[3] = {};
    float vel[3] = {};
    uint8_t pad[6] = {};
    uint8_t flags = 0;

    // The sender's S-foils, 0 closed .. 250 open (its X-wing's foil state), shown on its puppet.
    uint8_t foil = 250;

    // The sender's steady clock (ms) when it sent this: the receiver's interpolation rebuilds the sender's timeline from it.
    uint32_t sent_ms = 0;
};

std::vector<uint8_t> encode_state(const StatePacket& s);
bool decode_state(const uint8_t* data, size_t n, StatePacket* out);

// Ghost co-op, reliable: the host's mission result (0 abort, 1 success, 2 failed, 3 out of lives) and a player entering (1) or leaving (0) a mission.
std::vector<uint8_t> encode_result(uint8_t epoch, uint8_t result);
bool decode_result(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* result);
std::vector<uint8_t> encode_presence(uint8_t epoch, uint8_t in_mission);
bool decode_presence(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* in_mission);

// Ghost co-op objectives: the host's objective booleans, counts, timers and lives (unreliable, newest seq wins).
struct ObjSnapshot {
    uint8_t epoch = 0;
    uint32_t seq = 0;
    uint8_t bools[128] = {};
    int32_t counts[128] = {};
    float timers[8] = {};

    // The shared lives pool (gMissionState+0).
    uint8_t lives = 0;
};

std::vector<uint8_t> encode_obj(const ObjSnapshot& s);
bool decode_obj(const uint8_t* data, size_t n, ObjSnapshot* out);

// Ghost co-op shared deaths: DAT item (index in the level's type-0 list) health drops seen in the sender's world. At most kDamageBatch pairs per packet; encode truncates.
constexpr size_t kDamageBatch = 190;
std::vector<uint8_t> encode_damage(uint8_t epoch, const std::vector<std::pair<uint16_t, int32_t>>& items);
bool decode_damage(const uint8_t* data, size_t n, uint8_t* epoch, std::vector<std::pair<uint16_t, int32_t>>* items);

// Ghost co-op: the host's id for the mission it is starting (sent before LAUNCH); DAMAGE, OBJ and LIFE carry it so traffic from another attempt is dropped.
std::vector<uint8_t> encode_mission(uint8_t id);
bool decode_mission(const uint8_t* data, size_t n, uint8_t* id);

// Ghost co-op: the client lost a life; the host takes it from the shared pool.
std::vector<uint8_t> encode_life_lost(uint8_t epoch);
bool decode_life_lost(const uint8_t* data, size_t n, uint8_t* epoch);

// Lobby mission flow: the host's confirmed mission (level 0..0x12), and a player's confirmed craft (0..8; 0xFF = backed out of craft select).
std::vector<uint8_t> encode_pick(uint8_t level);
bool decode_pick(const uint8_t* data, size_t n, uint8_t* level);

// The host browsing mission select: its cursor (level) and screen state (2 browse, 3 briefing, 4 exiting), sent on change so the client mirrors it live.
std::vector<uint8_t> encode_browse(uint8_t level, uint8_t state);
bool decode_browse(const uint8_t* data, size_t n, uint8_t* level, uint8_t* state);

// A player skipped a cutscene; the other side skips its own. scene = skip phase << 8 | skippable scenes entered in that phase (SkipShare).
std::vector<uint8_t> encode_skip(uint16_t scene);
bool decode_skip(const uint8_t* data, size_t n, uint16_t* scene);

// The client ended the mission briefing; the host confirms its own (the client then follows the host out).
std::vector<uint8_t> encode_briefing_done();
bool decode_briefing_done(const uint8_t* data, size_t n);
std::vector<uint8_t> encode_ready(uint8_t craft);
bool decode_ready(const uint8_t* data, size_t n, uint8_t* craft);

// Ghost co-op, host to client: the client's death took the last shared life, so it is out and spectates until the mission ends.
std::vector<uint8_t> encode_out(uint8_t epoch);
bool decode_out(const uint8_t* data, size_t n, uint8_t* epoch);

// Power-up bits in the settings word 0x80130B4C that npcPowerUpUpdate ORs in (0x200 adv blasters .. 0x10000 adv shields).
constexpr uint32_t kUpgradeMask = 0x0001FE00u;

// Ghost co-op, reliable: upgrades a player picked up this mission; the other side ORs them into its own word.
std::vector<uint8_t> encode_upgrades(uint8_t epoch, uint32_t bits);
bool decode_upgrades(const uint8_t* data, size_t n, uint8_t* epoch, uint32_t* bits);

// Ghost co-op, reliable: a player collected the power-up with this DAT item index; the other side collects its own copy.
std::vector<uint8_t> encode_pickup(uint8_t epoch, uint16_t item);
bool decode_pickup(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item);

// Ghost co-op, reliable, client to host: the client's craft entered (or left) a player trigger volume; event = its index in the level's DAT event table (12 bits).
std::vector<uint8_t> encode_trigger(uint8_t epoch, uint16_t event, bool enter);
bool decode_trigger(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* event, bool* enter);

// Host -> client: a mod-linked mid-mission camera shot for the client (camera item and trigger names, 1-255 bytes each).
std::vector<uint8_t> encode_shot(uint8_t epoch, const std::string& camera, const std::string& trigger);
bool decode_shot(const uint8_t* data, size_t n, uint8_t* epoch, std::string* camera, std::string* trigger);

// Ghost co-op, reliable: a player's tow cable tripped the walker with this DAT item index; the other side trips its own copy.
std::vector<uint8_t> encode_tow_trip(uint8_t epoch, uint16_t item);
bool decode_tow_trip(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item);

// Ghost co-op: a player's tow cable for the other side to draw. Its state (cable state 0x8010B6E8: phase, head index, segment count) and, unless index is kNoSegment, one segment's first 16 words (start, 4x3 transform).
// The head segment goes every frame unreliably, a finished segment and every phase change reliably.
struct TowSeg {
    static constexpr uint8_t kNoSegment = 0xFF;
    uint8_t epoch = 0;
    uint8_t phase = 0;
    uint8_t head = 0;
    uint8_t count = 0;
    uint8_t index = kNoSegment;
    uint32_t words[16] = {};
};
std::vector<uint8_t> encode_tow_seg(const TowSeg& t);
bool decode_tow_seg(const uint8_t* data, size_t n, TowSeg* out);

// A received cable the renderer can use: its head inside the receiver's ring of segments, the segment's position and transform finite and in range.
bool tow_seg_ok(const TowSeg& t, uint32_t ring);

// Network compatibility of this build: bump it whenever two builds can no longer play together. The lobby's HELLO carries it.
constexpr uint32_t kBuildId = 0x5253000Cu;

// Lobby handshake, sent once by each side on connect. It decodes whatever the sender's protocol byte, so a mismatch can be reported.
struct Hello {
    uint16_t protocol = kProtocol;
    uint32_t build = kBuildId;

    // 0 host, 1 client.
    uint8_t role = 0;

    // Host: the game has a password (the client must run the key exchange).
    uint8_t locked = 0;

    // This connection's random half of the key exchange's session id.
    uint8_t nonce[kPakeNonce] = {};
};
std::vector<uint8_t> encode_hello(const Hello& h);
bool decode_hello(const uint8_t* data, size_t n, Hello* out);

// Sent by each side once connected (sealed in a password game): the pilot name (3 letters, gGameSettings+7), shown over this player's ship.
std::vector<uint8_t> encode_name(const char name[3]);
bool decode_name(const uint8_t* data, size_t n, char name[3]);

// The peer's pilot name as text to draw: up to its first NUL, printable characters only ('~' is the game font's line break).
std::string pilot_name(const char name[3]);

// Client to host after HELLO in a game without a password. The host answers ACCEPT, or BYE WrongPassword when it has one.
std::vector<uint8_t> encode_auth();
bool decode_auth(const uint8_t* data, size_t n);
std::vector<uint8_t> encode_accept();
bool decode_accept(const uint8_t* data, size_t n);

// The key exchange of a password game (net/crypto_session.h): PAKE1 client point, PAKE2 host point and tag, PAKE3 client tag.
std::vector<uint8_t> encode_pake(Msg type, const uint8_t* point, const uint8_t* tag);
bool decode_pake(const uint8_t* data, size_t n, Msg type, uint8_t* point, uint8_t* tag);

// This player pressed continue on the debrief; both leave it once each side has.
std::vector<uint8_t> encode_continue();
bool decode_continue(const uint8_t* data, size_t n);

// The lobby's IPv4 entry: four octets, shown without dots (menu font 5 has no '.'); while editing, the octet under the cursor is bracketed.
// Menu input as direction/button bits, independent of the pad layout.
constexpr uint32_t kPadUp = 1, kPadDown = 2, kPadLeft = 4, kPadRight = 8, kPadB = 16, kPadA = 32;

// LAN discovery, plain UDP on the game port + 1 (outside ENet): a joiner broadcasts a query, a host answers with its game port and pilot name.
constexpr uint16_t kDiscoveryPortOffset = 1;
struct DiscoveryReply {
    uint16_t port = 0;
    uint32_t build = kBuildId;
    char name[3] = {0, 0, 0};
};
std::vector<uint8_t> encode_discovery_query();
bool is_discovery_query(const uint8_t* data, size_t n);
std::vector<uint8_t> encode_discovery_reply(const DiscoveryReply& r);
bool decode_discovery_reply(const uint8_t* data, size_t n, DiscoveryReply* out);

// Whether a host answers a search from this address: private addresses only, and for a host bound to one address (bind != 0) only its /24. Host byte order.
bool discovery_allowed(uint32_t from, uint32_t bind);

// Lobby lines that mean "still trying" get dots appearing one at a time (kDotPeriodMs each, back to none after three), padded to a fixed width so the centred line stays put.
constexpr uint32_t kDotPeriodMs = 600;

// Pads the missing dots: a blank glyph as wide as the period (the menu font's Ö slot, made at runtime with the period; see ensure_menu_period).
constexpr char kDotPad = '`';

// The typing caret: a thin italic bar (the menu font's Ä slot, made at runtime from the stem of '!'; see ensure_menu_period).
constexpr char kCaret = '|';
bool waiting_status(const std::string& status);
std::string status_dots(const std::string& status, uint32_t now_ms);

// JOIN GAME's list: a relay's game list, kListRows games a page. count -1 = the search still runs.
constexpr int kListRows = 6;
int list_pages(int count);
std::string list_status(int count, int page);

// Internet hosting through UPnP: whether the router opened the game port. Players are never shown it; an open port only turns the address line into the NET address.
enum class PortMapState { Idle, Working, Open, NoRouter, Refused, SharedAddress };

// An address the internet can reach: not private, carrier-grade NAT (100.64/10), loopback or link-local. Host byte order.
bool public_ipv4(uint32_t ip);

// The host's address line: the internet address once the router opened the port, else the LAN one (lan_ip empty: no network). Addresses are dotted (the menu font's period is added at runtime, ensure_menu_period).
std::string host_address_label(PortMapState state, const std::string& lan_ip, const std::string& ext_ip, uint16_t port = 0);

// A relay's browser address, "ip:port#sha256" (the certificate hash a browser pins): the address and the hash (empty unless 64 hex digits).
void split_relay_hash(const std::string& s, std::string* addr, std::string* hash);
// The address part, as the address row shows it.
std::string relay_address(const std::string& s);

// HOST GAME's address: the row's IPv4 when it is one of this machine's or loopback (bind = shown = row), else bind any (0.0.0.0 asks for that) and show the fallback (0: none).
struct HostAddress {
    uint32_t bind = 0;
    uint32_t shown = 0;
};
HostAddress choose_host_address(std::optional<uint32_t> row, const std::vector<uint32_t>& local, std::optional<uint32_t> fallback);
std::string ipv4_text(uint32_t ip);

struct AddressEditor {
    uint8_t octet[4] = {192, 168, 1, 2};

    // A typed ":port" (0: none, the configured port applies).
    uint16_t port = 0;
    int cursor = 0;
    bool editing = false;
    void move(int dir);
    void step(int delta);

    // One menu frame while editing: up/down step the octet (held: every 3 frames after 12, by 10 from 45), left/right move, A or B end editing. True = the input was taken and the menu must not see it (every frame while editing).
    bool input(uint32_t pressed, uint32_t held);
    int held_frames = 0;

    // A saved address the octets cannot show (a hostname, IPv6) is kept and joined as-is until the player edits the octets.
    std::string saved;
    bool touched = false;
    void load(const std::string& s);

    // Keyboard entry (the phone's on-screen keyboard, or a desktop keyboard): an address or host name with an optional port, at most kMaxTyped, shown with a caret (kCaret); the pad only ends it (A or B).
    static constexpr size_t kMaxTyped = 64;
    bool typing = false;
    std::string typed;
    void begin_typing();
    void type_text(const char* s);
    void type_backspace();

    // Ends typing and editing; a typed address that parses replaces the octets and a typed host name becomes the saved address (true), anything else leaves them.
    bool end_typing();

    // Ends typing and editing without applying the typed text (the page closed mid-entry).
    void cancel_typing();

    // The address JOIN connects to, without the port.
    std::string target() const;

    // target() plus ":port" when one was typed (what roguesq_net.json keeps).
    std::string entry() const;

    // The row's octets, unless it holds a saved address they cannot show.
    std::optional<uint32_t> ipv4() const;
    std::string label() const;
    std::string text() const;

    // "a.b.c.d" or "a.b.c.d:port" (port 1-65535).
    static bool parse(const std::string& s, AddressEditor* out);
};

// The join code a relay transport's host shows (mp.transport host_label): kDigits digits. Pad: up/down step the digit under the cursor (wrapping), left/right move, A or B end editing. Keyboard: digits only; a complete code replaces it.
struct CodeEditor {
    static constexpr int kDigits = 6;
    char digit[kDigits] = {'0', '0', '0', '0', '0', '0'};

    // What the row shows before the digits (the same editor holds game passwords).
    const char* prefix = "JOIN CODE ";
    int cursor = 0;
    bool editing = false;
    bool typing = false;
    std::string typed;
    bool input(uint32_t pressed, uint32_t held);
    void begin_typing();
    void type_text(const char* s);
    void type_backspace();

    // Ends typing and editing; true when the typed text was a full code and replaced it.
    bool end_typing();
    void cancel_typing();
    void load(const std::string& s);
    std::string code() const;
    std::string label() const;
    static bool valid(const std::string& s);
};

// HOST SETTINGS' server name: menu-font text (A-Z, 0-9, single spaces), at most kMax. Keyboard types it (lower case goes upper); the pad's up/down step the character under the cursor (blank, A-Z, 0-9), left/right move, A or B end.
struct NameEditor {
    static constexpr int kMax = 16;
    std::string name;
    int cursor = 0;
    bool editing = false;
    bool typing = false;
    std::string typed;
    bool input(uint32_t pressed, uint32_t held);
    void begin_typing();
    void type_text(const char* s);
    void type_backspace();

    // Ends typing and editing; the typed text (cleaned) replaces the name. Always true.
    bool end_typing();
    void cancel_typing();

    // fallback: shown when the name is empty (the pilot's name, which a host with no name advertises).
    std::string label(const std::string& fallback = "") const;
    static std::string clean(const std::string& s);
};

// Lobby progress for one side: join gives up after kJoinTimeoutMs, a connected peer must finish the handshake within kHelloTimeoutMs, and a HELLO from another build fails a client. A host closes such a peer and keeps waiting, as it does when its player leaves.
// Without a password the client answers the host's HELLO with AUTH and is Connected on ACCEPT; with one they run CPace (PAKE1-3) and ACCEPT comes sealed. Messages queue in take_sends, the caller sends them before installing take_keys.
enum class LobbyState { Idle, Hosting, Joining, Connected, Failed };
class Lobby {
public:
    static constexpr uint32_t kJoinTimeoutMs = 15000;
    static constexpr uint32_t kHelloTimeoutMs = 5000;
    static constexpr uint32_t kLeftNoticeMs = 5000;
    void host();
    void join();
    void cancel();

    // The link could not start (the host's port is taken, no network).
    void on_start_failed();
    void on_connected(uint32_t now_ms);
    void on_hello(const Hello& h);

    // Host: the game's password. Client: the one typed for this game. Empty: none.
    void set_password(const std::string& pin) { password_ = pin; }

    // Host: the client's AUTH. False = refused because this game has a password (BYE WrongPassword is queued).
    bool on_auth();

    // The key exchange's messages. Host: PAKE1 counts a password guess, and during the wait after repeated wrong ones it is refused unanswered.
    void on_pake1(const uint8_t point[kPakePoint]);
    void on_pake2(const uint8_t point[kPakePoint], const uint8_t tag[kPakeTag]);
    void on_pake3(const uint8_t tag[kPakeTag]);
    std::vector<std::vector<uint8_t>> take_sends();
    bool take_keys(SessionKeys* out);

    // Host: whether the peer just refused must be disconnected; reading it clears it.
    bool take_kick()
    {

        const bool k = kick_;

        kick_ = false;
        return k;
    }

    // Client: the host accepted this player, or refused its password (sent: whether one was typed).
    void on_accept();
    void on_refused(bool sent);

    // Client: the host or relay server could not be reached (why: the transport's reason; empty: COULD NOT CONNECT TO HOST). Fails the join at once instead of at kJoinTimeoutMs.
    void on_join_failed(const std::string& why);

    // Either side: the session ended for a reason the status line keeps until the next host or join (a host whose relay server stopped answering).
    void fail(const std::string& why);
    bool password_failed() const { return state_ == LobbyState::Failed && pw_fail_; }
    void on_lost();
    void tick(uint32_t now_ms);
    LobbyState state() const { return state_; }
    bool hosting() const { return host_; }
    // Menu-font-safe status line: uppercase, digits, spaces.
    std::string status() const;
    bool ready() const { return state_ == LobbyState::Connected; }
    // The build this side compares HELLOs against and sends (tests override it to force a mismatch).
    void set_local_build(uint32_t b) { build_ = b; }
    // ROGUESQ_MP=join waits as long as the scripted path always did.
    void set_join_timeout(uint32_t ms) { join_timeout_ = ms; }
    uint32_t local_build() const { return build_; }
private:
    void reset(LobbyState s, bool host);
    void refuse();
    void connect_if_done();
    void queue(std::vector<uint8_t> b) { sends_.push_back(std::move(b)); }
    void client_fail(const char* why, bool password);
    std::vector<uint8_t> sid(const uint8_t* client, const uint8_t* host) const;
    LobbyState state_ = LobbyState::Idle;
    bool host_ = false;
    bool peer_ = false;

    // The peer's HELLO matched; the password step passed (host: AUTH checked, client: ACCEPT came).
    bool hello_ = false;
    bool auth_ = false;
    bool pw_fail_ = false;
    std::string password_;
    bool kick_ = false;
    uint8_t nonce_[kPakeNonce] = {};
    uint8_t peer_nonce_[kPakeNonce] = {};

    // The key exchange: started (client: PAKE1 sent, host: PAKE2 sent), and the keys once both tags checked.
    Cpace pake_;
    bool pake_started_ = false;
    bool confirmed_ = false;
    bool has_keys_ = false;
    SessionKeys keys_;
    std::vector<std::vector<uint8_t>> sends_;

    // Guesses since this host last accepted a password (each PAKE1 is one), and when it will next take one. since this host last accepted one, and when it will next check one.
    uint32_t wrong_ = 0;
    int64_t retry_ms_ = 0;
    int64_t started_ms_ = -1;
    int64_t connected_ms_ = -1;
    bool left_ = false;
    int64_t left_ms_ = -1;
    std::string fail_;
    uint32_t build_ = kBuildId;
    uint32_t join_timeout_ = kJoinTimeoutMs;
};

// The Msg of a packet with our protocol byte (any protocol byte for a HELLO), or 0.
uint8_t message_type(const uint8_t* data, size_t n);

// Whether a message of this type is read before the lobby handshake has finished.
bool handshake_message(uint8_t type);
}
