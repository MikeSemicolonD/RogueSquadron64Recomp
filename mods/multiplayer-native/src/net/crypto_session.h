// Password games: CPace (draft-irtf-cfrg-cpace-21, CPACE-X25519-SHA512, initiator-responder, with key confirmation) turns the password into session keys, and Cipher seals every message with XChaCha20-Poly1305.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rs64::net {

// The operating system's secure random generator; false if it failed (a password game cannot start then).
bool secure_random(uint8_t* out, size_t n);

constexpr size_t kPakeNonce = 16;
constexpr size_t kPakePoint = 32;
constexpr size_t kPakeTag = 64;

struct SessionKeys {
    uint8_t send[32] = {};
    uint8_t recv[32] = {};
};
void wipe_keys(SessionKeys* k);

// One side of a CPace exchange. The client is the initiator (A), the host the responder (B).
class Cpace {
public:
    ~Cpace() { wipe(); }

    // Derives the generator from the password, ci and sid and makes this side's point; scalar null = a random one, ad = this side's associated data (empty in the game). False: no randomness.
    bool begin(bool initiator, const std::string& prs, const std::vector<uint8_t>& ci, const std::vector<uint8_t>& sid, const uint8_t* scalar = nullptr, const std::vector<uint8_t>& ad = {});
    const uint8_t* point() const { return own_; }

    // Takes the peer's point and associated data: false for a low-order point (the draft's abort). Then tag() and check() work.
    bool finish(const uint8_t peer[kPakePoint], const std::vector<uint8_t>& peer_ad = {});
    const uint8_t* tag() const { return own_tag_; }
    bool check(const uint8_t peer_tag[kPakeTag]) const;

    // The two session keys, this side's send key first.
    SessionKeys keys() const;
    const uint8_t* isk() const { return isk_; }
    void wipe();

    // The draft's generator for a password, ci and sid (exposed for its test vectors).
    static void generator(uint8_t g[32], const std::string& prs, const std::vector<uint8_t>& ci, const std::vector<uint8_t>& sid);
private:
    bool initiator_ = false;
    bool done_ = false;
    std::vector<uint8_t> sid_;
    std::vector<uint8_t> ad_;
    uint8_t scalar_[32] = {};
    uint8_t own_[kPakePoint] = {};
    uint8_t isk_[64] = {};
    uint8_t own_tag_[kPakeTag] = {};
    uint8_t peer_tag_[kPakeTag] = {};
};

// The sealed wire format: 0xAE | class (0 unreliable, 1 reliable) | counter u64 LE | tag 16 | ciphertext.
constexpr uint8_t kSealedMark = 0xAE;
constexpr size_t kSealOverhead = 26;

class Cipher {
public:
    ~Cipher() { clear(); }
    void install(const SessionKeys& k);
    void clear();
    bool active() const { return active_; }
    std::vector<uint8_t> seal(const std::vector<uint8_t>& plain, bool reliable);

    // Drop: forged, replayed or plaintext, ignore it. Fail: a reliable message that does not open, the session cannot go on.
    enum class Open { Ok, Drop, Fail };
    Open open(const uint8_t* data, size_t n, std::vector<uint8_t>* out);
private:
    bool active_ = false;
    SessionKeys k_;
    uint64_t send_ctr_[2] = {};
    uint64_t recv_reliable_ = 0;

    // Unreliable replay window: the highest counter seen and a bit per counter below it (bit i = highest - i).
    bool seen_any_ = false;
    uint64_t highest_ = 0;
    uint64_t window_ = 0;
};
}
