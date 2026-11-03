// The MULTIPLAYER page's text rows: the address or host name (AddressEditor), a relay join code or password (CodeEditor), the host's game name (NameEditor).
#include "net/net_core.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace rs64::net {

// "1-65535" only: no sign, no leading junk.
static bool parse_port(const std::string& s, uint16_t* out)
{

    if (s.empty() || s.size() > 5 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
        return false;
    }

    const int v = std::stoi(s);

    if (v < 1 || v > 65535)
    {
        return false;
    }

    *out = (uint16_t)v;
    return true;
}

void AddressEditor::move(int dir) {
    cursor = std::clamp(cursor + dir, 0, 3);
}

void AddressEditor::step(int delta)
{
    octet[cursor] = (uint8_t)std::clamp((int)octet[cursor] + delta, 0, 255);
}

bool AddressEditor::input(uint32_t pressed, uint32_t held)
{

    if (!editing)
    {
        return false;
    }

    touched = true;

    if (pressed & (kPadA | kPadB)) {
        if (typing) {
            end_typing();
        }

        editing = false;
        held_frames = 0;
        return true;
    }

    if (typing)
    {
        return true;
    }

    if (pressed & kPadLeft) {
        move(-1);
    }

    if (pressed & kPadRight) {
        move(+1);
    }

    // A light stick flick sets the pressed bit without reaching the held threshold.
    const uint32_t ud = held | pressed;
    const int dir = (ud & kPadUp) ? +1 : (ud & kPadDown) ? -1 : 0;

    if (dir == 0) {
        held_frames = 0;
    } else if (pressed & (kPadUp | kPadDown)) {
        held_frames = 1;
        step(dir);
    } else if (++held_frames >= 12 && (held_frames - 12) % 3 == 0) {
        step(held_frames >= 45 ? dir * 10 : dir);
    }

    return true;
}

void AddressEditor::load(const std::string& s)
{

    saved.clear();
    touched = false;
    port = 0;

    if (s.empty() || parse(s, this))
    {
        return;
    }

    // A hostname may carry ":port"; an IPv6 literal (several colons) is kept whole.
    const size_t colon = s.rfind(':');

    if (colon != std::string::npos && s.find(':') == colon && colon > 0 && parse_port(s.substr(colon + 1), &port))
    {
        saved = s.substr(0, colon);
        return;
    }

    saved = s;
}

std::string AddressEditor::target() const
{

    if (!touched && !saved.empty())
    {
        return saved;
    }

    return std::to_string(octet[0]) + "." + std::to_string(octet[1]) + "." + std::to_string(octet[2]) + "." + std::to_string(octet[3]);
}

std::string AddressEditor::entry() const {
    return target() + (port ? ":" + std::to_string(port) : "");
}

std::optional<uint32_t> AddressEditor::ipv4() const
{

    if (!touched && !saved.empty())
    {
        return std::nullopt;
    }

    return ((uint32_t)octet[0] << 24) | ((uint32_t)octet[1] << 16) | ((uint32_t)octet[2] << 8) | octet[3];
}

void AddressEditor::begin_typing() {

    typing = true;
    typed = entry();
}

// Digits, letters, '.', '-' and ':': an IPv4 address or a host name (a friend's DDNS name, a relay server), with an optional port.
void AddressEditor::type_text(const char* s) {

    for (; s && *s; ++s)
    {
        const char c = *s;
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == ':';

        if (ok && typed.size() < kMaxTyped) {
            typed += (char)std::tolower((unsigned char)c);
        }
    }
}

void AddressEditor::type_backspace() {

    if (!typed.empty()) {
        typed.pop_back();
    }
}

void AddressEditor::cancel_typing() {

    typing = false;
    editing = false;
    held_frames = 0;
    typed.clear();
}

bool AddressEditor::end_typing() {

    typing = false;
    editing = false;
    held_frames = 0;
    AddressEditor parsed;

    // A name with a letter in it is a host name, kept as typed (load splits off a port).
    if (!typed.empty() && !parse(typed, &parsed) && std::any_of(typed.begin(), typed.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
    {
        load(typed);
        return true;
    }

    if (typed.empty() || !parse(typed, &parsed))
    {
        return false;
    }

    for (int i = 0; i < 4; ++i) {
        octet[i] = parsed.octet[i];
    }

    port = parsed.port;
    saved.clear();
    touched = true;
    return true;
}

// The menu font is capitals only; host names ignore case.
static std::string upper(std::string s) {

    for (char& c : s) c = (char)std::toupper((unsigned char)c);

    return s;
}

std::string AddressEditor::label() const {

    if (typing)
    {
        return upper(typed) + kCaret;
    }

    if (!touched && !saved.empty())
    {
        return upper(entry());
    }

    std::string s;

    for (int i = 0; i < 4; ++i)
    {
        if (i > 0) {
            s += '.';
        }

        const bool mark = editing && i == cursor;
        s += (mark ? "<" : "") + std::to_string(octet[i]) + (mark ? ">" : "");
    }

    return port ? s + ":" + std::to_string(port) : s;
}

bool CodeEditor::input(uint32_t pressed, uint32_t held)
{

    (void)held;

    if (!editing)
    {
        return false;
    }

    if (pressed & (kPadA | kPadB)) {
        if (typing) {
            end_typing();
        }

        editing = false;
        return true;
    }

    if (typing)
    {
        return true;
    }

    if (pressed & kPadLeft) {
        cursor = std::max(0, cursor - 1);
    }

    if (pressed & kPadRight) {
        cursor = std::min(kDigits - 1, cursor + 1);
    }

    const int dir = (pressed & kPadUp) ? 1 : (pressed & kPadDown) ? 9 : 0;
    digit[cursor] = (char)('0' + (digit[cursor] - '0' + dir) % 10);
    return true;
}

void CodeEditor::begin_typing() {

    typing = true;
    typed = code();
}

void CodeEditor::type_text(const char* s) {

    for (; s && *s; ++s) {
        if (*s >= '0' && *s <= '9' && typed.size() < (size_t)kDigits) {
            typed += *s;
        }
    }
}

void CodeEditor::type_backspace() {

    if (!typed.empty()) {
        typed.pop_back();
    }
}

bool CodeEditor::end_typing() {

    typing = false;
    editing = false;
    const bool ok = valid(typed);

    if (ok) {
        load(typed);
    }

    typed.clear();
    return ok;
}

void CodeEditor::cancel_typing() {

    typing = false;
    editing = false;
    typed.clear();
}

void CodeEditor::load(const std::string& s) {

    if (valid(s)) {
        std::copy(s.begin(), s.end(), digit);
    }
}

std::string CodeEditor::code() const {
    return std::string(digit, digit + kDigits);
}

std::string CodeEditor::label() const {

    // Typing: the digits so far, the caret, then a dash per digit still to type.
    if (typing)
    {
        return prefix + typed + kCaret + std::string(kDigits - std::min(typed.size(), (size_t)kDigits), '-');
    }

    std::string s = prefix;

    for (int i = 0; i < kDigits; ++i)
    {
        const bool mark = editing && i == cursor;
        s += mark ? std::string("<") + digit[i] + ">" : std::string(1, digit[i]);
    }

    return s;
}

bool CodeEditor::valid(const std::string& s)
{
    return s.size() == (size_t)kDigits && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

namespace {

constexpr const char* kNameChars = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
}

std::string NameEditor::clean(const std::string& s) {

    std::string out;

    for (char c : s)
    {
        if (out.size() >= (size_t)kMax) {
            break;
        }

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }

        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c == ' ' && !out.empty() && out.back() != ' '))
        {
            out += c;
        }
    }

    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }

    return out;
}

bool NameEditor::input(uint32_t pressed, uint32_t held)
{

    (void)held;

    if (!editing)
    {
        return false;
    }

    if (pressed & (kPadA | kPadB)) {
        if (typing) {
            end_typing();
        }

        editing = false;
        name = clean(name);
        cursor = 0;
        return true;
    }

    if (typing)
    {
        return true;
    }

    const int last = std::min((int)name.size(), kMax - 1);

    if (pressed & kPadLeft) {
        cursor = std::max(0, cursor - 1);
    }

    if (pressed & kPadRight) {
        cursor = std::min(last, cursor + 1);
    }

    const int dir = (pressed & kPadUp) ? 1 : (pressed & kPadDown) ? -1 : 0;

    if (dir) {
        if (cursor >= (int)name.size()) {
            name.resize(cursor + 1, ' ');
        }

        const int n = (int)strlen(kNameChars);
        const char* at = strchr(kNameChars, name[cursor]);
        const int i = at ? (int)(at - kNameChars) : 0;
        name[cursor] = kNameChars[(i + dir + n) % n];
    }

    return true;
}

void NameEditor::begin_typing() {

    typing = true;
    typed = name;
}

void NameEditor::type_text(const char* s) {

    for (; s && *s; ++s)
    {
        const char c = (*s >= 'a' && *s <= 'z') ? (char)(*s - 'a' + 'A') : *s;

        if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') && typed.size() < (size_t)kMax)
        {
            typed += c;
        }
    }
}

void NameEditor::type_backspace() {

    if (!typed.empty()) {
        typed.pop_back();
    }
}

bool NameEditor::end_typing() {

    name = clean(typed);
    typing = false;
    editing = false;
    typed.clear();
    cursor = 0;
    return true;
}

void NameEditor::cancel_typing() {

    typing = false;
    editing = false;
    typed.clear();
}

std::string NameEditor::label(const std::string& fallback) const {

    if (typing)
    {
        return "SERVER NAME " + typed + kCaret;
    }

    if (!editing)
    {
        return "SERVER NAME " + (name.empty() ? fallback : name);
    }

    std::string s = "SERVER NAME ";

    // The cursor past the end is the slot a step appends to.
    for (int i = 0; i < std::max((int)name.size(), cursor + 1); ++i) {
        const char c = i < (int)name.size() ? name[i] : ' ';
        s += i == cursor ? std::string("<") + c + ">" : std::string(1, c);
    }

    return s;
}

std::string AddressEditor::text() const
{

    const std::string a = std::to_string(octet[0]) + "." + std::to_string(octet[1]) + "." + std::to_string(octet[2]) + "." + std::to_string(octet[3]);
    return port ? a + ":" + std::to_string(port) : a;
}

bool AddressEditor::parse(const std::string& in, AddressEditor* out)
{

    const size_t colon = in.find(':');
    uint16_t p = 0;

    if (colon != std::string::npos && !parse_port(in.substr(colon + 1), &p))
    {
        return false;
    }

    const std::string s = in.substr(0, colon);
    uint8_t o[4];
    size_t at = 0;

    for (int i = 0; i < 4; ++i)
    {
        size_t end = at;

        while (end < s.size() && s[end] >= '0' && s[end] <= '9' && end - at < 3)
        {
            ++end;
        }

        if (end == at || (i < 3 ? (end >= s.size() || s[end] != '.') : end != s.size()))
        {
            return false;
        }

        const int v = std::stoi(s.substr(at, end - at));

        if (v > 255)
        {
            return false;
        }

        o[i] = (uint8_t)v;
        at = end + 1;
    }

    memcpy(out->octet, o, 4);
    out->port = p;
    return true;
}

}
