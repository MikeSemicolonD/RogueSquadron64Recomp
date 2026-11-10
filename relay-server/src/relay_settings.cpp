// The relay's settings: defaults, the config file (rs64_relay.cfg), command-line flags, limits and the usage text.
#include "relay_core.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace rs64::relay {

namespace {

std::string trim(const std::string& s)
{

    const size_t a = s.find_first_not_of(" \t\r\n");

    if (a == std::string::npos)
    {
        return "";
    }

    const size_t b = s.find_last_not_of(" \t\r\n");

    return s.substr(a, b - a + 1);
}

std::string flag_name(std::string key)
{

    std::replace(key.begin(), key.end(), '_', '-');
    return "--" + key;
}
}

bool parse_int(const std::string& s, int* out)
{

    const std::string t = trim(s);
    int v = 0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), v);

    if (t.empty() || ec != std::errc() || end != t.data() + t.size())
    {
        return false;
    }

    *out = v;
    return true;
}

std::vector<std::string> words(const std::string& line)
{

    std::istringstream in(line);
    std::vector<std::string> out;
    std::string w;

    while (in >> w) out.push_back(w);

    return out;
}


const std::vector<LimitDef>& limits()
{

    static const std::vector<LimitDef> s_limits = {
        {"max_clients", &Settings::max_clients, 1, kPeerCeiling, "Connections at once (hosts, joiners and browsers)"},
        {"max_games", &Settings::max_games, 1, 2000, "Registered games"},
        {"max_per_ip", &Settings::max_per_ip, 1, kPeerCeiling, "Connections from one address"},
        {"games_per_ip", &Settings::games_per_ip, 1, 2000, "Games hosted from one address"},
        {"rate", &Settings::rate, 10, 10000, "Packets per second per client before it is dropped"},
        {"idle_timeout", &Settings::idle_timeout, 5, 3600, "Seconds before a connection outside a game is closed"},
    };

    return s_limits;
}

bool set_value(Settings* s, const std::string& key, const std::string& value, std::string* error)
{

    int v = 0;

    if (key == "port") {
        if (!parse_int(value, &v) || v < 1 || v > 65535) {
            *error = "port must be 1-65535";
            return false;
        }

        s->port = (uint16_t)v;
        return true;
    }

    if (key == "name") {
        const std::string name = sanitize_name(value, "");

        if (name.empty()) {
            *error = "name needs letters or digits";
            return false;
        }

        s->name = name;
        return true;
    }

    for (const LimitDef& d : limits()) {
        if (key != d.key) continue;

        if (!parse_int(value, &v) || v < d.lo || v > d.hi) {
            *error = std::string(d.key) + " must be " + std::to_string(d.lo) + "-" + std::to_string(d.hi);
            return false;
        }

        s->*d.field = v;
        return true;
    }

    *error = "unknown setting '" + key + "'";
    return false;
}

std::vector<std::string> parse_config(const std::string& text, Settings* s)
{

    std::vector<std::string> warnings;
    std::istringstream in(text);
    std::string line;
    int number = 0;

    while (std::getline(in, line)) {
        ++number;

        const std::string body = trim(line.substr(0, line.find('#')));

        if (body.empty()) continue;

        const size_t eq = body.find('=');
        std::string error = "expected 'key = value'";

        if (eq == std::string::npos || !set_value(s, trim(body.substr(0, eq)), trim(body.substr(eq + 1)), &error)) {
            warnings.push_back("line " + std::to_string(number) + ": " + error + ", skipped");
        }
    }

    return warnings;
}

std::string config_text(const Settings& s)
{

    std::string out = "# RS64 relay server settings ('save' in the server console rewrites this file)\n";

    out += "# UDP port, 1-65535\nport = " + std::to_string(s.port) + "\n";
    out += "# Server name shown in the game's list (A-Z, 0-9, space)\nname = " + s.name + "\n";

    for (const LimitDef& d : limits()) {
        out += "# " + std::string(d.what) + ", " + std::to_string(d.lo) + "-" + std::to_string(d.hi) + "\n";
        out += std::string(d.key) + " = " + std::to_string(s.*d.field) + "\n";
    }

    return out;
}

bool load_config(const std::string& path, Settings* s, std::vector<std::string>* warnings)
{

    std::ifstream in(path, std::ios::binary);

    if (!in)
    {
        return false;
    }

    std::stringstream text;

    text << in.rdbuf();

    for (const std::string& w : parse_config(text.str(), s)) warnings->push_back(path + " " + w);

    return true;
}

bool save_config(const std::string& path, const Settings& s)
{

    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    out << config_text(s);
    return out.good();
}

Options parse_args(int argc, const char* const* argv)
{

    Options o;
    std::vector<std::pair<std::string, std::string>> flags;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        if (a == "--help" || a == "-h") {
            o.help = true;
            continue;
        }

        if (a.rfind("--", 0) != 0) {
            o.errors.push_back("unexpected argument '" + a + "'");
            continue;
        }

        std::string key = a.substr(2);
        std::string value;
        const size_t eq = key.find('=');

        if (eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else if (i + 1 < argc) {
            value = argv[++i];
        } else {
            o.errors.push_back("missing value for " + a);
            continue;
        }

        std::replace(key.begin(), key.end(), '-', '_');

        if (key == "config") {
            o.config = value;
        } else {
            flags.push_back({key, value});
        }
    }

    o.config_found = load_config(o.config, &o.settings, &o.warnings);

    for (const auto& [key, value] : flags) {
        std::string error;

        if (!set_value(&o.settings, key, value, &error)) o.errors.push_back(flag_name(key) + ": " + error);
    }

    return o;
}

std::string usage()
{

    const Settings d;
    std::string out = "Usage: rs64_relay [options]\n\n";

    out += "  --config <path>       Config file (default " + std::string(kDefaultConfig) + "); flags override it\n";
    out += "  --port <n>            UDP port (default " + std::to_string(d.port) + ")\n";
    out += "  --name <text>         Server name shown in the game's list (default " + d.name + ")\n";

    for (const LimitDef& l : limits()) {
        std::string flag = flag_name(l.key) + " <n>";

        flag.resize(22, ' ');
        out += "  " + flag + l.what + " (default " + std::to_string(d.*l.field) + ", " + std::to_string(l.lo) + "-" + std::to_string(l.hi) + ")\n";
    }

    out += "  --help                This text\n\n";
    out += "Console commands: help, games, kick <game id>, limits, set <limit> <value>, save, reload, quit\n";
    return out;
}

}
