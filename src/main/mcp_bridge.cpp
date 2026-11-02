#include "main/mcp_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "main/mcp_framing.h"
#include "hle/rt64_rs64_mcp.h"
#include "host_api/host_api.h"
#include "lockstep/lockstep_core.h"
#include "main/renderdoc_capture.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
using sock_t = SOCKET;
static constexpr sock_t kNoSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t kNoSock = -1;
#endif

using nlohmann::json;

extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
extern "C" void rs64_mcp_on_disconnect();

static std::mutex s_lock;
static std::map<std::string, HostHandler> s_handlers;
static std::string s_outbox;
static std::atomic<bool> s_connected{ false };
static std::atomic<uint32_t> s_generation{ 0 };

static void close_sock(sock_t s)
{

#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif

}

static int process_id()
{

#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif

}

static void send_all(sock_t s, const std::string& text)
{

    size_t sent = 0;

    while (sent < text.size())
    {
        const int n = send(s, text.data() + sent, (int)(text.size() - sent), 0);

        if (n <= 0)
        {
            return;
        }

        sent += (size_t)n;
    }
}

// Replies from a closed connection are dropped, so a stale handler can't answer the next client.
static void queue_reply(uint32_t generation, const json& reply)
{

    std::lock_guard<std::mutex> g(s_lock);

    if (generation == s_generation.load()) {
        s_outbox += reply.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    }
}

static json error_reply(const json& id, const std::string& text)
{

    return { { "id", id }, { "ok", false }, { "error", text } };
}

static HostHandler find_handler(const std::string& method)
{

    std::lock_guard<std::mutex> g(s_lock);
    auto it = s_handlers.find(method);

    return (it != s_handlers.end()) ? it->second : nullptr;
}

static std::string take_outbox()
{

    std::lock_guard<std::mutex> g(s_lock);
    std::string out;

    out.swap(s_outbox);

    return out;
}

static void end_connection()
{

    std::lock_guard<std::mutex> g(s_lock);

    s_generation++;
    s_outbox.clear();
}

// False for a line that isn't a request object; the caller hangs up, so an HTTP request (a browser page posting to the port) never reaches its body.
static bool dispatch(const std::string& line, uint32_t generation)
{

    const json req = json::parse(line, nullptr, false);

    if (req.is_discarded() || !req.is_object() || !req.contains("method") || !req["method"].is_string())
    {
        return false;
    }

    const json id = req.value("id", json());
    const std::string method = req["method"].get<std::string>();
    const json params = req.contains("params") ? req["params"] : json::object();

    if (method == "ping") {
        queue_reply(generation, { { "id", id }, { "ok", true }, { "result", { { "pong", true }, { "pid", process_id() } } } });

        return true;
    }

    if (RT64::rs64mcp::has(method)) {
        RT64::rs64mcp::post(generation, id, method, params);

        return true;
    }

    const HostHandler handler = find_handler(method);

    if (!handler)
    {
        queue_reply(generation, error_reply(id, "unknown method: " + method));

        return true;
    }

    std::thread([handler, id, params, generation]() {
        try {
            queue_reply(generation, { { "id", id }, { "ok", true }, { "result", handler(params) } });
        }
        catch (const std::exception& e) {
            queue_reply(generation, error_reply(id, e.what()));
        }
    }).detach();

    return true;
}

static void serve(sock_t client, sock_t listener)
{

    rs64::mcp::LineBuffer buf;
    const uint32_t generation = s_generation.load();
    char chunk[4096];
    std::string line;

    for (;;)
    {
        fd_set rd;

        FD_ZERO(&rd);
        FD_SET(client, &rd);
        FD_SET(listener, &rd);

        timeval tv{ 0, 2000 };
        const int maxfd = (int)((client > listener) ? client : listener) + 1;
        const int ready = select(maxfd, &rd, nullptr, nullptr, &tv);

        if (ready < 0)
        {
            break;
        }

        if (FD_ISSET(listener, &rd)) {
            sock_t extra = accept(listener, nullptr, nullptr);

            if (extra != kNoSock) {
                send_all(extra, "{\"id\":0,\"ok\":false,\"error\":\"busy\"}\n");
                close_sock(extra);
            }
        }

        if (FD_ISSET(client, &rd)) {
            const int n = recv(client, chunk, sizeof(chunk), 0);

            if (n <= 0)
            {
                break;
            }

            buf.append(chunk, (size_t)n);

            if (buf.overflowed()) {
                queue_reply(generation, error_reply(nullptr, "line too long"));
            }

            bool valid = true;

            while (valid && buf.pop(line)) {
                valid = dispatch(line, generation);
            }

            if (!valid)
            {
                send_all(client, error_reply(nullptr, "malformed request; closing").dump() + "\n");

                break;
            }
        }

        uint32_t tag = 0;
        json rt64_reply;

        while (RT64::rs64mcp::takeReply(tag, rt64_reply)) {
            queue_reply(tag, rt64_reply);
        }

        const std::string out = take_outbox();

        if (!out.empty()) {
            send_all(client, out);
        }
    }
}

static void bridge_thread(uint16_t port)
{

#ifdef _WIN32
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        fprintf(stderr, "[mcp] WSAStartup failed\n");

        return;
    }

#endif

    sock_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (listener == kNoSock || bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 4) != 0)
    {
        fprintf(stderr, "[mcp] could not listen on 127.0.0.1:%u\n", port);

        return;
    }

    fprintf(stderr, "[mcp] listening on 127.0.0.1:%u\n", port);

    for (;;)
    {
        sock_t client = accept(listener, nullptr, nullptr);

        if (client == kNoSock)
        {
            continue;
        }

        s_connected = true;
        fprintf(stderr, "[mcp] client connected\n");
        serve(client, listener);
        close_sock(client);
        end_connection();
        s_connected = false;
        rs64_mcp_on_disconnect();
        fprintf(stderr, "[mcp] client disconnected\n");
    }
}

// The host byte for a game address (RDRAM is byte-swapped per word), after any moved-range relocation; throws outside the 16 MB the host
// tables use (manifests, terrain grid, fonts), wider than the mods' ram_ok.
static volatile uint8_t* rdram_byte(uint32_t addr)
{

    volatile uint8_t* rdram = g_recomp_rdram_for_wp_raw;
    const uint32_t at = rs64::ls::relocated(addr);

    if (!rdram || at < 0x80000000u || at >= 0x81000000u)
    {
        throw std::runtime_error("address outside valid RAM");
    }

    return &rdram[(at - 0x80000000u) ^ 3];
}

static uint32_t checked_range(const json& p, uint32_t size)
{

    if (!p.contains("addr") || !p["addr"].is_number_unsigned())
    {
        throw std::runtime_error("addr (unsigned) required");
    }

    const uint64_t addr = p["addr"].get<uint64_t>();

    if (size == 0 || size > 65536 || addr < 0x80000000u || addr + size > 0x100000000ull)
    {
        throw std::runtime_error("need 0x80000000 <= addr and 1 <= size <= 65536");
    }

    rdram_byte((uint32_t)addr);
    rdram_byte((uint32_t)(addr + size - 1));

    return (uint32_t)addr;
}

static json rdram_read(const json& p)
{

    static constexpr const char* kHex = "0123456789abcdef";
    const uint32_t size = p.value("size", 4u);
    const uint32_t addr = checked_range(p, size);
    std::string hex;

    for (uint32_t i = 0; i < size; i++)
    {
        const uint8_t b = *rdram_byte(addr + i);

        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }

    return { { "addr", addr }, { "hex", hex } };
}

static json rdram_write(const json& p)
{

    const std::string hex = p.value("hex", std::string());

    if (hex.size() % 2 != 0 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    {
        throw std::runtime_error("hex must be an even-length hex string");
    }

    const uint32_t size = (uint32_t)(hex.size() / 2);
    const uint32_t addr = checked_range(p, size);

    for (uint32_t i = 0; i < size; i++)
    {
        *rdram_byte(addr + i) = (uint8_t)std::stoul(hex.substr(i * 2, 2), nullptr, 16);
    }

    return { { "written", size } };
}

// Big-endian RAM image: the game's 8 MB (the ROGUESQ_DUMP_RDRAM_AT_VI format), or with full the 16 MB including the host tables above it; rdram_golden_diff.py reads both.
static json rdram_dump(const json& p)
{

    const uint32_t kSize = p.value("full", false) ? 0x1000000u : 0x800000u;
    volatile uint8_t* rdram = g_recomp_rdram_for_wp_raw;

    if (!p.contains("path") || !p["path"].is_string())
    {
        throw std::runtime_error("path (string) required");
    }

    if (!rdram)
    {
        throw std::runtime_error("RDRAM not available yet");
    }

    const std::string path = p["path"].get<std::string>();
    std::vector<uint8_t> bytes(kSize);

    for (uint32_t i = 0; i < kSize; i++) {
        bytes[i] = rdram[i ^ 3];
    }

    FILE* f = fopen(path.c_str(), "wb");

    if (!f)
    {
        throw std::runtime_error("could not write " + path);
    }

    const size_t written = fwrite(bytes.data(), 1, bytes.size(), f);

    fclose(f);

    return { { "path", path }, { "bytes", written } };
}

// Addresses of a big-endian byte pattern in the 16 MB, at a multiple of align, up to limit hits.
static json rdram_find(const json& p)
{

    static constexpr uint32_t kSize = 0x1000000;
    volatile uint8_t* rdram = g_recomp_rdram_for_wp_raw;
    const std::string hex = p.value("hex", std::string());
    const uint32_t align = std::max(1u, p.value("align", 1u));
    const uint32_t limit = p.value("limit", 64u);

    if (hex.empty() || hex.size() % 2 != 0 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    {
        throw std::runtime_error("hex must be a non-empty even-length hex string");
    }

    if (!rdram)
    {
        throw std::runtime_error("RDRAM not available yet");
    }

    std::vector<uint8_t> pat(hex.size() / 2);

    for (size_t i = 0; i < pat.size(); i++) pat[i] = (uint8_t)std::stoul(hex.substr(i * 2, 2), nullptr, 16);

    json hits = json::array();
    bool more = false;

    for (uint32_t at = 0; at + pat.size() <= kSize; at += align)
    {
        size_t i = 0;

        while (i < pat.size() && rdram[(at + i) ^ 3] == pat[i]) ++i;

        if (i < pat.size()) continue;

        if (hits.size() >= limit)
        {
            more = true;
            break;
        }

        hits.push_back(0x80000000u + at);
    }

    return { { "hits", hits }, { "more", more } };
}

// Captures the next `frames` presents and returns the saved .rdc once RenderDoc has written it.
static json renderdoc_capture(const json& p)
{

    if (!rs64_renderdoc_active())
    {
        throw std::runtime_error("renderdoc not loaded");
    }

    const std::string before = rs64_renderdoc_last_capture();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)(p.value("timeout_s", 20.0) * 1000.0));

    rs64_renderdoc_trigger(p.value("frames", 1u));

    while (std::chrono::steady_clock::now() < until)
    {
        const std::string now = rs64_renderdoc_last_capture();

        if (!now.empty() && now != before)
        {
            return { { "path", now } };
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    throw std::runtime_error("no capture saved before the timeout");
}

void rs64_mcp_bridge_start(uint16_t port)
{

    rs64_mcp_register("rdram_read", rdram_read);
    rs64_mcp_register("rdram_write", rdram_write);
    rs64_mcp_register("renderdoc_capture", renderdoc_capture);
    rs64_mcp_register("rdram_dump", rdram_dump);
    rs64_mcp_register("rdram_find", rdram_find);

    std::thread(bridge_thread, port).detach();
}

bool rs64_mcp_connected()
{

    return s_connected.load();
}

void rs64_mcp_register(const char* method, HostHandler handler)
{

    std::lock_guard<std::mutex> g(s_lock);
    s_handlers[method] = handler;
}
