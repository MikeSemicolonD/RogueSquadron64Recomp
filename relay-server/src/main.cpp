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
    print_log("type help for commands");

    while (!relay.quitting()) {
        host.service(relay, 10);
        relay.tick(now_ms());

        std::deque<std::string> lines = take_lines();

        if (g_stop.exchange(false)) lines.push_back("quit");

        for (const std::string& l : lines) print_lines(relay.command(l));

        host.apply(relay);

        for (const std::string& l : relay.take_log()) print_log(l);
    }

    // Lets the disconnects reach the players before the socket closes.
    host.close();

    for (const std::string& l : relay.take_log()) print_log(l);

    print_log("stopped");
    return 0;
}
