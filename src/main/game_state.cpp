#include "game_state.h"
#include "debug_logs.h"
#include "lockstep_core.h"
#include <atomic>
#include <cstring>
#include <cstdio>

#include "state_table.inl"   // g_state_predicates, g_state_force_params, g_state_reachable,
                             // g_state_table, RS_STATE_COUNT

static std::atomic<int> g_current{-1};
static std::atomic<bool> g_cutscene_running{false};
static std::atomic<uint32_t> g_cutscene_skips{0};
static std::atomic<uint32_t> g_level{0};

// Match rd8/rd32 in rt64_render_context.cpp: host RDRAM is byte-swapped within each
// word (the ^3), and a field is read as a big-endian word starting at its exact address.
static inline uint8_t rd8(const uint8_t* rdram, uint32_t off) {
    return (off < 0x800000u) ? rdram[off ^ 3] : 0;
}

static uint32_t rd_be(const uint8_t* rdram, uint32_t kseg0) {
    uint32_t off = kseg0 - 0x80000000u;
    return ((uint32_t)rd8(rdram, off) << 24) | ((uint32_t)rd8(rdram, off + 1) << 16) |
           ((uint32_t)rd8(rdram, off + 2) << 8) | (uint32_t)rd8(rdram, off + 3);
}

static bool pred_holds(const uint8_t* rdram, const RsPredicate& p) {
    uint32_t v = rd_be(rdram, p.addr) & p.mask;
    uint32_t pv = p.value & p.mask;
    switch (p.op) {
        case RS_OP_EQ:    return v == pv;
        case RS_OP_NE:    return v != pv;
        case RS_OP_FLAG:  return v == pv;
        case RS_OP_RANGE: return v >= (p.value & p.mask) && v <= (p.value_hi & p.mask);
        default:          return false;
    }
}

static bool state_matches(const uint8_t* rdram, const RsState& s) {
    if (s.pred_cnt <= 0) return false;
    for (int i = 0; i < s.pred_cnt; ++i) {
        if (!pred_holds(rdram, g_state_predicates[s.pred_off + i])) return false;
    }
    return true;
}

void rs64_state_poll(const uint8_t* rdram) {
    if (!rdram) return;
    int best = -1, best_depth = -1;
    for (int i = 0; i < RS_STATE_COUNT; ++i) {
        if (state_matches(rdram, g_state_table[i]) && g_state_table[i].depth > best_depth) {
            best = i;
            best_depth = g_state_table[i].depth;
        }
    }
    const int prev = g_current.exchange(best, std::memory_order_relaxed);
    static const bool s_log = recomp::dbg::env_on("ROGUESQ_LOG_GAMESTATE");
    if (s_log && (best != prev)) {
        fprintf(stderr, "[gamestate] -> %s\n", (best >= 0) ? g_state_table[best].id : "unknown");
    }

    // A cutscene timeline is playing: gCurrentCutsceneFile (0x800B1904) is loaded and gateCtr (0x800B0B28) is below its end frame (file+0x44, minus the same 0xA margin the game uses).
    // This covers the boot intro, where the "menu" predicate is a false positive (0x800CE730 is still heap), as well as in-mission cutscenes.
    bool running = false;
    uint32_t gate = 0;
    uint32_t endFrame = 0;
    const uint32_t cut = rd_be(rdram, 0x800B1904u);
    if ((cut >= 0x80000000u) && (cut < 0x80800000u)) {
        gate = rd_be(rdram, 0x800B0B28u);
        endFrame = rd_be(rdram, cut + 0x44u);
        running = (endFrame > 0x10u) && (endFrame < 0x100000u) && (gate < endFrame - 0xAu);
    }
    static uint32_t s_gate = 0;
    static uint32_t s_end = 0;
    const bool was_running = g_cutscene_running.load(std::memory_order_relaxed);
    if (rs64::ls::cutscene_skipped(was_running, s_gate, s_end, running)) {
        g_cutscene_skips.fetch_add(1, std::memory_order_relaxed);
        fprintf(stderr, "[cutscene] skipped at frame %u of %u\n", s_gate, s_end);
    }
    if (running && !was_running) {
        fprintf(stderr, "[cutscene] started at frame %u of %u\n", gate, endFrame);
    }
    s_gate = gate;
    s_end = endFrame;
    g_cutscene_running.store(running, std::memory_order_relaxed);
    g_level.store(rd_be(rdram, 0x80130B70u), std::memory_order_relaxed);
}

extern "C" uint32_t rs64_state_level(void) {
    return g_level.load(std::memory_order_relaxed);
}

extern "C" int rs64_state_current(void) {
    return g_current.load(std::memory_order_relaxed);
}

extern "C" const char* rs64_state_current_id(void) {
    int i = g_current.load(std::memory_order_relaxed);
    return (i >= 0 && i < RS_STATE_COUNT) ? g_state_table[i].id : "unknown";
}

// ---- Force: stage values that TOML slot hooks consume at natural transition points. ----
// Never calls a recompiled game function; only stages params.

static struct { int active; int slot; int32_t params[4]; int nparams; } g_pending{0, RS_SLOT_NONE, {0, 0, 0, 0}, 0};

static int find_state(const char* id) {
    for (int i = 0; i < RS_STATE_COUNT; ++i) {
        if (std::strcmp(g_state_table[i].id, id) == 0) return i;
    }
    return -1;
}

extern "C" uint32_t rs64_state_cutscene_skips(void) {
    return g_cutscene_skips.load(std::memory_order_relaxed);
}

extern "C" int rs64_state_in_cinematic(void) {
    static const int s_cine = find_state("cinematic");
    const int cur = g_current.load(std::memory_order_relaxed);
    return g_cutscene_running.load(std::memory_order_relaxed) || ((cur >= 0) && (cur == s_cine));
}

extern "C" int rs64_force_state(const char* id) {
    if (!id) return -1;
    int i = find_state(id);
    if (i < 0) return -1;
    const RsState& s = g_state_table[i];
    if (s.force_slot == RS_SLOT_NONE) return -1;
    g_pending.active = 1;
    g_pending.slot = s.force_slot;
    g_pending.nparams = s.fp_cnt < 4 ? s.fp_cnt : 4;
    for (int k = 0; k < g_pending.nparams; ++k) g_pending.params[k] = g_state_force_params[s.fp_off + k];
    return 0;
}

extern "C" int rs64_force_consume(int slot, int32_t* out, int out_cap) {
    if (!g_pending.active || g_pending.slot != slot) return 0;
    int n = g_pending.nparams < out_cap ? g_pending.nparams : out_cap;
    for (int k = 0; k < n; ++k) out[k] = g_pending.params[k];
    g_pending.active = 0;
    return n;
}
