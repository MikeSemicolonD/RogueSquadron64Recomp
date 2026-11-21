#pragma once
// Mid-mission camera shots linked from level triggers: mods ship camera_shots.txt, one "<level> <trigger> <camera> [personal]",
// "<level> <trigger> destroy <object>" or "<level> <camera> replace <camera>|none" per line.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rs64shots {

// personal: in co-op only the player who set the trigger off sees the shot; otherwise (shared) both do.
// camera is empty for a trigger that only destroys objects.
struct Link {
    std::string level, trigger, camera;
    bool personal = false;
    std::vector<std::string> destroy;
};

// Mission frames between a trigger's destroys and its camera, so the explosion is under way when the shot cuts in.
constexpr int kShotDelayFrames = 15;

// Blank lines, '#' comments, CR and lines with fewer than three fields are skipped; a later camera line for the same level and
// trigger wins, destroy lines add up. Returns the camera links it replaced.
inline std::vector<Link> addLinks(std::vector<Link>& links, std::string_view text) {
    std::vector<Link> replaced_links;
    std::istringstream in{ std::string(text) };
    for (std::string line; std::getline(in, line);) {
        std::istringstream f(line);
        Link l;
        std::string mode;
        if (!(f >> l.level) || l.level[0] == '#' || !(f >> l.trigger >> l.camera)) {
            continue;
        }
        const bool destroy = l.camera == "destroy";
        if (l.camera == "replace") {
            continue;
        }
        if (destroy && !(f >> mode)) {
            continue;
        }
        if (!destroy) {
            l.personal = (f >> mode) && mode == "personal";
        }
        Link* o = nullptr;
        for (auto& x : links) {
            if (x.level == l.level && x.trigger == l.trigger) {
                o = &x;
            }
        }
        if (!o) {
            o = &links.emplace_back(Link{ l.level, l.trigger });
        }
        if (destroy) {
            if (std::find(o->destroy.begin(), o->destroy.end(), mode) == o->destroy.end()) {
                o->destroy.push_back(mode);
            }
            continue;
        }
        if (!o->camera.empty()) {
            replaced_links.push_back(*o);
        }
        o->camera = l.camera;
        o->personal = l.personal;
    }
    return replaced_links;
}

// "<level> <camera> replace <camera>|none": when the mission script starts that camera, the other plays instead (none: nothing).
struct Swap {
    std::string level, from, to;
};

// The later line for the same level and camera wins.
inline void addSwaps(std::vector<Swap>& swaps, std::string_view text) {
    std::istringstream in{ std::string(text) };
    for (std::string line; std::getline(in, line);) {
        std::istringstream f(line);
        Swap s;
        std::string mode;
        if (!(f >> s.level) || s.level[0] == '#' || !(f >> s.from >> mode >> s.to) || mode != "replace") {
            continue;
        }
        const auto old = std::find_if(swaps.begin(), swaps.end(), [&](const Swap& x) { return x.level == s.level && x.from == s.from; });
        if (old != swaps.end()) {
            old->to = s.to;
        } else {
            swaps.push_back(s);
        }
    }
}

// What plays instead of camera on level: nullopt to keep it, "" for nothing.
inline std::optional<std::string> swapFor(const std::vector<Swap>& swaps, const std::string& level, const std::string& camera) {
    for (const auto& s : swaps) {
        if (s.level == level && s.from == camera) {
            return s.to == "none" ? std::string() : s.to;
        }
    }
    return std::nullopt;
}

// How a destroy line treats its object: a spawned unit (slot != 0xFFFF) takes the lethal HIT only through a slot whose context points
// back at it (a stale slot may belong to another NPC); an unloaded one has its stored health cut; dead or tripped walkers are skipped.
enum class Destroy { Hit, CutHealth, Skip };
inline Destroy destroyKind(int32_t health, uint32_t slot, bool slot_consistent, bool tripped) {
    if (health <= 0 || tripped) {
        return Destroy::Skip;
    }
    if (slot == 0xFFFFu) {
        return Destroy::CutHealth;
    }
    return slot_consistent ? Destroy::Hit : Destroy::Skip;
}

// A shot waits while one plays (0x8010C9E0 & 0x40), a mission transition runs (& 0x80) or the game is paused (0x8010CA1C & 1).
inline bool busy(uint32_t hud_flags, uint32_t pause_word) {
    return (hud_flags & 0xC0u) != 0 || (pause_word & 1u) != 0;
}
// Only camera items (DAT type 0, subtype 0x26 in the high half of their first word) play; any other name would be read as one.
inline bool isCameraItem(uint32_t first_word) {
    return (first_word >> 16) == 0x26u;
}

// for_peer: this game applied the trigger on the other player's behalf (co-op host applying the client's trigger).
struct Shot {
    std::string camera;
    bool personal = false;
    bool for_peer = false;
    std::string trigger;
    std::vector<std::string> destroy;
};
inline bool playsHere(const Shot& s) {
    return !(s.personal && s.for_peer);
}
inline bool tellsPeer(const Shot& s) {
    return !s.personal || s.for_peer;
}

// One level load: each linked trigger queues its camera the first time it fires; take() hands the oldest out while nothing else plays.
class Queue {
public:
    void reset(std::string level) {
        level_ = std::move(level);
        fired_.clear();
        pending_.clear();
        wait_ = 0;
    }
    // True when this firing queued a camera.
    bool fired(const std::vector<Link>& links, std::string_view trigger, bool for_peer) {
        for (const auto& l : links) {
            if (l.level == level_ && l.trigger == trigger && fired_.insert(l.trigger).second) {
                pending_.push_back({ l.camera, l.personal, for_peer, l.trigger, l.destroy });
                return true;
            }
        }
        return false;
    }
    // A shot the other player's game sent for its trigger (co-op): false when that trigger already fired here; otherwise it counts as
    // fired, so a trigger both worlds run (an NPC on a path) plays once.
    bool remote(const std::vector<Link>& links, std::string_view trigger) {
        for (const auto& l : links) {
            if (l.level == level_ && l.trigger == trigger) {
                return fired_.insert(l.trigger).second;
            }
        }
        return true;
    }
    // A shot with destroys comes out twice: first with only its destroys, then with only its camera kShotDelayFrames calls later.
    std::optional<Shot> take(bool busy) {
        if (busy || pending_.empty()) {
            return std::nullopt;
        }
        Shot& front = pending_.front();
        if (!front.destroy.empty()) {
            Shot d{ {}, front.personal, front.for_peer, front.trigger, std::move(front.destroy) };
            front.destroy.clear();
            if (front.camera.empty()) {
                pending_.pop_front();
            } else {
                wait_ = kShotDelayFrames;
            }
            return d;
        }
        if (wait_ > 0) {
            --wait_;
            return std::nullopt;
        }
        Shot s = std::move(pending_.front());
        pending_.pop_front();
        return s;
    }
    const std::string& level() const { return level_; }

private:
    std::string level_;
    std::set<std::string> fired_;
    std::deque<Shot> pending_;
    int wait_ = 0;
};

}
