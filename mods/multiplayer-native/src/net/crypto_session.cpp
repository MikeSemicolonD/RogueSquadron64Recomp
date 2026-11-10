// CPace and message sealing for password games (net/crypto_session.h), on Monocypher.
#include "net/crypto_session.h"
#include <cstring>
#include <monocypher.h>
#include <monocypher-ed25519.h>

#if defined(_WIN32)

#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")

#elif defined(__APPLE__)

#include <sys/types.h>
#include <sys/random.h>

#else

#include <cerrno>
#include <sys/random.h>

#endif

namespace rs64::net {

bool secure_random(uint8_t* out, size_t n)
{

#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#elif defined(__APPLE__)

    while (n > 0) {
        const size_t chunk = n < 256 ? n : 256;

        if (getentropy(out, chunk) != 0)
        {
            return false;
        }

        out += chunk;
        n -= chunk;
    }

    return true;

#else

    while (n > 0) {
        const ssize_t got = getrandom(out, n, 0);

        if (got < 0 && errno == EINTR)
        {
            continue;
        }

        if (got <= 0)
        {
            return false;
        }

        out += got;
        n -= (size_t)got;
    }

    return true;

#endif
}

namespace {

constexpr char kDsi[] = "CPace255";
constexpr size_t kHashBlock = 128;

void put_len(std::vector<uint8_t>& out, size_t len)
{

    do {
        uint8_t b = (uint8_t)(len & 0x7F);

        len >>= 7;

        if (len) {
            b |= 0x80;
        }

        out.push_back(b);
    } while (len);
}

size_t len_size(size_t len)
{

    std::vector<uint8_t> tmp;

    put_len(tmp, len);
    return tmp.size();
}

void lv(std::vector<uint8_t>& out, const uint8_t* p, size_t n)
{

    put_len(out, n);
    out.insert(out.end(), p, p + n);
}

void lv(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) { lv(out, v.data(), v.size()); }
void lv(std::vector<uint8_t>& out, const std::string& s) { lv(out, (const uint8_t*)s.data(), s.size()); }

// Little-endian 256-bit value as 8 limbs; r = a - b, returns the borrow.
uint32_t sub256(uint32_t r[8], const uint32_t a[8], const uint32_t b[8])
{

    uint64_t borrow = 0;

    for (int i = 0; i < 8; ++i)
    {
        const uint64_t d = (uint64_t)a[i] - b[i] - borrow;

        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }

    return (uint32_t)borrow;
}

void select256(uint32_t r[8], const uint32_t a[8], const uint32_t b[8], uint32_t take_a)
{

    const uint32_t mask = 0u - take_a;

    for (int i = 0; i < 8; ++i)
    {
        r[i] = (a[i] & mask) | (b[i] & ~mask);
    }
}

// Monocypher's Elligator map reads 254 bits, the draft's 255: reduce mod p, then swap r for p - r (the same point) when bit 254 is set. Constant time, since r depends on the password.
void canonical_field(uint8_t bytes[32])
{

    static const uint32_t p[8] = {0xFFFFFFEDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu};
    uint32_t r[8];
    uint32_t t[8];

    for (int i = 0; i < 8; ++i)
    {
        r[i] = (uint32_t)bytes[4 * i] | (uint32_t)bytes[4 * i + 1] << 8 | (uint32_t)bytes[4 * i + 2] << 16 | (uint32_t)bytes[4 * i + 3] << 24;
    }

    const uint32_t below_p = sub256(t, r, p);

    select256(r, r, t, below_p);
    sub256(t, p, r);
    select256(r, t, r, (r[7] >> 30) & 1u);

    for (int i = 0; i < 8; ++i)
    {
        bytes[4 * i] = (uint8_t)r[i];
        bytes[4 * i + 1] = (uint8_t)(r[i] >> 8);
        bytes[4 * i + 2] = (uint8_t)(r[i] >> 16);
        bytes[4 * i + 3] = (uint8_t)(r[i] >> 24);
    }

    crypto_wipe(r, sizeof(r));
    crypto_wipe(t, sizeof(t));
}

void sha512(uint8_t out[64], const std::vector<uint8_t>& in) { crypto_sha512(out, in.data(), in.size()); }

void wipe_vec(std::vector<uint8_t>& v)
{

    if (!v.empty()) {
        crypto_wipe(v.data(), v.size());
    }
}
}

void Cpace::generator(uint8_t g[32], const std::string& prs, const std::vector<uint8_t>& ci, const std::vector<uint8_t>& sid)
{

    const size_t dsi_len = sizeof(kDsi) - 1;
    const size_t used = 1 + len_size(prs.size()) + prs.size() + len_size(dsi_len) + dsi_len;
    const size_t zpad = used < kHashBlock ? kHashBlock - used : 0;
    std::vector<uint8_t> s;

    lv(s, (const uint8_t*)kDsi, dsi_len);
    lv(s, prs);
    lv(s, std::vector<uint8_t>(zpad, 0));
    lv(s, ci);
    lv(s, sid);

    uint8_t h[64];

    sha512(h, s);
    h[31] &= 0x7F;
    canonical_field(h);
    crypto_elligator_map(g, h);
    crypto_wipe(h, sizeof(h));
    wipe_vec(s);
}

bool Cpace::begin(bool initiator, const std::string& prs, const std::vector<uint8_t>& ci, const std::vector<uint8_t>& sid, const uint8_t* scalar, const std::vector<uint8_t>& ad)
{

    wipe();
    initiator_ = initiator;
    sid_ = sid;
    ad_ = ad;

    if (scalar) {
        memcpy(scalar_, scalar, sizeof(scalar_));
    } else if (!secure_random(scalar_, sizeof(scalar_))) {
        return false;
    }

    uint8_t g[32];

    generator(g, prs, ci, sid);
    crypto_x25519(own_, scalar_, g);
    crypto_wipe(g, sizeof(g));
    return true;
}

bool Cpace::finish(const uint8_t peer[kPakePoint], const std::vector<uint8_t>& peer_ad)
{

    static const uint8_t zero[32] = {};
    uint8_t k[32];

    crypto_x25519(k, scalar_, peer);
    crypto_wipe(scalar_, sizeof(scalar_));

    if (crypto_verify32(k, zero) == 0)
    {
        crypto_wipe(k, sizeof(k));
        return false;
    }

    const uint8_t* ya = initiator_ ? own_ : peer;
    const uint8_t* yb = initiator_ ? peer : own_;
    const std::vector<uint8_t>& ada = initiator_ ? ad_ : peer_ad;
    const std::vector<uint8_t>& adb = initiator_ ? peer_ad : ad_;
    std::vector<uint8_t> t;

    lv(t, std::string(kDsi) + "_ISK");
    lv(t, sid_);
    lv(t, k, sizeof(k));
    lv(t, ya, kPakePoint);
    lv(t, ada);
    lv(t, yb, kPakePoint);
    lv(t, adb);
    sha512(isk_, t);
    crypto_wipe(k, sizeof(k));
    wipe_vec(t);

    std::vector<uint8_t> m = {'C', 'P', 'a', 'c', 'e', 'M', 'a', 'c'};

    m.insert(m.end(), sid_.begin(), sid_.end());
    m.insert(m.end(), isk_, isk_ + sizeof(isk_));

    uint8_t mac_key[64];

    sha512(mac_key, m);
    wipe_vec(m);

    std::vector<uint8_t> a;
    std::vector<uint8_t> b;

    lv(a, ya, kPakePoint);
    lv(a, ada);
    lv(b, yb, kPakePoint);
    lv(b, adb);
    crypto_sha512_hmac(initiator_ ? own_tag_ : peer_tag_, mac_key, sizeof(mac_key), a.data(), a.size());
    crypto_sha512_hmac(initiator_ ? peer_tag_ : own_tag_, mac_key, sizeof(mac_key), b.data(), b.size());
    crypto_wipe(mac_key, sizeof(mac_key));
    done_ = true;
    return true;
}

bool Cpace::check(const uint8_t peer_tag[kPakeTag]) const
{
    return done_ && crypto_verify64(peer_tag, peer_tag_) == 0;
}

SessionKeys Cpace::keys() const
{

    static const char kC2h[] = "rs64 c2h";
    static const char kH2c[] = "rs64 h2c";
    SessionKeys k;

    crypto_sha512_hkdf(initiator_ ? k.send : k.recv, 32, isk_, sizeof(isk_), nullptr, 0, (const uint8_t*)kC2h, sizeof(kC2h) - 1);
    crypto_sha512_hkdf(initiator_ ? k.recv : k.send, 32, isk_, sizeof(isk_), nullptr, 0, (const uint8_t*)kH2c, sizeof(kH2c) - 1);
    return k;
}

void Cpace::wipe()
{

    crypto_wipe(scalar_, sizeof(scalar_));
    crypto_wipe(isk_, sizeof(isk_));
    crypto_wipe(own_tag_, sizeof(own_tag_));
    crypto_wipe(peer_tag_, sizeof(peer_tag_));
    done_ = false;
}

namespace {

constexpr size_t kSealHeader = 10;

void seal_nonce(uint8_t nonce[24], const uint8_t* header)
{

    memset(nonce, 0, 24);
    memcpy(nonce, header + 1, 9);
}

uint64_t read_u64(const uint8_t* p)
{

    uint64_t v = 0;

    for (int i = 7; i >= 0; --i)
    {
        v = v << 8 | p[i];
    }

    return v;
}
}

void wipe_keys(SessionKeys* k) { crypto_wipe(k, sizeof(*k)); }

void Cipher::install(const SessionKeys& k)
{

    clear();
    k_ = k;
    active_ = true;
}

void Cipher::clear()
{

    crypto_wipe(&k_, sizeof(k_));
    active_ = false;
    send_ctr_[0] = 0;
    send_ctr_[1] = 0;
    recv_reliable_ = 0;
    seen_any_ = false;
    highest_ = 0;
    window_ = 0;
}

std::vector<uint8_t> Cipher::seal(const std::vector<uint8_t>& plain, bool reliable)
{

    std::vector<uint8_t> out(kSealOverhead + plain.size());
    const uint64_t ctr = send_ctr_[reliable ? 1 : 0]++;

    out[0] = kSealedMark;
    out[1] = reliable ? 1 : 0;

    for (int i = 0; i < 8; ++i)
    {
        out[2 + i] = (uint8_t)(ctr >> (8 * i));
    }

    uint8_t nonce[24];

    seal_nonce(nonce, out.data());
    crypto_aead_lock(out.data() + kSealOverhead, out.data() + kSealHeader, k_.send, nonce, out.data(), kSealHeader, plain.data(), plain.size());
    return out;
}

Cipher::Open Cipher::open(const uint8_t* data, size_t n, std::vector<uint8_t>* out)
{

    if (!active_ || !data || n < kSealOverhead || data[0] != kSealedMark || data[1] > 1)
    {
        return Open::Drop;
    }

    const bool reliable = data[1] == 1;
    const uint64_t ctr = read_u64(data + 2);

    if (reliable && ctr != recv_reliable_)
    {
        return Open::Fail;
    }

    if (!reliable && seen_any_ && ctr <= highest_ && (highest_ - ctr >= 64 || (window_ >> (highest_ - ctr)) & 1u))
    {
        return Open::Drop;
    }

    uint8_t nonce[24];
    std::vector<uint8_t> plain(n - kSealOverhead);

    seal_nonce(nonce, data);

    if (crypto_aead_unlock(plain.data(), data + kSealHeader, k_.recv, nonce, data, kSealHeader, data + kSealOverhead, plain.size()) != 0)
    {
        return reliable ? Open::Fail : Open::Drop;
    }

    if (reliable) {
        ++recv_reliable_;
    } else if (!seen_any_ || ctr > highest_) {
        const uint64_t shift = seen_any_ ? ctr - highest_ : 64;

        window_ = (shift >= 64 ? 0 : window_ << shift) | 1u;
        highest_ = ctr;
        seen_any_ = true;
    } else {
        window_ |= 1ull << (highest_ - ctr);
    }

    *out = std::move(plain);
    return Open::Ok;
}
}
