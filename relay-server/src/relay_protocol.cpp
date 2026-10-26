#include "relay_protocol.h"
#include "wire.h"

namespace rs64::relay {

namespace {

using rs64::net::Reader;
using rs64::net::Writer;

Writer start(Type t)
{

    Writer w;

    w.u8(kVersion);
    w.u8((uint8_t)t);
    return w;
}

bool header(Reader& r, Type t)
{

    if (!r.p || r.n > kMaxPacket)
    {
        return false;
    }

    const uint8_t version = r.u8();
    const uint8_t type = r.u8();

    return r.ok && version == kVersion && type == (uint8_t)t;
}

void put_name(Writer& w, const std::string& s)
{

    const size_t n = s.size() < kNameMax ? s.size() : kNameMax;

    w.u8((uint8_t)n);

    for (size_t i = 0; i < n; ++i) w.u8((uint8_t)s[i]);
}

bool get_name(Reader& r, std::string* out)
{

    const uint8_t n = r.u8();

    if (!r.ok || n > kNameMax || !r.need(n))
    {
        return false;
    }

    out->assign((const char*)r.p + r.at, n);
    r.at += n;
    return true;
}

std::vector<uint8_t> encode_id(Type t, uint32_t id)
{

    Writer w = start(t);

    w.u32(id);
    return w.b;
}

bool decode_id(const uint8_t* data, size_t n, Type t, uint32_t* id)
{

    Reader r{data, n};

    if (!header(r, t))
    {
        return false;
    }

    const uint32_t v = r.u32();

    if (!r.done())
    {
        return false;
    }

    *id = v;
    return true;
}

}

const char* reason_text(Reason r)
{

    switch (r) {
    case Reason::None: return "none";
    case Reason::ServerFull: return "server full";
    case Reason::TooManyFromAddress: return "too many connections from one address";
    case Reason::GameFull: return "game full";
    case Reason::GameGone: return "game gone";
    case Reason::BuildMismatch: return "build mismatch";
    case Reason::BadRequest: return "bad request";
    case Reason::RateLimited: return "rate limited";
    case Reason::Kicked: return "kicked";
    case Reason::Idle: return "idle";
    case Reason::ServerQuit: return "server quit";
    case Reason::Version: return "relay protocol version mismatch";
    case Reason::GamesFull: return "game limit reached";
    case Reason::AlreadyInGame: return "already in a game";
    }

    return "unknown";
}

std::string sanitize_name(const std::string& s, const std::string& fallback)
{

    std::string out;

    for (char c : s) {
        if (out.size() >= kNameMax) break;

        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if (c == ' ' && !out.empty() && out.back() != ' ') {
            out += c;
        }
    }

    while (!out.empty() && out.back() == ' ') out.pop_back();

    return out.empty() ? fallback : out;
}

std::vector<uint8_t> encode_register(uint32_t build, bool locked, const std::string& name)
{

    Writer w = start(Type::Register);

    w.u32(build);
    w.u8(locked ? 1 : 0);
    put_name(w, name);
    return w.b;
}

bool decode_register(const uint8_t* data, size_t n, uint32_t* build, bool* locked, std::string* name)
{

    Reader r{data, n};

    if (!header(r, Type::Register))
    {
        return false;
    }

    const uint32_t b = r.u32();
    const uint8_t l = r.u8();
    std::string s;

    if (!get_name(r, &s) || !r.done() || l > 1)
    {
        return false;
    }

    *build = b;
    *locked = l != 0;
    *name = s;
    return true;
}

std::vector<uint8_t> encode_update(bool locked, const std::string& name)
{

    Writer w = start(Type::Update);

    w.u8(locked ? 1 : 0);
    put_name(w, name);
    return w.b;
}

bool decode_update(const uint8_t* data, size_t n, bool* locked, std::string* name)
{

    Reader r{data, n};

    if (!header(r, Type::Update))
    {
        return false;
    }

    const uint8_t l = r.u8();
    std::string s;

    if (!get_name(r, &s) || !r.done() || l > 1)
    {
        return false;
    }

    *locked = l != 0;
    *name = s;
    return true;
}

std::vector<uint8_t> encode_list(uint32_t build)
{
    return encode_id(Type::List, build);
}

bool decode_list(const uint8_t* data, size_t n, uint32_t* build)
{
    return decode_id(data, n, Type::List, build);
}

std::vector<uint8_t> encode_join(uint32_t id, uint32_t build)
{

    Writer w = start(Type::Join);

    w.u32(id);
    w.u32(build);
    return w.b;
}

bool decode_join(const uint8_t* data, size_t n, uint32_t* id, uint32_t* build)
{

    Reader r{data, n};

    if (!header(r, Type::Join))
    {
        return false;
    }

    const uint32_t i = r.u32();
    const uint32_t b = r.u32();

    if (!r.done())
    {
        return false;
    }

    *id = i;
    *build = b;
    return true;
}

std::vector<uint8_t> encode_data(const uint8_t* bytes, size_t n)
{

    Writer w = start(Type::Data);

    w.b.insert(w.b.end(), bytes, bytes + n);
    return w.b;
}

bool decode_data(const uint8_t* data, size_t n, const uint8_t** bytes, size_t* len)
{

    Reader r{data, n};

    if (!header(r, Type::Data) || n == kHeader)
    {
        return false;
    }

    *bytes = data + kHeader;
    *len = n - kHeader;
    return true;
}

std::vector<uint8_t> encode_registered(uint32_t id)
{
    return encode_id(Type::Registered, id);
}

bool decode_registered(const uint8_t* data, size_t n, uint32_t* id)
{
    return decode_id(data, n, Type::Registered, id);
}

std::vector<uint8_t> encode_list_reply(const ListReply& r)
{

    Writer w = start(Type::ListReply);
    const size_t count = r.games.size() < kListMax ? r.games.size() : kListMax;

    put_name(w, r.server);
    w.u16(r.total);
    w.u8((uint8_t)count);

    for (size_t i = 0; i < count; ++i) {
        w.u32(r.games[i].id);
        w.u8(r.games[i].locked ? 1 : 0);
        put_name(w, r.games[i].name);
    }

    return w.b;
}

bool decode_list_reply(const uint8_t* data, size_t n, ListReply* out)
{

    Reader r{data, n};
    ListReply lr;

    if (!header(r, Type::ListReply) || !get_name(r, &lr.server))
    {
        return false;
    }

    lr.total = r.u16();

    const uint8_t count = r.u8();

    if (!r.ok || count > kListMax)
    {
        return false;
    }

    for (uint8_t i = 0; i < count; ++i) {
        GameEntry g;

        g.id = r.u32();

        const uint8_t l = r.u8();

        if (!get_name(r, &g.name) || l > 1)
        {
            return false;
        }

        g.locked = l != 0;
        lr.games.push_back(g);
    }

    if (!r.done())
    {
        return false;
    }

    *out = lr;
    return true;
}

std::vector<uint8_t> encode_joined(uint32_t id)
{
    return encode_id(Type::Joined, id);
}

bool decode_joined(const uint8_t* data, size_t n, uint32_t* id)
{
    return decode_id(data, n, Type::Joined, id);
}

std::vector<uint8_t> encode_peer_joined()
{
    return start(Type::PeerJoined).b;
}

std::vector<uint8_t> encode_peer_left()
{
    return start(Type::PeerLeft).b;
}

std::vector<uint8_t> encode_refused(Type request, Reason reason)
{

    Writer w = start(Type::Refused);

    w.u8((uint8_t)request);
    w.u8((uint8_t)reason);
    return w.b;
}

bool decode_refused(const uint8_t* data, size_t n, Type* request, Reason* reason)
{

    Reader r{data, n};

    if (!header(r, Type::Refused))
    {
        return false;
    }

    const uint8_t q = r.u8();
    const uint8_t s = r.u8();

    if (!r.done())
    {
        return false;
    }

    *request = (Type)q;
    *reason = (Reason)s;
    return true;
}

uint8_t message_type(const uint8_t* data, size_t n, bool* version_ok)
{

    if (version_ok) *version_ok = true;

    if (!data || n < kHeader || n > kMaxPacket)
    {
        return 0;
    }

    if (data[0] != kVersion) {
        if (version_ok) *version_ok = false;

        return 0;
    }

    return data[1];
}

}
