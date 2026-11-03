#include "relay_protocol.h"
#include "net/wire.h"

namespace rs64::relay {

namespace {

using namespace rs64::net;

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

void put_name(Writer& w, const std::string& s) { put_string<uint8_t>(w, s, kNameMax); }
bool get_name(Reader& r, std::string* out) { return take_string<uint8_t>(r, out, kNameMax); }

template <class... F>
std::vector<uint8_t> encode_fields(Type t, const F&... fields)
{

    Writer w = start(t);

    put_all(w, fields...);
    return w.b;
}

// The outputs change only when the whole packet decoded.
template <class... F>
bool decode_fields(const uint8_t* data, size_t n, Type t, F*... out)
{

    Reader r{data, n};

    return header(r, t) && take_all_committed(r, out...);
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

std::vector<uint8_t> encode_list(uint32_t build) { return encode_fields(Type::List, build); }
bool decode_list(const uint8_t* data, size_t n, uint32_t* build) { return decode_fields(data, n, Type::List, build); }
std::vector<uint8_t> encode_join(uint32_t id, uint32_t build) { return encode_fields(Type::Join, id, build); }
bool decode_join(const uint8_t* data, size_t n, uint32_t* id, uint32_t* build) { return decode_fields(data, n, Type::Join, id, build); }

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

std::vector<uint8_t> encode_registered(uint32_t id) { return encode_fields(Type::Registered, id); }
bool decode_registered(const uint8_t* data, size_t n, uint32_t* id) { return decode_fields(data, n, Type::Registered, id); }

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

std::vector<uint8_t> encode_joined(uint32_t id) { return encode_fields(Type::Joined, id); }
bool decode_joined(const uint8_t* data, size_t n, uint32_t* id) { return decode_fields(data, n, Type::Joined, id); }
std::vector<uint8_t> encode_peer_joined() { return encode_fields(Type::PeerJoined); }
std::vector<uint8_t> encode_peer_left() { return encode_fields(Type::PeerLeft); }
std::vector<uint8_t> encode_refused(Type request, Reason reason) { return encode_fields(Type::Refused, (uint8_t)request, (uint8_t)reason); }

bool decode_refused(const uint8_t* data, size_t n, Type* request, Reason* reason)
{

    uint8_t q = 0;
    uint8_t s = 0;

    if (!decode_fields(data, n, Type::Refused, &q, &s))
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
