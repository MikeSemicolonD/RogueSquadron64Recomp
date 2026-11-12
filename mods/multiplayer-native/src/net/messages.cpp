// The session's messages: [kProtocol, Msg, fields...], little-endian (net/wire.h). Most are a few scalars and fixed arrays through encode_fields / decode_fields.
#include "net/net_core.h"
#include "net/wire.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rs64::net {

namespace {

bool header(Reader& r, Msg type)
{

    if (!r.p || r.n > kMaxPacket)
    {
        return false;
    }

    const uint8_t version = r.u8();
    const uint8_t t = r.u8();
    return r.ok && version == kProtocol && t == (uint8_t)type;
}

template <class... F>
std::vector<uint8_t> encode_fields(Msg type, const F&... fields)
{

    Writer w;

    w.u8(kProtocol);
    w.u8((uint8_t)type);
    put_all(w, fields...);
    return w.b;
}

// Into the caller's temporaries (fixed arrays included); the caller commits them when this returns true.
template <class... F>
bool read_fields(const uint8_t* data, size_t n, Msg type, F*... out)
{

    Reader r{data, n};

    return header(r, type) && take_all(r, out...);
}

// Scalar fields: the outputs change only when the whole message decoded.
template <class... F>
bool decode_fields(const uint8_t* data, size_t n, Msg type, F*... out)
{

    Reader r{data, n};

    return header(r, type) && take_all_committed(r, out...);
}
}

uint8_t message_type(const uint8_t* data, size_t n)
{

    if (!data || n < 2 || n > kMaxPacket)
    {
        return 0;
    }

    if (data[1] == (uint8_t)Msg::Hello)
    {
        return data[1];
    }

    if (data[0] != kProtocol || data[1] < 1 || data[1] > (uint8_t)Msg::Rules)
    {
        return 0;
    }

    return data[1];
}

bool handshake_message(uint8_t type)
{

    switch ((Msg)type) {
    case Msg::Hello:
    case Msg::Auth:
    case Msg::Accept:
    case Msg::Bye:
    case Msg::Pake1:
    case Msg::Pake2:
    case Msg::Pake3:
        return true;
    default:
        return false;
    }
}

// HELLO leads with the sender's protocol, not ours, so a mismatch decodes and can be reported.
std::vector<uint8_t> encode_hello(const Hello& h)
{

    Writer w;

    w.u8((uint8_t)h.protocol);
    w.u8((uint8_t)Msg::Hello);
    w.u16(h.protocol);
    w.u32(h.build);
    w.u8(h.role);
    w.u8(h.locked);

    for (uint8_t c : h.nonce) w.u8(c);

    return w.b;
}

bool decode_hello(const uint8_t* data, size_t n, Hello* out)
{

    Reader r{data, n};

    if (!r.p || r.n > kMaxPacket)
    {
        return false;
    }

    r.u8();

    if (r.u8() != (uint8_t)Msg::Hello)
    {
        return false;
    }

    Hello h;

    h.protocol = r.u16();
    h.build = r.u32();
    h.role = r.u8();
    h.locked = r.u8();

    for (uint8_t& c : h.nonce) c = r.u8();

    if (!r.done() || h.role > 1 || h.locked > 1)
    {
        return false;
    }

    *out = h;
    return true;
}

std::vector<uint8_t> encode_name(const char name[3])
{

    Writer w;

    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Name);

    for (int i = 0; i < 3; ++i) w.u8((uint8_t)name[i]);

    return w.b;
}

bool decode_name(const uint8_t* data, size_t n, char name[3])
{

    Reader r{data, n};
    char c[3];

    if (!header(r, Msg::Name))
    {
        return false;
    }

    for (char& x : c) x = (char)r.u8();

    if (!r.done())
    {
        return false;
    }

    memcpy(name, c, 3);
    return true;
}

std::vector<uint8_t> encode_pake(Msg type, const uint8_t* point, const uint8_t* tag)
{

    Writer w;

    w.u8(kProtocol);
    w.u8((uint8_t)type);

    if (type != Msg::Pake3) {
        for (size_t i = 0; i < kPakePoint; ++i) w.u8(point[i]);
    }

    if (type != Msg::Pake1) {
        for (size_t i = 0; i < kPakeTag; ++i) w.u8(tag[i]);
    }

    return w.b;
}

bool decode_pake(const uint8_t* data, size_t n, Msg type, uint8_t* point, uint8_t* tag)
{

    Reader r{data, n};
    uint8_t p[kPakePoint] = {};
    uint8_t t[kPakeTag] = {};

    if (!header(r, type) || (type != Msg::Pake1 && type != Msg::Pake2 && type != Msg::Pake3))
    {
        return false;
    }

    if (type != Msg::Pake3) {
        for (uint8_t& c : p) c = r.u8();
    }

    if (type != Msg::Pake1) {
        for (uint8_t& c : t) c = r.u8();
    }

    if (!r.done())
    {
        return false;
    }

    if (point) {
        memcpy(point, p, sizeof(p));
    }

    if (tag) {
        memcpy(tag, t, sizeof(t));
    }

    return true;
}

std::string pilot_name(const char name[3])
{

    std::string out;

    for (int i = 0; i < 3; ++i) {
        const char c = name[i];

        if (!c)
        {
            break;
        }

        if (c >= 0x20 && c < '~') {
            out += c;
        }
    }

    return out;
}

std::vector<uint8_t> encode_launch(const std::string& header_text)
{

    Writer w;

    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Launch);
    put_string<uint16_t>(w, header_text, kMaxPacket - 4 - kSealOverhead);
    return w.b;
}

bool decode_launch(const uint8_t* data, size_t n, std::string* header_text)
{

    Reader r{data, n};
    std::string s;

    if (!header(r, Msg::Launch) || !take_string<uint16_t>(r, &s, kMaxPacket) || !r.done())
    {
        return false;
    }

    *header_text = std::move(s);
    return true;
}

std::vector<uint8_t> encode_shot(uint8_t epoch, const std::string& camera, const std::string& trigger)
{

    Writer w;

    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Shot);
    w.u8(epoch);
    put_string<uint8_t>(w, camera, 255);
    put_string<uint8_t>(w, trigger, 255);
    return w.b;
}

bool decode_shot(const uint8_t* data, size_t n, uint8_t* epoch, std::string* camera, std::string* trigger)
{

    Reader r{data, n};

    if (!header(r, Msg::Shot))
    {
        return false;
    }

    const uint8_t e = r.u8();
    std::string names[2];

    for (std::string& s : names) {
        if (!take_string<uint8_t>(r, &s, 255) || s.empty())
        {
            return false;
        }
    }

    if (!r.done())
    {
        return false;
    }

    *epoch = e;
    *camera = std::move(names[0]);
    *trigger = std::move(names[1]);
    return true;
}

std::vector<uint8_t> encode_damage(uint8_t epoch, const std::vector<std::pair<uint16_t, int32_t>>& items)
{

    Writer w;
    const size_t n = std::min(items.size(), kDamageBatch);

    w.u8(kProtocol);
    w.u8((uint8_t)Msg::Damage);
    w.u8(epoch);
    w.u8((uint8_t)n);

    for (size_t i = 0; i < n; ++i) {
        put(w, items[i].first);
        put(w, items[i].second);
    }

    return w.b;
}

bool decode_damage(const uint8_t* data, size_t n, uint8_t* epoch, std::vector<std::pair<uint16_t, int32_t>>* items)
{

    Reader r{data, n};

    if (!header(r, Msg::Damage))
    {
        return false;
    }

    const uint8_t e = r.u8();
    const uint8_t count = r.u8();

    if (!r.ok || count == 0 || count > kDamageBatch)
    {
        return false;
    }

    std::vector<std::pair<uint16_t, int32_t>> out(count);

    for (auto& [idx, hp] : out) {
        take(r, &idx);
        take(r, &hp);
    }

    if (!r.done())
    {
        return false;
    }

    *epoch = e;
    *items = std::move(out);
    return true;
}

std::vector<uint8_t> encode_state(const StatePacket& s)
{
    return encode_fields(Msg::State, s.seq, s.epoch, s.pos, s.fwd, s.down, s.vel, s.pad, s.flags, s.foil, s.sent_ms);
}

bool decode_state(const uint8_t* data, size_t n, StatePacket* out)
{

    StatePacket s;

    if (!read_fields(data, n, Msg::State, &s.seq, &s.epoch, &s.pos, &s.fwd, &s.down, &s.vel, &s.pad, &s.flags, &s.foil, &s.sent_ms) || s.foil > 250)
    {
        return false;
    }

    *out = s;
    return true;
}

std::vector<uint8_t> encode_obj(const ObjSnapshot& s)
{
    return encode_fields(Msg::Obj, s.epoch, s.seq, s.bools, s.counts, s.timers, s.lives);
}

bool decode_obj(const uint8_t* data, size_t n, ObjSnapshot* out)
{

    ObjSnapshot s;

    if (!read_fields(data, n, Msg::Obj, &s.epoch, &s.seq, &s.bools, &s.counts, &s.timers, &s.lives))
    {
        return false;
    }

    *out = s;
    return true;
}

std::vector<uint8_t> encode_tow_seg(const TowSeg& t)
{

    if (t.index == TowSeg::kNoSegment)
    {
        return encode_fields(Msg::TowSeg, t.epoch, t.phase, t.head, t.count, t.index);
    }

    return encode_fields(Msg::TowSeg, t.epoch, t.phase, t.head, t.count, t.index, t.words);
}

bool decode_tow_seg(const uint8_t* data, size_t n, TowSeg* out)
{

    TowSeg t;
    const bool state_only = n == 7;
    const bool ok = state_only ? read_fields(data, n, Msg::TowSeg, &t.epoch, &t.phase, &t.head, &t.count, &t.index) : read_fields(data, n, Msg::TowSeg, &t.epoch, &t.phase, &t.head, &t.count, &t.index, &t.words);

    if (!ok || state_only != (t.index == TowSeg::kNoSegment))
    {
        return false;
    }

    *out = t;
    return true;
}

bool tow_seg_ok(const TowSeg& t, uint32_t ring)
{

    if (t.head >= ring)
    {
        return false;
    }

    if (t.index == TowSeg::kNoSegment)
    {
        return true;
    }

    for (int i = 0; i < 16; ++i)
    {
        // +0xC sits between the start position and the transform and is not a coordinate.
        if (i == 3) {
            continue;
        }

        float v;

        std::memcpy(&v, &t.words[i], 4);

        if (!std::isfinite(v) || std::fabs(v) > 1e6f)
        {
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> encode_bye(ByeReason reason, uint32_t frame) { return encode_fields(Msg::Bye, (uint8_t)reason, frame); }

bool decode_bye(const uint8_t* data, size_t n, ByeReason* reason, uint32_t* frame)
{

    uint8_t v = 0;

    if (!decode_fields(data, n, Msg::Bye, &v, frame) || v > (uint8_t)ByeReason::WrongPassword)
    {
        return false;
    }

    *reason = (ByeReason)v;
    return true;
}

std::vector<uint8_t> encode_result(uint8_t epoch, uint8_t result) { return encode_fields(Msg::Result, epoch, result); }

bool decode_result(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* result)
{

    uint8_t e = 0;
    uint8_t v = 0;

    if (!decode_fields(data, n, Msg::Result, &e, &v) || v > 3)
    {
        return false;
    }

    *epoch = e;
    *result = v;
    return true;
}

std::vector<uint8_t> encode_presence(uint8_t epoch, uint8_t in_mission) { return encode_fields(Msg::Presence, epoch, in_mission); }

bool decode_presence(const uint8_t* data, size_t n, uint8_t* epoch, uint8_t* in_mission)
{

    uint8_t e = 0;
    uint8_t v = 0;

    if (!decode_fields(data, n, Msg::Presence, &e, &v) || v > 1)
    {
        return false;
    }

    *epoch = e;
    *in_mission = v;
    return true;
}

// MISSION, LIFE and OUT keep the [epoch, value] layout they shared with RESULT; the value is fixed.
std::vector<uint8_t> encode_mission(uint8_t id) { return encode_fields(Msg::Mission, id, (uint8_t)0); }
std::vector<uint8_t> encode_life_lost(uint8_t epoch) { return encode_fields(Msg::Life, epoch, (uint8_t)1); }
std::vector<uint8_t> encode_out(uint8_t epoch) { return encode_fields(Msg::Out, epoch, (uint8_t)1); }

bool decode_mission(const uint8_t* data, size_t n, uint8_t* id)
{

    uint8_t v = 0;

    return decode_fields(data, n, Msg::Mission, id, &v) && v == 0;
}

bool decode_life_lost(const uint8_t* data, size_t n, uint8_t* epoch)
{

    uint8_t v = 0;

    return decode_fields(data, n, Msg::Life, epoch, &v) && v <= 1;
}

bool decode_out(const uint8_t* data, size_t n, uint8_t* epoch)
{

    uint8_t v = 0;

    return decode_fields(data, n, Msg::Out, epoch, &v) && v <= 1;
}

std::vector<uint8_t> encode_pick(uint8_t level) { return encode_fields(Msg::Pick, (uint8_t)0, level); }

bool decode_pick(const uint8_t* data, size_t n, uint8_t* level)
{

    uint8_t e = 0;
    uint8_t l = 0;

    if (!decode_fields(data, n, Msg::Pick, &e, &l) || l > 0x12)
    {
        return false;
    }

    *level = l;
    return true;
}

std::vector<uint8_t> encode_browse(uint8_t level, uint8_t state) { return encode_fields(Msg::Browse, level, state); }

bool decode_browse(const uint8_t* data, size_t n, uint8_t* level, uint8_t* state)
{

    uint8_t l = 0;
    uint8_t st = 0;

    if (!decode_fields(data, n, Msg::Browse, &l, &st) || l > 0x12 || st > 4)
    {
        return false;
    }

    *level = l;
    *state = st;
    return true;
}

std::vector<uint8_t> encode_ready(uint8_t craft) { return encode_fields(Msg::Ready, (uint8_t)0, craft); }

bool decode_ready(const uint8_t* data, size_t n, uint8_t* craft)
{

    uint8_t e = 0;
    uint8_t c = 0;

    if (!decode_fields(data, n, Msg::Ready, &e, &c) || (c > 8 && c != 0xFF))
    {
        return false;
    }

    *craft = c;
    return true;
}

std::vector<uint8_t> encode_skip(uint16_t scene) { return encode_fields(Msg::Skip, (uint8_t)(scene >> 8), (uint8_t)scene); }

bool decode_skip(const uint8_t* data, size_t n, uint16_t* scene)
{

    uint8_t phase = 0;
    uint8_t count = 0;

    if (!decode_fields(data, n, Msg::Skip, &phase, &count))
    {
        return false;
    }

    *scene = (uint16_t)((phase << 8) | count);
    return true;
}

std::vector<uint8_t> encode_briefing_done() { return encode_fields(Msg::BriefingDone); }
bool decode_briefing_done(const uint8_t* data, size_t n) { return decode_fields(data, n, Msg::BriefingDone); }
std::vector<uint8_t> encode_auth() { return encode_fields(Msg::Auth); }
bool decode_auth(const uint8_t* data, size_t n) { return decode_fields(data, n, Msg::Auth); }
std::vector<uint8_t> encode_accept() { return encode_fields(Msg::Accept); }
bool decode_accept(const uint8_t* data, size_t n) { return decode_fields(data, n, Msg::Accept); }
std::vector<uint8_t> encode_continue() { return encode_fields(Msg::Continue); }
bool decode_continue(const uint8_t* data, size_t n) { return decode_fields(data, n, Msg::Continue); }

std::vector<uint8_t> encode_upgrades(uint8_t epoch, uint32_t bits) { return encode_fields(Msg::Upgrades, epoch, bits); }

bool decode_upgrades(const uint8_t* data, size_t n, uint8_t* epoch, uint32_t* bits)
{

    uint8_t e = 0;
    uint32_t b = 0;

    if (!decode_fields(data, n, Msg::Upgrades, &e, &b) || b == 0 || (b & ~kUpgradeMask) != 0)
    {
        return false;
    }

    *epoch = e;
    *bits = b;
    return true;
}

std::vector<uint8_t> encode_pickup(uint8_t epoch, uint16_t item) { return encode_fields(Msg::Pickup, epoch, item); }

bool decode_pickup(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item)
{
    return decode_fields(data, n, Msg::Pickup, epoch, item);
}

std::vector<uint8_t> encode_tow_trip(uint8_t epoch, uint16_t item) { return encode_fields(Msg::TowTrip, epoch, item); }

bool decode_tow_trip(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* item)
{
    return decode_fields(data, n, Msg::TowTrip, epoch, item);
}

std::vector<uint8_t> encode_hit(uint8_t epoch, uint16_t damage) { return encode_fields(Msg::Hit, epoch, damage); }

bool decode_hit(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* damage)
{
    return decode_fields(data, n, Msg::Hit, epoch, damage);
}

std::vector<uint8_t> encode_rules(bool friendly_fire) { return encode_fields(Msg::Rules, (uint8_t)(friendly_fire ? 1 : 0)); }

bool decode_rules(const uint8_t* data, size_t n, bool* friendly_fire)
{

    uint8_t ff = 0;

    if (!decode_fields(data, n, Msg::Rules, &ff))
    {
        return false;
    }

    *friendly_fire = ff != 0;
    return true;
}

std::vector<uint8_t> encode_trigger(uint8_t epoch, uint16_t event, bool enter)
{
    return encode_fields(Msg::Trigger, epoch, event, (uint8_t)(enter ? 1 : 0));
}

bool decode_trigger(const uint8_t* data, size_t n, uint8_t* epoch, uint16_t* event, bool* enter)
{

    uint8_t e = 0;
    uint16_t ev = 0;
    uint8_t in = 0;

    if (!decode_fields(data, n, Msg::Trigger, &e, &ev, &in) || ev > 0xFFFu || in > 1)
    {
        return false;
    }

    *epoch = e;
    *event = ev;
    *enter = in == 1;
    return true;
}

}
