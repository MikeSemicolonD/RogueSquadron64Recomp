#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "relay_bridge.h"

namespace rs64::relay {

// A self-signed ECDSA P-256 certificate valid 13 days (WebTransport's hash pinning allows at most 14), as PKCS#12 bytes, and its SHA-256 in hex.
bool make_certificate(std::vector<uint8_t>* pkcs12, std::string* sha256_hex, std::string* error);

// The relay's WebTransport listener for browser players: each session's datagrams go through the Bridge to the relay's ENet port.
class BrowserListener {
public:
    explicit BrowserListener(Bridge& bridge) : bridge_(bridge) {}
    ~BrowserListener() { stop(); }
    // Makes a certificate and starts listening; false with *error set on failure.
    bool start(uint16_t port, int64_t now_ms, std::string* error);
    // Renews the certificate once it is 12 days old and no browser is connected (a session in progress keeps the old one up to 13); true when it did.
    bool tick(int64_t now_ms, std::string* error);
    void stop();
    // The part after '#' in the address browser players paste.
    const std::string& hash() const { return hash_; }

private:
    Bridge& bridge_;
    uint16_t port_ = 0;
    int64_t made_ms_ = 0;
    std::string hash_;
    void* context_ = nullptr;
    void* server_ = nullptr;
};

}
