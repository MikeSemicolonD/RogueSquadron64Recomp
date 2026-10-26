#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The relay server's own protocol (relay-server/, and later the client transport in multiplayer-native). Game messages ride inside Data packets and are never parsed.
namespace rs64::relay {

constexpr uint8_t kVersion = 1;
constexpr uint16_t kDefaultPort = 27070;
// The game's largest message (rs64::net::kMaxPacket) plus the Data header.
constexpr size_t kMaxGamePacket = 1200;
constexpr size_t kHeader = 2;
constexpr size_t kMaxPacket = kMaxGamePacket + kHeader;
// Server and game names: menu-font text (A-Z, 0-9, space).
constexpr size_t kNameMax = 16;
constexpr size_t kListMax = 50;
// ENet channels: reliable traffic on 0, unreliable on 1.
constexpr uint8_t kChannelReliable = 0;
constexpr uint8_t kChannelUnreliable = 1;
constexpr uint8_t kChannels = 2;

enum class Type : uint8_t {
    Register = 1,
    Update = 2,
    List = 3,
    Join = 4,
    Data = 5,
    Registered = 16,
    ListReply = 17,
    Joined = 18,
    PeerJoined = 19,
    PeerLeft = 20,
    Refused = 21,
};

// Why a request was refused (Refused) or a connection closed (the ENet disconnect data).
enum class Reason : uint8_t {
    None = 0,
    ServerFull = 1,
    TooManyFromAddress = 2,
    GameFull = 3,
    GameGone = 4,
    BuildMismatch = 5,
    BadRequest = 6,
    RateLimited = 7,
    Kicked = 8,
    Idle = 9,
    ServerQuit = 10,
    Version = 11,
    GamesFull = 12,
    AlreadyInGame = 13,
};

const char* reason_text(Reason r);

// Uppercase A-Z, 0-9 and single spaces, at most kNameMax; empty input gives fallback.
std::string sanitize_name(const std::string& s, const std::string& fallback);

struct GameEntry {
    uint32_t id = 0;
    bool locked = false;
    std::string name;
};

struct ListReply {
    std::string server;
    // Waiting games of the asked build, including any past the kListMax listed.
    uint16_t total = 0;
    std::vector<GameEntry> games;
};

std::vector<uint8_t> encode_register(uint32_t build, bool locked, const std::string& name);
bool decode_register(const uint8_t* data, size_t n, uint32_t* build, bool* locked, std::string* name);
std::vector<uint8_t> encode_update(bool locked, const std::string& name);
bool decode_update(const uint8_t* data, size_t n, bool* locked, std::string* name);
std::vector<uint8_t> encode_list(uint32_t build);
bool decode_list(const uint8_t* data, size_t n, uint32_t* build);
std::vector<uint8_t> encode_join(uint32_t id, uint32_t build);
bool decode_join(const uint8_t* data, size_t n, uint32_t* id, uint32_t* build);
// A game message for the paired player; the server forwards the packet unchanged.
std::vector<uint8_t> encode_data(const uint8_t* bytes, size_t n);
bool decode_data(const uint8_t* data, size_t n, const uint8_t** bytes, size_t* len);

std::vector<uint8_t> encode_registered(uint32_t id);
bool decode_registered(const uint8_t* data, size_t n, uint32_t* id);
std::vector<uint8_t> encode_list_reply(const ListReply& r);
bool decode_list_reply(const uint8_t* data, size_t n, ListReply* out);
std::vector<uint8_t> encode_joined(uint32_t id);
bool decode_joined(const uint8_t* data, size_t n, uint32_t* id);
std::vector<uint8_t> encode_peer_joined();
std::vector<uint8_t> encode_peer_left();
std::vector<uint8_t> encode_refused(Type request, Reason reason);
bool decode_refused(const uint8_t* data, size_t n, Type* request, Reason* reason);

// The Type of a packet with our version byte, or 0; *version_ok is false when only the version differs.
uint8_t message_type(const uint8_t* data, size_t n, bool* version_ok = nullptr);

}
