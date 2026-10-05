#include "net_core.h"
#include <algorithm>
#include <cstring>

namespace rs64::net {

namespace {

// Little-endian wire writer.
struct Writer {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((uint8_t)(v >> (8 * i))); }
};

// Every read is bounds-checked; a short read clears ok and returns 0.
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t at = 0;
    bool ok = true;
    bool need(size_t k) {
        if (!ok || n - at < k) {
            ok = false;
        }
        return ok;
    }
    uint8_t u8() { return need(1) ? p[at++] : 0; }
    uint16_t u16() {
        if (!need(2)) return 0;
        const uint16_t v = (uint16_t)(p[at] | (p[at + 1] << 8));
        at += 2;
        return v;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | p[at + i];
        at += 4;
        return v;
    }
    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | p[at + i];
        at += 8;
        return v;
    }
    bool done() const { return ok && at == n; }
};

bool header(Reader& r, Msg type) {
    if (!r.p || r.n > kMaxPacket) {
        return false;
    }
    const uint8_t version = r.u8();
    const uint8_t t = r.u8();
    return r.ok && version == kProtocol && t == (uint8_t)type;
}

std::vector<uint8_t> encode_pair(Msg type, uint8_t epoch, uint8_t value) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)type);
    w.u8(epoch);
    w.u8(value);
    return w.b;
}

bool decode_pair(const uint8_t* data, size_t n, Msg type, uint8_t max_value, uint8_t* epoch, uint8_t* value) {
    Reader r{data, n};
    if (!header(r, type)) {
        return false;
    }
    const uint8_t e = r.u8();
    const uint8_t v = r.u8();
    if (!r.done() || v > max_value) {
        return false;
    }
    *epoch = e;
    *value = v;
    return true;
}

}

std::vector<uint8_t> encode_result(uint8_t epoch, uint8_t result) {
    return encode_pair(Msg::Result, epoch, result);
}

bool decode_result(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* result) {
    return decode_pair(data, n, Msg::Result, 3, epoch, result);
}

std::vector<uint8_t> encode_presence(uint8_t epoch, uint8_t in_mission) {
    return encode_pair(Msg::Presence, epoch, in_mission);
}

bool decode_presence(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* in_mission) {
    return decode_pair(data, n, Msg::Presence, 1, epoch, in_mission);
}

uint8_t message_type(const uint8_t* data, size_t n) {
    if (!data || n < 2 || n > kMaxPacket) {
        return 0;
    }
    if (data[1] == (uint8_t)Msg::Hello) {
        return data[1];
    }
    if (data[0] != kProtocol || data[1] < 1 || data[1] > (uint8_t)Msg::TowTrip) {
        return 0;
    }
    return data[1];
}

std::vector<uint8_t> encode_hello(const Hello& h) {
    Writer w;
    w.u8((uint8_t)h.protocol);
    w.u8((uint8_t)Msg::Hello);
    w.u16(h.protocol);
    w.u32(h.build);
    w.u8(h.role);
    for (char c : h.name) {
        w.u8((uint8_t)c);
    }
    return w.b;
}

bool decode_hello(const uint8_t* data, size_t n, Hello* out) {
    Reader r{data, n};
    if (!r.p || r.n > kMaxPacket) {
        return false;
    }
    r.u8();
    if (r.u8() != (uint8_t)Msg::Hello) {
        return false;
    }
    Hello h;
    h.protocol = r.u16();
    h.build = r.u32();
    h.role = r.u8();
    for (char& c : h.name) {
        c = (char)r.u8();
    }
    if (!r.done() || h.role > 1) {
        return false;
    }
    *out = h;
    return true;
}

bool waiting_status(const std::string& status) {
    return status == "SEARCHING" || status == "CONNECTING" || status == "PLAYER JOINING" || status == "WAITING FOR PLAYER" || status == "OPENING INTERNET PORT";
}

std::string status_dots(const std::string& status, uint32_t now_ms) {
    const uint32_t n = (now_ms / kDotPeriodMs) % 4;
    return status + std::string(n, '.') + std::string(3 - n, kDotPad);
}

bool public_ipv4(uint32_t ip) {
    const uint32_t a = ip >> 24;
    const uint32_t b = (ip >> 16) & 0xFFu;
    return !(a == 0u || a == 10u || a == 127u || (a == 172u && (b & 0xF0u) == 16u) || (a == 192u && b == 168u) || (a == 100u && (b & 0xC0u) == 64u) || (a == 169u && b == 254u));
}

std::string online_label(PortMapState state) {
    switch (state) {
    case PortMapState::Idle:
        return "";
    case PortMapState::Working:
        return "OPENING INTERNET PORT";
    case PortMapState::Open:
        return "";
    case PortMapState::NoRouter:
        return "UPNP IS OFF ON YOUR ROUTER";
    case PortMapState::Refused:
        return "YOUR ROUTER REFUSED THE PORT";
    case PortMapState::SharedAddress:
        return "YOUR ISP SHARES YOUR ADDRESS";
    }
    return "";
}

std::string host_address_label(PortMapState state, const std::string& lan_ip, const std::string& ext_ip, uint16_t port) {
    const std::string suffix = port ? ":" + std::to_string(port) : "";
    if (state == PortMapState::Open && !ext_ip.empty()) {
        return "YOUR NET ADDRESS " + ext_ip + suffix;
    }
    return lan_ip.empty() ? "NO NETWORK ADDRESS" : "YOUR LAN ADDRESS " + lan_ip + suffix;
}

HostAddress choose_host_address(std::optional<uint32_t> row, const std::vector<uint32_t>& local, std::optional<uint32_t> fallback) {
    HostAddress h;
    if (row && *row != 0u && ((*row >> 24) == 127u || std::find(local.begin(), local.end(), *row) != local.end())) {
        h.bind = *row;
        h.shown = *row;
        return h;
    }
    h.shown = fallback.value_or(0u);
    return h;
}

std::string ipv4_text(uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255u) + "." + std::to_string((ip >> 8) & 255u) + "." + std::to_string(ip & 255u);
}

// "1-65535" only: no sign, no leading junk.
static bool parse_port(const std::string& s, uint16_t* out) {
    if (s.empty() || s.size() > 5 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    const int v = std::stoi(s);
    if (v < 1 || v > 65535) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

static const uint8_t kDiscoveryMagic[4] = {'R', 'S', '6', '4'};
enum : uint8_t { kDiscoveryQuery = 'Q', kDiscoveryReply = 'R' };

static bool discovery_header(Reader& r, uint8_t kind) {
    for (uint8_t m : kDiscoveryMagic) {
        if (r.u8() != m) {
            return false;
        }
    }
    return r.u8() == kind;
}

std::vector<uint8_t> encode_discovery_query() {
    Writer w;
    for (uint8_t m : kDiscoveryMagic) {
        w.u8(m);
    }
    w.u8(kDiscoveryQuery);
    return w.b;
}

bool is_discovery_query(const uint8_t* data, size_t n) {
    Reader r{data, n};
    return r.p && discovery_header(r, kDiscoveryQuery) && r.done();
}

std::vector<uint8_t> encode_discovery_reply(const DiscoveryReply& d) {
    Writer w;
    for (uint8_t m : kDiscoveryMagic) {
        w.u8(m);
    }
    w.u8(kDiscoveryReply);
    w.u16(d.port);
    w.u32(d.build);
    for (char c : d.name) {
        w.u8((uint8_t)c);
    }
    return w.b;
}

bool decode_discovery_reply(const uint8_t* data, size_t n, DiscoveryReply* out) {
    Reader r{data, n};
    if (!r.p || !discovery_header(r, kDiscoveryReply)) {
        return false;
    }
    DiscoveryReply d;
    d.port = r.u16();
    d.build = r.u32();
    for (char& c : d.name) {
        c = (char)r.u8();
    }
    if (!r.done()) {
        return false;
    }
    *out = d;
    return true;
}

void AddressEditor::move(int dir) {
    cursor = std::clamp(cursor + dir, 0, 3);
}

void AddressEditor::step(int delta) {
    octet[cursor] = (uint8_t)std::clamp((int)octet[cursor] + delta, 0, 255);
}

bool AddressEditor::input(uint32_t pressed, uint32_t held) {
    if (!editing) {
        return false;
    }
    touched = true;
    if (pressed & (kPadA | kPadB)) {
        if (typing) {
            end_typing();
        }
        editing = false;
        held_frames = 0;
        return true;
    }
    if (typing) {
        return true;
    }
    if (pressed & kPadLeft) {
        move(-1);
    }
    if (pressed & kPadRight) {
        move(+1);
    }
    // A light stick flick sets the pressed bit without reaching the held threshold.
    const uint32_t ud = held | pressed;
    const int dir = (ud & kPadUp) ? +1 : (ud & kPadDown) ? -1 : 0;
    if (dir == 0) {
        held_frames = 0;
    } else if (pressed & (kPadUp | kPadDown)) {
        held_frames = 1;
        step(dir);
    } else if (++held_frames >= 12 && (held_frames - 12) % 3 == 0) {
        step(held_frames >= 45 ? dir * 10 : dir);
    }
    return true;
}

void AddressEditor::load(const std::string& s) {
    saved.clear();
    touched = false;
    port = 0;
    if (s.empty() || parse(s, this)) {
        return;
    }
    // A hostname may carry ":port"; an IPv6 literal (several colons) is kept whole.
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos && s.find(':') == colon && colon > 0 && parse_port(s.substr(colon + 1), &port)) {
        saved = s.substr(0, colon);
        return;
    }
    saved = s;
}

std::string AddressEditor::target() const {
    if (!touched && !saved.empty()) {
        return saved;
    }
    return std::to_string(octet[0]) + "." + std::to_string(octet[1]) + "." + std::to_string(octet[2]) + "." + std::to_string(octet[3]);
}

std::string AddressEditor::entry() const {
    return target() + (port ? ":" + std::to_string(port) : "");
}

std::optional<uint32_t> AddressEditor::ipv4() const {
    if (!touched && !saved.empty()) {
        return std::nullopt;
    }
    return ((uint32_t)octet[0] << 24) | ((uint32_t)octet[1] << 16) | ((uint32_t)octet[2] << 8) | octet[3];
}

void AddressEditor::begin_typing() {
    typing = true;
    typed = text();
}

void AddressEditor::type_text(const char* s) {
    for (; s && *s; ++s) {
        if (((*s >= '0' && *s <= '9') || *s == '.' || *s == ':') && typed.size() < 21) {
            typed += *s;
        }
    }
}

void AddressEditor::type_backspace() {
    if (!typed.empty()) {
        typed.pop_back();
    }
}

void AddressEditor::cancel_typing() {
    typing = false;
    editing = false;
    held_frames = 0;
    typed.clear();
}

bool AddressEditor::end_typing() {
    typing = false;
    editing = false;
    held_frames = 0;
    AddressEditor parsed;
    if (typed.empty() || !parse(typed, &parsed)) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        octet[i] = parsed.octet[i];
    }
    port = parsed.port;
    saved.clear();
    touched = true;
    return true;
}

std::string AddressEditor::label() const {
    if (typing) {
        return typed + kCaret;
    }
    if (!touched && !saved.empty()) {
        return "SAVED ADDRESS";
    }
    std::string s;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            s += '.';
        }
        const bool mark = editing && i == cursor;
        s += (mark ? "<" : "") + std::to_string(octet[i]) + (mark ? ">" : "");
    }
    return port ? s + ":" + std::to_string(port) : s;
}

bool CodeEditor::input(uint32_t pressed, uint32_t held) {
    (void)held;
    if (!editing) {
        return false;
    }
    if (pressed & (kPadA | kPadB)) {
        if (typing) {
            end_typing();
        }
        editing = false;
        return true;
    }
    if (typing) {
        return true;
    }
    if (pressed & kPadLeft) {
        cursor = std::max(0, cursor - 1);
    }
    if (pressed & kPadRight) {
        cursor = std::min(kDigits - 1, cursor + 1);
    }
    const int dir = (pressed & kPadUp) ? 1 : (pressed & kPadDown) ? 9 : 0;
    digit[cursor] = (char)('0' + (digit[cursor] - '0' + dir) % 10);
    return true;
}

void CodeEditor::begin_typing() {
    typing = true;
    typed = code();
}

void CodeEditor::type_text(const char* s) {
    for (; s && *s; ++s) {
        if (*s >= '0' && *s <= '9' && typed.size() < (size_t)kDigits) {
            typed += *s;
        }
    }
}

void CodeEditor::type_backspace() {
    if (!typed.empty()) {
        typed.pop_back();
    }
}

bool CodeEditor::end_typing() {
    typing = false;
    editing = false;
    const bool ok = valid(typed);
    if (ok) {
        load(typed);
    }
    typed.clear();
    return ok;
}

void CodeEditor::cancel_typing() {
    typing = false;
    editing = false;
    typed.clear();
}

void CodeEditor::load(const std::string& s) {
    if (valid(s)) {
        std::copy(s.begin(), s.end(), digit);
    }
}

std::string CodeEditor::code() const {
    return std::string(digit, digit + kDigits);
}

std::string CodeEditor::label() const {
    if (typing) {
        return typed + kCaret;
    }
    std::string s = "JOIN CODE ";
    for (int i = 0; i < kDigits; ++i) {
        const bool mark = editing && i == cursor;
        s += mark ? std::string("<") + digit[i] + ">" : std::string(1, digit[i]);
    }
    return s;
}

bool CodeEditor::valid(const std::string& s) {
    return s.size() == (size_t)kDigits && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string AddressEditor::text() const {
    const std::string a = std::to_string(octet[0]) + "." + std::to_string(octet[1]) + "." + std::to_string(octet[2]) + "." + std::to_string(octet[3]);
    return port ? a + ":" + std::to_string(port) : a;
}

bool AddressEditor::parse(const std::string& in, AddressEditor* out) {
    const size_t colon = in.find(':');
    uint16_t p = 0;
    if (colon != std::string::npos && !parse_port(in.substr(colon + 1), &p)) {
        return false;
    }
    const std::string s = in.substr(0, colon);
    uint8_t o[4];
    size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        size_t end = at;
        while (end < s.size() && s[end] >= '0' && s[end] <= '9' && end - at < 3) {
            ++end;
        }
        if (end == at || (i < 3 ? (end >= s.size() || s[end] != '.') : end != s.size())) {
            return false;
        }
        const int v = std::stoi(s.substr(at, end - at));
        if (v > 255) {
            return false;
        }
        o[i] = (uint8_t)v;
        at = end + 1;
    }
    memcpy(out->octet, o, 4);
    out->port = p;
    return true;
}

std::vector<uint8_t> encode_launch(const std::string& header_text) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Launch);
    const size_t len = header_text.size() < kMaxPacket - 4 ? header_text.size() : kMaxPacket - 4;
    w.u16((uint16_t)len);
    for (size_t i = 0; i < len; ++i) w.u8((uint8_t)header_text[i]);
    return w.b;
}

bool decode_launch(const uint8_t* data, size_t n, std::string* header_text) {
    Reader r{data, n};
    if (!header(r, Msg::Launch)) {
        return false;
    }
    const uint16_t len = r.u16();
    if (!r.need(len)) {
        return false;
    }
    std::string s((const char*)data + r.at, len);
    r.at += len;
    if (!r.done()) {
        return false;
    }
    *header_text = std::move(s);
    return true;
}

LinkEdges link_poll_edges(bool up, bool was_up, uint32_t connects, uint32_t seen_connects) {
    LinkEdges e;
    e.connected = up && connects != seen_connects;
    e.lost = was_up && (!up || e.connected);
    return e;
}

std::vector<uint8_t> encode_bye(ByeReason reason, uint32_t frame) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Bye);
    w.u8((uint8_t)reason);
    w.u32(frame);
    return w.b;
}

bool decode_bye(const uint8_t* data, size_t n, ByeReason* reason, uint32_t* frame) {
    Reader r{data, n};
    if (!header(r, Msg::Bye)) {
        return false;
    }
    const uint8_t v = r.u8();
    const uint32_t f = r.u32();
    if (!r.done() || v > (uint8_t)ByeReason::Mismatch) {
        return false;
    }
    *reason = (ByeReason)v;
    *frame = f;
    return true;
}

std::vector<uint8_t> encode_state(const StatePacket& s) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::State);
    w.u32(s.seq);
    w.u8(s.epoch);
    auto f = [&w](float v) {
        uint32_t b;
        memcpy(&b, &v, 4);
        w.u32(b);
    };
    for (int i = 0; i < 3; ++i) f(s.pos[i]);
    for (int i = 0; i < 3; ++i) f(s.fwd[i]);
    for (int i = 0; i < 3; ++i) f(s.down[i]);
    for (int i = 0; i < 3; ++i) f(s.vel[i]);
    for (int i = 0; i < 6; ++i) w.u8(s.pad[i]);
    w.u8(s.flags);
    w.u8(s.foil);
    w.u32(s.sent_ms);
    return w.b;
}

bool decode_state(const uint8_t* data, size_t n, StatePacket* out) {
    Reader r{data, n};
    if (!header(r, Msg::State)) {
        return false;
    }
    StatePacket s;
    s.seq = r.u32();
    s.epoch = r.u8();
    auto f = [&r]() {
        const uint32_t b = r.u32();
        float v;
        memcpy(&v, &b, 4);
        return v;
    };
    for (int i = 0; i < 3; ++i) s.pos[i] = f();
    for (int i = 0; i < 3; ++i) s.fwd[i] = f();
    for (int i = 0; i < 3; ++i) s.down[i] = f();
    for (int i = 0; i < 3; ++i) s.vel[i] = f();
    for (int i = 0; i < 6; ++i) s.pad[i] = r.u8();
    s.flags = r.u8();
    s.foil = r.u8();
    s.sent_ms = r.u32();
    if (!r.done() || s.foil > 250) {
        return false;
    }
    *out = s;
    return true;
}

std::vector<uint8_t> encode_obj(const ObjSnapshot& s) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Obj);
    w.u8(s.epoch);
    w.u32(s.seq);
    for (int i = 0; i < 128; ++i) w.u8(s.bools[i]);
    for (int i = 0; i < 128; ++i) w.u32((uint32_t)s.counts[i]);
    for (int i = 0; i < 8; ++i) {
        uint32_t b;
        memcpy(&b, &s.timers[i], 4);
        w.u32(b);
    }
    w.u8(s.lives);
    return w.b;
}

std::vector<uint8_t> encode_mission(uint8_t id) {
    return encode_pair(Msg::Mission, id, 0);
}

bool decode_mission(const uint8_t* data, size_t n, uint8_t* id) {
    uint8_t v = 0;
    return decode_pair(data, n, Msg::Mission, 0, id, &v);
}

std::vector<uint8_t> encode_life_lost(uint8_t epoch) {
    return encode_pair(Msg::Life, epoch, 1);
}

bool decode_life_lost(const uint8_t* data, size_t n, uint8_t* epoch) {
    uint8_t v = 0;
    return decode_pair(data, n, Msg::Life, 1, epoch, &v);
}

std::vector<uint8_t> encode_pick(uint8_t level) {
    return encode_pair(Msg::Pick, 0, level);
}

bool decode_pick(const uint8_t* data, size_t n, uint8_t* level) {
    uint8_t e = 0;
    return decode_pair(data, n, Msg::Pick, 0x12, &e, level);
}

std::vector<uint8_t> encode_browse(uint8_t level, uint8_t state) {
    return encode_pair(Msg::Browse, level, state);
}

bool decode_browse(const uint8_t* data, size_t n, uint8_t* level, uint8_t* state) {
    uint8_t l = 0;
    uint8_t st = 0;
    if (!decode_pair(data, n, Msg::Browse, 4, &l, &st) || l > 0x12) {
        return false;
    }
    *level = l;
    *state = st;
    return true;
}

static std::vector<uint8_t> encode_bare(Msg type) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)type);
    return w.b;
}

static bool decode_bare(const uint8_t* data, size_t n, Msg type) {
    Reader r{data, n};
    if (!r.p) {
        return false;
    }
    r.u8();
    return r.u8() == (uint8_t)type && r.done();
}

std::vector<uint8_t> encode_skip() {
    return encode_bare(Msg::Skip);
}

bool decode_skip(const uint8_t* data, size_t n) {
    return decode_bare(data, n, Msg::Skip);
}

std::vector<uint8_t> encode_briefing_done() {
    return encode_bare(Msg::BriefingDone);
}

bool decode_briefing_done(const uint8_t* data, size_t n) {
    return decode_bare(data, n, Msg::BriefingDone);
}

std::vector<uint8_t> encode_ready(uint8_t craft) {
    return encode_pair(Msg::Ready, 0, craft);
}

bool decode_ready(const uint8_t* data, size_t n, uint8_t* craft) {
    uint8_t e = 0;
    uint8_t c = 0;
    if (!decode_pair(data, n, Msg::Ready, 0xFF, &e, &c) || (c > 8 && c != 0xFF)) {
        return false;
    }
    *craft = c;
    return true;
}

std::vector<uint8_t> encode_out(uint8_t epoch) {
    return encode_pair(Msg::Out, epoch, 1);
}

std::vector<uint8_t> encode_upgrades(uint8_t epoch, uint32_t bits) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Upgrades);
    w.u8(epoch);
    w.u32(bits);
    return w.b;
}

std::vector<uint8_t> encode_pickup(uint8_t epoch, uint16_t item) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Pickup);
    w.u8(epoch);
    w.u16(item);
    return w.b;
}

bool decode_pickup(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item) {
    Reader r{data, n};
    if (!header(r, Msg::Pickup)) {
        return false;
    }
    const uint8_t e = r.u8();
    const uint16_t i = r.u16();
    if (!r.done()) {
        return false;
    }
    *epoch = e;
    *item = i;
    return true;
}

std::vector<uint8_t> encode_trigger(uint8_t epoch, uint16_t event, bool enter) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Trigger);
    w.u8(epoch);
    w.u16(event);
    w.u8(enter ? 1 : 0);
    return w.b;
}

bool decode_trigger(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* event, bool* enter) {
    Reader r{data, n};
    if (!header(r, Msg::Trigger)) {
        return false;
    }
    const uint8_t e = r.u8();
    const uint16_t ev = r.u16();
    const uint8_t in = r.u8();
    if (!r.done() || ev > 0xFFFu || in > 1) {
        return false;
    }
    *epoch = e;
    *event = ev;
    *enter = in == 1;
    return true;
}

std::vector<uint8_t> encode_tow_trip(uint8_t epoch, uint16_t item) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::TowTrip);
    w.u8(epoch);
    w.u16(item);
    return w.b;
}

bool decode_tow_trip(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item) {
    Reader r{data, n};
    if (!header(r, Msg::TowTrip)) {
        return false;
    }
    const uint8_t e = r.u8();
    const uint16_t i = r.u16();
    if (!r.done()) {
        return false;
    }
    *epoch = e;
    *item = i;
    return true;
}

bool decode_upgrades(const uint8_t* data, size_t n, uint8_t* epoch, uint32_t* bits) {
    Reader r{data, n};
    if (!header(r, Msg::Upgrades)) {
        return false;
    }
    const uint8_t e = r.u8();
    const uint32_t b = r.u32();
    if (!r.done() || b == 0 || (b & ~kUpgradeMask) != 0) {
        return false;
    }
    *epoch = e;
    *bits = b;
    return true;
}

bool decode_out(const uint8_t* data, size_t n, uint8_t* epoch) {
    uint8_t v = 0;
    return decode_pair(data, n, Msg::Out, 1, epoch, &v);
}

bool decode_obj(const uint8_t* data, size_t n, ObjSnapshot* out) {
    Reader r{data, n};
    if (!header(r, Msg::Obj)) {
        return false;
    }
    ObjSnapshot s;
    s.epoch = r.u8();
    s.seq = r.u32();
    for (int i = 0; i < 128; ++i) s.bools[i] = r.u8();
    for (int i = 0; i < 128; ++i) s.counts[i] = (int32_t)r.u32();
    for (int i = 0; i < 8; ++i) {
        const uint32_t b = r.u32();
        memcpy(&s.timers[i], &b, 4);
    }
    s.lives = r.u8();
    if (!r.done()) {
        return false;
    }
    *out = s;
    return true;
}

std::vector<uint8_t> encode_damage(uint8_t epoch, const std::vector<std::pair<uint16_t, int32_t>>& items) {
    Writer w;
    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Damage);
    w.u8(epoch);
    const size_t n = items.size() < kDamageBatch ? items.size() : kDamageBatch;
    w.u8((uint8_t)n);
    for (size_t i = 0; i < n; ++i) {
        w.u16(items[i].first);
        w.u32((uint32_t)items[i].second);
    }
    return w.b;
}

bool decode_damage(const uint8_t* data, size_t n, uint8_t* epoch, std::vector<std::pair<uint16_t, int32_t>>* items) {
    Reader r{data, n};
    if (!header(r, Msg::Damage)) {
        return false;
    }
    const uint8_t e = r.u8();
    const uint8_t count = r.u8();
    if (!r.ok || count == 0 || count > kDamageBatch) {
        return false;
    }
    std::vector<std::pair<uint16_t, int32_t>> out;
    for (uint8_t i = 0; i < count; ++i) {
        const uint16_t idx = r.u16();
        const int32_t hp = (int32_t)r.u32();
        out.emplace_back(idx, hp);
    }
    if (!r.done()) {
        return false;
    }
    *epoch = e;
    *items = std::move(out);
    return true;
}

void Lobby::reset(LobbyState s, bool host) {
    state_ = s;
    host_ = host;
    peer_ = false;
    started_ms_ = -1;
    connected_ms_ = -1;
    left_ = false;
    left_ms_ = -1;
    fail_.clear();
}

void Lobby::host() {
    reset(LobbyState::Hosting, true);
}

void Lobby::join() {
    reset(LobbyState::Joining, false);
}

void Lobby::cancel() {
    reset(LobbyState::Idle, false);
}

void Lobby::on_start_failed() {
    state_ = LobbyState::Failed;
    fail_ = host_ ? "CANNOT HOST" : "NETWORK ERROR";
}

void Lobby::on_connected(uint32_t now_ms) {
    if (state_ != LobbyState::Hosting && state_ != LobbyState::Joining) {
        return;
    }
    peer_ = true;
    connected_ms_ = now_ms;
    left_ = false;
}

void Lobby::on_hello(const Hello& h) {
    if (!peer_ || (state_ != LobbyState::Hosting && state_ != LobbyState::Joining)) {
        return;
    }
    if (h.protocol != kProtocol || h.build != build_) {
        state_ = LobbyState::Failed;
        fail_ = "VERSION MISMATCH";
        return;
    }
    state_ = LobbyState::Connected;
}

void Lobby::on_lost() {
    if (state_ == LobbyState::Idle || state_ == LobbyState::Failed) {
        return;
    }
    if (host_) {
        reset(LobbyState::Hosting, true);
        left_ = true;
        return;
    }
    if (state_ == LobbyState::Connected || (state_ == LobbyState::Joining && peer_)) {
        state_ = LobbyState::Failed;
        fail_ = "CONNECTION LOST";
    }
}

void Lobby::tick(uint32_t now_ms) {
    if (started_ms_ < 0) {
        started_ms_ = now_ms;
    }
    if (left_ && left_ms_ < 0) {
        left_ms_ = now_ms;
    }
    if (left_ && (int64_t)now_ms - left_ms_ > kLeftNoticeMs) {
        left_ = false;
        left_ms_ = -1;
    }
    if (state_ == LobbyState::Joining && !peer_ && (int64_t)now_ms - started_ms_ > join_timeout_) {
        state_ = LobbyState::Failed;
        fail_ = "NO ANSWER";
    }
    if ((state_ == LobbyState::Joining || state_ == LobbyState::Hosting) && peer_ && (int64_t)now_ms - connected_ms_ > kHelloTimeoutMs) {
        state_ = LobbyState::Failed;
        fail_ = "NO HELLO";
    }
}

std::string Lobby::status() const {
    switch (state_) {
    case LobbyState::Idle:
        return "HOST OR JOIN A GAME";
    case LobbyState::Hosting:
        if (peer_) {
            return "PLAYER JOINING";
        }
        return left_ ? "PLAYER LEFT" : "WAITING FOR PLAYER";
    case LobbyState::Joining:
        return "CONNECTING";
    case LobbyState::Connected:
        return "CONNECTED";
    case LobbyState::Failed:
        return fail_;
    }
    return "";
}

}
