// rs64_relay: pairs RS64 co-op players and forwards their packets. Run from a terminal; see docs/guides/relay-server.md.
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include "relay_core.h"
#include "relay_host.h"

#ifdef RS64_RELAY_WEBTRANSPORT
#include "relay_webtransport.h"
#endif

using namespace rs64::relay;

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_lines_mu;
std::deque<std::string> g_lines;

void print_log(const std::string& line)
{

    char stamp[16] = "";
    const std::time_t t = std::time(nullptr);

    std::strftime(stamp, sizeof stamp, "%H:%M:%S", std::localtime(&t));
    std::printf("[%s] %s\n", stamp, line.c_str());
    std::fflush(stdout);
}

void print_lines(const std::vector<std::string>& lines)
{

    for (const std::string& l : lines) std::printf("%s\n", l.c_str());

    std::fflush(stdout);
}

void on_signal(int)
{
    g_stop = true;
}

// Commands come from stdin on their own thread; with no terminal (stdin closed) it ends at once and the server runs on.
void read_console()
{

    std::string line;

    while (std::getline(std::cin, line)) {
        std::lock_guard<std::mutex> lock(g_lines_mu);

        g_lines.push_back(line);
    }
}

std::deque<std::string> take_lines()
{

    std::deque<std::string> out;
    std::lock_guard<std::mutex> lock(g_lines_mu);

    out.swap(g_lines);
    return out;
}

}

int main(int argc, char** argv)
{

    const Options o = parse_args(argc, argv);

    if (o.help) {
        std::printf("%s", usage().c_str());
        return 0;
    }

    for (const std::string& w : o.warnings) print_log(w);

    if (!o.errors.empty()) {
        for (const std::string& e : o.errors) std::fprintf(stderr, "%s\n", e.c_str());

        std::fprintf(stderr, "Run with --help for the options.\n");
        return 2;
    }

    Host host;

    if (!host.open(o.settings.port)) {
        std::fprintf(stderr, "cannot listen on UDP port %u (in use?)\n", o.settings.port);
        return 1;
    }

    Relay relay(o.settings, o.config);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::thread(read_console).detach();

    print_log("'" + o.settings.name + "' listening on UDP port " + std::to_string(o.settings.port) + (o.config_found ? ", settings from " : ", no config file at ") + o.config);

#ifdef RS64_RELAY_WEBTRANSPORT

    // Browser players reach the ENet port through the bridge; its sockets are serviced on their own thread.
    Bridge bridge(o.settings.port);
    BrowserListener browsers(bridge);
    std::atomic<bool> bridge_run{true};
    std::thread bridge_thread([&] {
        while (bridge_run) bridge.pump(5);
    });
    const auto browser_line = [&] {
        print_log("browser players: join with <this PC's public IP>:" + std::to_string(o.settings.browser_port) + "#" + browsers.hash() + " (UDP port " + std::to_string(o.settings.browser_port) + " forwarded too)");
    };

    host.set_bridge(&bridge);

    if (o.settings.browser_port) {
        std::string error;

        if (browsers.start(o.settings.browser_port, now_ms(), &error)) {
            browser_line();
        } else {
            print_log("browser players can't join: " + error);
        }
    }

#endif

    print_log("type help for commands");

    while (!relay.quitting()) {
        host.service(relay, 10);
        relay.tick(now_ms());

#ifdef RS64_RELAY_WEBTRANSPORT

        std::string renew_error;

        if (browsers.tick(now_ms(), &renew_error)) {
            print_log("new certificate: browser players use the new address");
            browser_line();
        } else if (!renew_error.empty()) {
            print_log("browser players can't join: " + renew_error);
        }

#endif

        std::deque<std::string> lines = take_lines();

        if (g_stop.exchange(false)) lines.push_back("quit");

        for (const std::string& l : lines) print_lines(relay.command(l));

        host.apply(relay);

        for (const std::string& l : relay.take_log()) print_log(l);
    }

#ifdef RS64_RELAY_WEBTRANSPORT

    browsers.stop();
    bridge_run = false;
    bridge_thread.join();
    host.set_bridge(nullptr);

#endif

    // Lets the disconnects reach the players before the socket closes.
    host.close();

    for (const std::string& l : relay.take_log()) print_log(l);

    print_log("stopped");
    return 0;
}
