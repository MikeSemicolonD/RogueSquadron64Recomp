// The lobby: its handshake state (Lobby), the lines the MULTIPLAYER page shows, the host address choice and LAN discovery packets.
#include "net/net_core.h"
#include "net/wire.h"
#include <algorithm>
#include <cstring>

namespace rs64::net {

bool waiting_status(const std::string& status)
{
    return status == "SEARCHING" || status == "CONNECTING" || status == "PLAYER JOINING" || status == "WAITING FOR PLAYER";
}

std::string status_dots(const std::string& status, uint32_t now_ms) {

    const uint32_t n = (now_ms / kDotPeriodMs) % 4;
    return status + std::string(n, '.') + std::string(3 - n, kDotPad);
}

int list_pages(int count) {
    return count > kListRows ? (count + kListRows - 1) / kListRows : 1;
}

std::string list_status(int count, int page)
{

    if (count < 0)
    {
        return "SEARCHING";
    }

    if (count == 0)
    {
        return "NO GAMES FOUND";
    }

    std::string s = std::to_string(count) + (count == 1 ? " GAME FOUND" : " GAMES FOUND");

    if (list_pages(count) > 1)
    {
        s += "  PAGE " + std::to_string(page + 1) + " OF " + std::to_string(list_pages(count));
    }

    return s;
}

bool public_ipv4(uint32_t ip)
{

    const uint32_t a = ip >> 24;
    const uint32_t b = (ip >> 16) & 0xFFu;
    return !(a == 0u || a == 10u || a == 127u || (a == 172u && (b & 0xF0u) == 16u) || (a == 192u && b == 168u) || (a == 100u && (b & 0xC0u) == 64u) || (a == 169u && b == 254u));
}

std::string host_address_label(PortMapState state, const std::string& lan_ip, const std::string& ext_ip, uint16_t port)
{

    const std::string suffix = port ? ":" + std::to_string(port) : "";

    if (state == PortMapState::Open && !ext_ip.empty())
    {
        return "YOUR NET ADDRESS " + ext_ip + suffix;
    }

    return lan_ip.empty() ? "NO NETWORK ADDRESS" : "YOUR LAN ADDRESS " + lan_ip + suffix;
}

HostAddress choose_host_address(std::optional<uint32_t> row, const std::vector<uint32_t>& local, std::optional<uint32_t> fallback)
{

    HostAddress h;

    if (row && *row != 0u && ((*row >> 24) == 127u || std::find(local.begin(), local.end(), *row) != local.end()))
    {
        h.bind = *row;
        h.shown = *row;
        return h;
    }

    h.shown = fallback.value_or(0u);
    return h;
}

std::string ipv4_text(uint32_t ip)
{
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255u) + "." + std::to_string((ip >> 8) & 255u) + "." + std::to_string(ip & 255u);
}

static const uint8_t kDiscoveryMagic[4] = {'R', 'S', '6', '4'};
enum : uint8_t { kDiscoveryQuery = 'Q', kDiscoveryReply = 'R' };

static bool discovery_header(Reader& r, uint8_t kind) {

    for (uint8_t m : kDiscoveryMagic) {
        if (r.u8() != m)
        {
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

bool decode_discovery_reply(const uint8_t* data, size_t n, DiscoveryReply* out)
{

    Reader r{data, n};

    if (!r.p || !discovery_header(r, kDiscoveryReply))
    {
        return false;
    }

    DiscoveryReply d;
    d.port = r.u16();
    d.build = r.u32();

    for (char& c : d.name) {
        c = (char)r.u8();
    }

    if (!r.done())
    {
        return false;
    }

    *out = d;
    return true;
}

bool discovery_allowed(uint32_t from, uint32_t bind)
{
    return !public_ipv4(from) && (!bind || (from >> 8) == (bind >> 8));
}

LinkEdges link_poll_edges(bool up, bool was_up, uint32_t connects, uint32_t seen_connects)
{

    LinkEdges e;
    e.connected = up && connects != seen_connects;
    e.lost = was_up && (!up || e.connected);
    return e;
}

void Lobby::reset(LobbyState s, bool host) {

    pake_.wipe();
    pake_started_ = false;
    confirmed_ = false;
    state_ = s;
    host_ = host;
    peer_ = false;
    hello_ = false;
    auth_ = false;
    pw_fail_ = false;
    started_ms_ = -1;
    connected_ms_ = -1;
    left_ = false;
    left_ms_ = -1;
    fail_.clear();
}

void Lobby::host() {

    reset(LobbyState::Hosting, true);
    sends_.clear();
    has_keys_ = false;
    kick_ = false;
    wrong_ = 0;
    retry_ms_ = 0;
}

void Lobby::refuse() {

    reset(LobbyState::Hosting, true);
    kick_ = true;
}

void Lobby::join() {

    reset(LobbyState::Joining, false);
    sends_.clear();
    has_keys_ = false;
}

void Lobby::cancel() {

    reset(LobbyState::Idle, false);
    sends_.clear();
    has_keys_ = false;
}

void Lobby::on_start_failed() {

    state_ = LobbyState::Failed;
    fail_ = host_ ? "CANNOT HOST" : "NETWORK ERROR";
}

void Lobby::on_connected(uint32_t now_ms) {

    if (state_ != LobbyState::Hosting && state_ != LobbyState::Joining)
    {
        return;
    }

    peer_ = true;
    connected_ms_ = now_ms;
    left_ = false;
    has_keys_ = false;

    Hello h;

    h.build = build_;
    h.role = host_ ? 0 : 1;
    h.locked = host_ && !password_.empty() ? 1 : 0;

    if (!secure_random(nonce_, sizeof(nonce_)) && !password_.empty())
    {
        if (host_)
        {
            refuse();
            return;
        }

        client_fail("NETWORK ERROR", false);
        return;
    }

    memcpy(h.nonce, nonce_, sizeof(nonce_));
    queue(encode_hello(h));
}

void Lobby::client_fail(const char* why, bool password)
{

    state_ = LobbyState::Failed;
    fail_ = why;
    pw_fail_ = password;
    pake_.wipe();
}

std::vector<uint8_t> Lobby::sid(const uint8_t* client, const uint8_t* host) const
{

    std::vector<uint8_t> s(client, client + kPakeNonce);

    s.insert(s.end(), host, host + kPakeNonce);
    return s;
}

// The key exchange's channel identifier: this game and its network build, never sent.
static std::vector<uint8_t> pake_ci(uint32_t build)
{

    std::vector<uint8_t> ci = {'r', 's', '6', '4', '-', 'c', 'o', 'o', 'p'};

    for (int i = 0; i < 4; ++i)
    {
        ci.push_back((uint8_t)(build >> (8 * i)));
    }

    return ci;
}

void Lobby::on_hello(const Hello& h)
{

    if (!peer_ || hello_ || (state_ != LobbyState::Hosting && state_ != LobbyState::Joining))
    {
        return;
    }

    if (h.protocol != kProtocol || h.build != build_) {

        if (host_)
        {
            refuse();
            return;
        }

        state_ = LobbyState::Failed;
        fail_ = "VERSION MISMATCH";
        return;
    }

    hello_ = true;
    memcpy(peer_nonce_, h.nonce, sizeof(peer_nonce_));

    if (host_)
    {
        connect_if_done();
        return;
    }

    // No downgrade: a typed password is never given up for an open game, and a locked game is never joined without the exchange.
    if (h.locked && password_.empty())
    {
        client_fail("PASSWORD NEEDED", true);
        return;
    }

    if (!h.locked && !password_.empty())
    {
        client_fail("GAME HAS NO PASSWORD", false);
        return;
    }

    if (!h.locked)
    {
        queue(encode_auth());
        return;
    }

    if (!pake_.begin(true, password_, pake_ci(build_), sid(nonce_, peer_nonce_)))
    {
        client_fail("NETWORK ERROR", false);
        return;
    }

    pake_started_ = true;
    queue(encode_pake(Msg::Pake1, pake_.point(), nullptr));
}

void Lobby::connect_if_done() {

    if (hello_ && auth_) {
        state_ = LobbyState::Connected;
    }
}

// The wait before a host checks another password: three wrong guesses are free, then 1 s doubling to 30 s.
static int64_t guess_delay_ms(uint32_t wrong)
{

    if (wrong <= 3)
    {
        return 0;
    }

    return wrong >= 9 ? 30000 : 1000LL << (wrong - 4);
}

bool Lobby::on_auth() {

    if (!host_ || !peer_ || state_ != LobbyState::Hosting)
    {
        return true;
    }

    if (!password_.empty())
    {
        queue(encode_bye(ByeReason::WrongPassword, 0));
        refuse();
        return false;
    }

    auth_ = true;
    connect_if_done();
    return true;
}

void Lobby::on_pake1(const uint8_t point[kPakePoint])
{

    if (!host_ || !peer_ || !hello_ || pake_started_ || state_ != LobbyState::Hosting)
    {
        return;
    }

    // PAKE2's tag lets the client test one password, so the guess counts now. Each guess needs its own connection, so its connect time is the guess's time.
    if (password_.empty() || connected_ms_ < retry_ms_)
    {
        queue(encode_bye(ByeReason::WrongPassword, 0));
        refuse();
        return;
    }

    ++wrong_;
    retry_ms_ = connected_ms_ + guess_delay_ms(wrong_);

    if (!pake_.begin(false, password_, pake_ci(build_), sid(peer_nonce_, nonce_)) || !pake_.finish(point))
    {
        refuse();
        return;
    }

    pake_started_ = true;
    queue(encode_pake(Msg::Pake2, pake_.point(), pake_.tag()));
}

void Lobby::on_pake2(const uint8_t point[kPakePoint], const uint8_t tag[kPakeTag])
{

    if (host_ || !peer_ || !pake_started_ || confirmed_ || state_ != LobbyState::Joining)
    {
        return;
    }

    if (!pake_.finish(point) || !pake_.check(tag))
    {
        queue(encode_bye(ByeReason::WrongPassword, 0));
        client_fail("WRONG PASSWORD", true);
        return;
    }

    confirmed_ = true;
    queue(encode_pake(Msg::Pake3, nullptr, pake_.tag()));
    keys_ = pake_.keys();
    has_keys_ = true;
    pake_.wipe();
}

void Lobby::on_pake3(const uint8_t tag[kPakeTag])
{

    if (!host_ || !peer_ || !pake_started_ || confirmed_ || state_ != LobbyState::Hosting)
    {
        return;
    }

    if (!pake_.check(tag))
    {
        refuse();
        return;
    }

    confirmed_ = true;
    wrong_ = 0;
    retry_ms_ = 0;
    keys_ = pake_.keys();
    has_keys_ = true;
    pake_.wipe();
    auth_ = true;
    connect_if_done();
}

std::vector<std::vector<uint8_t>> Lobby::take_sends()
{

    std::vector<std::vector<uint8_t>> out;

    out.swap(sends_);
    return out;
}

bool Lobby::take_keys(SessionKeys* out)
{

    if (!has_keys_)
    {
        return false;
    }

    *out = keys_;
    has_keys_ = false;
    keys_ = SessionKeys{};
    return true;
}

void Lobby::on_accept() {

    // In a password game ACCEPT counts only once the host's tag checked (it then comes sealed).
    if (host_ || !peer_ || !hello_ || state_ != LobbyState::Joining || (pake_started_ && !confirmed_))
    {
        return;
    }

    auth_ = true;
    connect_if_done();
}

void Lobby::on_join_failed(const std::string& why) {

    if (host_ || state_ != LobbyState::Joining || peer_)
    {
        return;
    }

    state_ = LobbyState::Failed;
    fail_ = why.empty() ? "COULD NOT CONNECT TO HOST" : why;
}

void Lobby::fail(const std::string& why) {

    state_ = LobbyState::Failed;
    fail_ = why;
}

void Lobby::on_refused(bool sent) {

    if (host_ || state_ != LobbyState::Joining)
    {
        return;
    }

    state_ = LobbyState::Failed;
    pw_fail_ = true;
    fail_ = sent ? "WRONG PASSWORD" : "PASSWORD NEEDED";
}

void Lobby::on_lost()
{

    if (state_ == LobbyState::Idle || state_ == LobbyState::Failed)
    {
        return;
    }

    if (host_) {
        reset(LobbyState::Hosting, true);
        left_ = true;
        return;
    }

    if (state_ == LobbyState::Connected || (state_ == LobbyState::Joining && peer_))
    {
        state_ = LobbyState::Failed;
        fail_ = "CONNECTION LOST";
    }
}

void Lobby::tick(uint32_t now_ms)
{

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

    if (state_ == LobbyState::Joining && !peer_ && (int64_t)now_ms - started_ms_ > join_timeout_)
    {
        state_ = LobbyState::Failed;
        fail_ = "COULD NOT CONNECT TO HOST";
    }

    if ((state_ == LobbyState::Joining || state_ == LobbyState::Hosting) && peer_ && (int64_t)now_ms - connected_ms_ > kHelloTimeoutMs)
    {
        if (host_)
        {
            refuse();
            return;
        }

        state_ = LobbyState::Failed;
        fail_ = "NO HELLO";
    }
}

std::string Lobby::status() const {

    switch (state_) {
    case LobbyState::Idle:
        return "HOST OR JOIN A GAME";
    case LobbyState::Hosting:
        if (peer_)
        {
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
