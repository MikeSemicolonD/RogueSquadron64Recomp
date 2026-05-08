// DPC bridge for RSP graphics ucodes that emit RDP commands directly via
// mtc0 to DPC_START/DPC_END (Factor5 ucode does this). On DPC_END writes
// we forward [start..end) RDRAM bytes to RT64's RDP interpreter (LLE path,
// processDisplayLists with isHLE=false), matching what RT64 would consume
// from raw RDP command bytes.
//
// Reads of DPC_CURRENT/DPC_END return g_rsp_dpc_end so busy-wait loops in
// the ucode see "RDP completed instantly" and exit.

#include <cstdint>
#include <cstdlib>
#include <atomic>
#include "librecomp/rsp.hpp"
#include "ultramodern/events.hpp"

uint32_t g_rsp_dpc_start = 0;
uint32_t g_rsp_dpc_end   = 0;

// Set when an RDP FULL_SYNC (op 0x29) is observed in a submission. The
// factor5_ucode dispatch loop polls this and force-returns Broke so
// task_thread_func can fire sp_complete() and the CPU's GFX_SCHED can
// advance to the next task. Without this, the LLE ucode runs forever
// (the original game halts the RSP externally between frames; we don't
// model that).
std::atomic<bool> g_rsp_full_sync_seen{false};

// RDRAM-relative address of the most recent real FULL_SYNC byte (op 0x29)
// the bridge forwarded. rsp_force_fullsync() re-submits those 8 bytes via
// ultramodern::submit_rdp_range so RT64 sees a fullSync at task-end. Paired
// with the LoadOperation validity check in rt64_state.cpp:fullSyncFramebufferPairTiles
// — without that check, mid-frame fullSync crashes in loadTileOperation.
static std::atomic<uint32_t> g_last_fullsync_addr{0xFFFFFFFFu};

// Per-task RDP byte / command counters. The ucode resets these via
// rsp_task_log_and_reset() at the synthetic-halt site so we get a per-task
// chunk-size distribution.
static std::atomic<uint32_t> g_task_rdp_bytes{0};
static std::atomic<uint32_t> g_task_rdp_cmds{0};
static std::atomic<uint32_t> g_task_rdp_fullsyncs{0};
// Per-task RDP opcode histogram (low 6 bits of first byte = op). Bridge and
// task_log_and_reset both run on the RSP task thread, so plain array is fine.
static uint32_t g_task_op_count[64] = {0};

// Per-task GFX opcode histogram (high byte of DL command word). Counted
// directly from the factor5_ucode dispatch loop — a single increment per
// command, no function call. Same single-thread-only constraint as above.
extern "C" uint32_t g_task_gfx_op_count[256] = {0};

// Per-task G_MOVEMEM index histogram. F3D indices: 0x80 viewport,
// 0x82 lookat_y, 0x84 lookat_x, 0x86..0x94 lights L0..L7, 0x96 txtatt,
// 0x98/9A/9C/9E matrix1-4. Tells us if Factor5 is loading lights every
// frame (animated lighting) or just static state.
extern "C" uint32_t g_task_movemem_idx_count[256] = {0};

// Per-task distinct G_SETCOMBINE mux tracker. Multi-pass rendering would
// produce DIFFERENT (w0, w1) pairs per task — single-pass would repeat the
// same mux. Tells us empirically whether the Pak (5 tris, 161 setCombine
// per task) is one combiner repeated or a multi-pass setup. Capped at 16
// distinct muxes per task; overflow counted separately.
struct TaskCombineEntry { uint32_t w0; uint32_t w1; uint32_t count; };
static TaskCombineEntry g_task_combine[16] = {};
static uint32_t g_task_combine_uniq = 0;
static uint32_t g_task_combine_total = 0;
static uint32_t g_task_combine_overflow = 0;

// True while the most recently-set combiner mux equals the Pak's
// (TEXEL × SHADE = 0xFC127E24/0xFFFFFFFF). Texture-command logging
// gates on this so we only see the Pak's loads, not the 159 text loads.
bool g_in_pak_combiner = false;

// Track the most recent SETTIMG address while in Pak combiner block —
// LOADTLUT consumes whatever SETTIMG address is current (RGBA16 source).
uint32_t g_pak_last_tlut_addr = 0;

// Cinematic / particle combiner instrumentation. Mux 0xFC11FE23 /
// 0xFFFFFFFF dominates task #290+ (count 522 in the captured run) and
// covers the cinematic's TEXRECT-rendered particles (shockwaves,
// fireballs, smoke). Asset lookup func_80057338 was confirmed feeding
// "shock_sml"/"shock_sph" into this combiner block, so this gate
// brackets the active particle-render path.
bool g_in_cine_combiner = false;
uint32_t g_cine_last_settimg_fmt = 0;
uint32_t g_cine_last_settimg_siz = 0;
uint32_t g_cine_last_settimg_addr = 0;
uint32_t g_cine_last_cimg_addr = 0;     // current SET_COLOR_IMAGE target fb

// Shadow of the most recent RDP state-set commands. Used so the
// 64-tri particle mux entry can dump the full draw context (otherMode,
// texture, framebuffer, tile) at the moment the particle combiner is
// activated — that's the state the particle triangles will actually use.
uint32_t g_last_othermode_l = 0;
uint32_t g_last_othermode_h = 0;
uint32_t g_last_settimg_w0 = 0;
uint32_t g_last_settimg_w1 = 0;
uint32_t g_last_setcimg_w0 = 0;
uint32_t g_last_setcimg_w1 = 0;
uint32_t g_last_settile_w0 = 0;
uint32_t g_last_settile_w1 = 0;
uint32_t g_last_fogcolor = 0;     // G_SETFOGCOLOR (0xF8) RGBA
uint32_t g_last_primcolor = 0;    // G_SETPRIMCOLOR (0xFA) RGBA
uint32_t g_last_envcolor = 0;     // G_SETENVCOLOR (0xFB) RGBA
uint32_t g_last_blendcolor = 0;   // G_SETBLENDCOLOR (0xF9) RGBA

namespace {
    // Cached env-var read for dpc_bridge logging. The bridge emits ~1000+
    // lines per second of `[task-brief]`, `[task#N gfx]`, `[dpc-pak]`,
    // `[dpc-cine]`, `[dpc-64tri]`, `[trace] cinematic_drv` etc. during
    // cinematic playback — extremely noisy. Default off; enable when
    // tracing what GFX commands the Factor5 LLE ucode is emitting.
    // ROGUESQ_LOG_DPC=1 (or ROGUESQ_LOG_ALL=1).
    bool log_dpc() {
        static const bool v = []{
            const char *a = std::getenv("ROGUESQ_LOG_ALL");
            if (a && *a && *a != '0') return true;
            const char *e = std::getenv("ROGUESQ_LOG_DPC");
            return e && *e && *e != '0';
        }();
        return v;
    }
}

void rsp_dpc_submit(uint8_t* rdram, uint32_t start, uint32_t end) {
    if (end <= start) {
        return;
    }

    // RT64 expects RDRAM-relative addresses. Factor5 writes KSEG0 (0x80xxxxxx)
    // to DPC_START/END — mask to 24 bits.
    uint32_t start_phys = start & 0x3FFFFFF;
    uint32_t end_phys   = end   & 0x3FFFFFF;

    // The ucode emits one RDP command at a time and bumps DPC_END after each.
    // On real hardware, RDP processes [DPC_CURRENT..DPC_END) once and advances
    // CURRENT. We mirror that: only forward bytes the RT64 RDP hasn't seen yet.
    // Reset on a new DPC_START (KSEG0 base address change).
    static uint32_t s_dl_base = 0;
    static uint32_t s_last_end = 0;

    if (start_phys != s_dl_base) {
        s_dl_base = start_phys;
        s_last_end = start_phys;
    }

    if (end_phys <= s_last_end) {
        return;
    }

    uint32_t submit_lo = s_last_end;
    uint32_t submit_hi = end_phys;
    s_last_end = end_phys;

    g_task_rdp_bytes.fetch_add(submit_hi - submit_lo, std::memory_order_relaxed);
    g_task_rdp_cmds.fetch_add(1, std::memory_order_relaxed);
    {
        int64_t mips_first = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t op = (uint8_t)MEM_B(0, mips_first) & 0x3F;
        g_task_op_count[op]++;
    }

    // PIPESYNC FILTER: Factor5 emits PIPESYNC (op_int 0x27) very heavily.
    // RT64 implicitly maintains pipeline coherence; per-cmd PIPESYNC floods
    // the action_queue. Keep LOADSYNC (0x26), TILESYNC (0x28), FULLSYNC (0x29).
    if ((submit_hi - submit_lo) == 8) {
        int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t b0 = MEM_B(0, mips);
        if ((b0 & 0x3F) == 0x27) {
            (void)rdram;
            return;
        }
        if ((b0 & 0x3F) == 0x29) {
            // FULL_SYNC marks the end of a frame's RDP work — on real hw
            // it raises DP interrupt and the CPU halts the RSP. We don't
            // halt externally, so signal the ucode dispatcher to break out.
            g_rsp_full_sync_seen.store(true, std::memory_order_release);
            g_task_rdp_fullsyncs.fetch_add(1, std::memory_order_relaxed);
            g_last_fullsync_addr.store(submit_lo, std::memory_order_release);
            // Count FULL_SYNC bytes submitted. Compare to State::fullSync
            // count to see if RT64 is observing every one we send.
            static std::atomic<uint64_t> s_fs{0};
            uint64_t n = ++s_fs;
            if (log_dpc() && (n <= 8 || (n & 31) == 0)) {
                fprintf(stderr, "[dpc] FULL_SYNC byte sent #%llu\n",
                    (unsigned long long)n);
                fflush(stderr);
            }
        }
        // SET_COLOR/DEPTH/TEXTURE_IMAGE tracer. Confirmed Factor5 LLE cycles a
        // small (A,B,C) tuple set forever; addFramebufferPair now dedupes
        // search-all by tuple, so per-call printing is no longer useful. Keep
        // the first few prints as a smoke test that the byte stream still has
        // the expected shape.
        uint8_t op6 = b0 & 0x3F;
        // Shadow the most recent state-set commands so the 64-tri particle
        // mux entry log can dump the full draw context. Cheap (always-on).
        if (op6 == 0x22 || op6 == 0x23 || op6 == 0x2F ||
            op6 == 0x3D || op6 == 0x3F || op6 == 0x35 ||
            op6 == 0x38 || op6 == 0x39 || op6 == 0x3A || op6 == 0x3B) {
            uint32_t w0_now = ((uint32_t)(uint8_t)MEM_B(0, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(3, mips));
            uint32_t w1_now = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(7, mips));
            switch (op6) {
                case 0x22: g_last_othermode_l = w1_now; break;
                case 0x23: g_last_othermode_h = w1_now; break;
                case 0x2F: g_last_othermode_h = w0_now; g_last_othermode_l = w1_now; break;
                case 0x3D: g_last_settimg_w0 = w0_now; g_last_settimg_w1 = w1_now; break;
                case 0x3F: g_last_setcimg_w0 = w0_now; g_last_setcimg_w1 = w1_now; break;
                case 0x35: g_last_settile_w0 = w0_now; g_last_settile_w1 = w1_now; break;
                case 0x38: g_last_fogcolor = w1_now; break;
                case 0x39: g_last_blendcolor = w1_now; break;
                case 0x3A: g_last_primcolor = w1_now; break;
                case 0x3B: g_last_envcolor = w1_now; break;
            }
        }
        // G_SETPRIMCOLOR (0xFA, masked to 0x3A) and G_SETENVCOLOR (0xFB,
        // masked to 0x3B): log the RGBA values being loaded. PRIM modulates
        // TEXEL in many Factor5 combiner modes; if PRIM RGBA is dimmer than
        // expected (e.g. 0xC0 instead of 0xFF) this could explain residual
        // saturation gap from ideal.
        if (op6 == 0x3A || op6 == 0x3B) {
            static std::atomic<uint64_t> s_prim{0}, s_env{0};
            uint32_t w1 = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                          ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                          ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                          ((uint32_t)(uint8_t)MEM_B(7, mips));
            uint64_t n = (op6 == 0x3A) ? ++s_prim : ++s_env;
            if (log_dpc() && (n <= 12 || n == 100 || n == 1000)) {
                const char *name = (op6 == 0x3A) ? "PRIM" : "ENV";
                fprintf(stderr, "[dpc] G_SET%s_COLOR #%llu RGBA=%02X %02X %02X %02X (w1=0x%08X)\n",
                    name, (unsigned long long)n,
                    (w1 >> 24) & 0xFF,
                    (w1 >> 16) & 0xFF,
                    (w1 >>  8) & 0xFF,
                    (w1 >>  0) & 0xFF,
                    w1);
                fflush(stderr);
            }
            // ROGUESQ_PRIM_FF: experimental override — force PRIM_COLOR to
            // (FF,FF,FF, original_alpha). Tests whether the (EB,E1,D7) warm
            // off-white tint we observed accounts for the residual "less
            // saturated reds" gap from ideal-initial.PNG.
            if (op6 == 0x3A) {
                static const bool prim_full = []() {
                    const char *e = std::getenv("ROGUESQ_PRIM_FF");
                    return e && *e && *e != '0';
                }();
                if (prim_full) {
                    uint8_t a = (w1 >> 0) & 0xFF;
                    // Skip the engine-glow outlier so we don't make orange flames white
                    bool is_glow = ((w1 >> 24) & 0xFF) < 0xC0 ||
                                   ((w1 >> 8) & 0xFF) < 0x40;
                    if (!is_glow) {
                        // Rewrite PRIM RGB to FF FF FF, keep alpha + alpha-only path
                        // (rdram bytes 4-7) — write back via direct rdram poke
                        int64_t mips_w1_lo = mips + 4;
                        // High byte of w1 is at offset 4
                        // Order: w1 bytes are at offsets 4..7 in big-endian.
                        // But MEM_B is endian-aware; let's set w1 directly as ulong
                        // and convert. Use the SAME MEM_B-style writes.
                        // For Big-endian w1: byte4=R(MSB), byte5=G, byte6=B, byte7=A.
                        // Replace bytes 4,5,6 with 0xFF.
                        // (We can't easily write through MEM_B without macro, so use rdram.)
                        // submit_lo is RDRAM-relative byte offset.
                        uint8_t *p = rdram + (submit_lo ^ 0);  // RDRAM is byte-swapped: index ^ 3
                        // RDRAM is stored little-endian internally? Use MIPS XOR-3 idiom.
                        rdram[(submit_lo + 4) ^ 3] = 0xFF;
                        rdram[(submit_lo + 5) ^ 3] = 0xFF;
                        rdram[(submit_lo + 6) ^ 3] = 0xFF;
                        // alpha (offset 7) preserved
                        static std::atomic<uint64_t> s_overridden{0};
                        uint64_t v = ++s_overridden;
                        if (v <= 3) {
                            fprintf(stderr, "[dpc] PRIM override -> FF FF FF %02X (#%llu)\n",
                                a, (unsigned long long)v);
                            fflush(stderr);
                        }
                    }
                }
            }
        }
        // G_SETCOMBINE (0xFC, masked to 0x3C): log first few combiner modes
        // task #256 (memory module) sets the combiner 161 times per task.
        // Combiner modes determine pixel-level color compositing — relevant
        // to env-map blending. Check distinct (w0, w1) pairs.
        if (op6 == 0x3C) {
            uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                          ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                          ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                          ((uint32_t)(uint8_t)MEM_B(3, mips));
            uint32_t w1 = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                          ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                          ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                          ((uint32_t)(uint8_t)MEM_B(7, mips));
            // Linear scan (max 16 entries — fast). Multi-pass rendering with
            // multiple combiner formulas will populate multiple slots.
            g_task_combine_total++;
            bool found = false;
            for (uint32_t i = 0; i < g_task_combine_uniq; i++) {
                if (g_task_combine[i].w0 == w0 && g_task_combine[i].w1 == w1) {
                    g_task_combine[i].count++;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (g_task_combine_uniq < 16) {
                    g_task_combine[g_task_combine_uniq].w0 = w0;
                    g_task_combine[g_task_combine_uniq].w1 = w1;
                    g_task_combine[g_task_combine_uniq].count = 1;
                    g_task_combine_uniq++;
                } else {
                    g_task_combine_overflow++;
                }
            }
            // Pak combiner = TEXEL × SHADE (mux 0xFC127E24/0xFFFFFFFF).
            // Track entry/exit so we can log only the Pak's texture commands
            // and skip the 159 setCombines for text rendering.
            extern bool g_in_pak_combiner;
            g_in_pak_combiner = (w0 == 0xFC127E24u && w1 == 0xFFFFFFFFu);
            if (g_in_pak_combiner && log_dpc()) {
                fprintf(stderr, "[dpc-pak] >>> ENTER Pak combiner block\n");
                fflush(stderr);
            }
            // Cinematic-particle combiner (mux 0xFC11FE23/0xFFFFFFFF).
            // 522 calls/cinematic-frame in task #290; covers the
            // particle/shockwave/fireball texrects.
            extern bool g_in_cine_combiner;
            bool was_cine = g_in_cine_combiner;
            g_in_cine_combiner = (w0 == 0xFC11FE23u && w1 == 0xFFFFFFFFu);
            if (g_in_cine_combiner && !was_cine && log_dpc()) {
                static std::atomic<uint64_t> s_first{0};
                uint64_t v = ++s_first;
                if (v <= 4) {
                    fprintf(stderr, "[dpc-cine] >>> ENTER cinematic particle combiner block #%llu\n",
                        (unsigned long long)v);
                    fflush(stderr);
                }
            }
            // 64-triangle particle mux: TEXEL×SHADE used by the actual 3D
            // particle billboards (32 quads = 64 triangles per cinematic
            // frame in task #290+). User confirmed these triangles do NOT
            // turn magenta with the alphaCvgSel diagnostic, so they use a
            // different render-mode preset. Capture full draw context to
            // see why they're not visible.
            static bool s_in_64tri = false;
            const bool now_64tri = (w0 == 0xFC127FFFu && w1 == 0xFFFFFE3Fu);
            if (now_64tri && !s_in_64tri && log_dpc()) {
                static std::atomic<uint64_t> s_first{0};
                uint64_t v = ++s_first;
                // Log first 8, then every 64th — boot uses this mux too,
                // so the first few entries are N64-logo state, not cinematic.
                // Cinematic phase repeats the mux many times per second.
                if (v <= 8 || (v & 63) == 0) {
                    extern uint32_t g_last_othermode_l, g_last_othermode_h;
                    extern uint32_t g_last_settimg_w0, g_last_settimg_w1;
                    extern uint32_t g_last_setcimg_w0, g_last_setcimg_w1;
                    extern uint32_t g_last_settile_w0, g_last_settile_w1;
                    extern uint32_t g_last_fogcolor, g_last_primcolor;
                    extern uint32_t g_last_envcolor, g_last_blendcolor;
                    fprintf(stderr,
                        "[dpc-64tri] ENTER #%llu  L=0x%08X H=0x%08X  TIMG=[%08X %08X]  CIMG=[%08X %08X]  TILE=[%08X %08X]  FOG=%08X PRIM=%08X ENV=%08X BLEND=%08X\n",
                        (unsigned long long)v,
                        g_last_othermode_l, g_last_othermode_h,
                        g_last_settimg_w0, g_last_settimg_w1,
                        g_last_setcimg_w0, g_last_setcimg_w1,
                        g_last_settile_w0, g_last_settile_w1,
                        g_last_fogcolor, g_last_primcolor,
                        g_last_envcolor, g_last_blendcolor);
                    fflush(stderr);
                }
            }
            s_in_64tri = now_64tri;
        }
        // Inside the Pak combiner block, log every texture-loading
        // command so we can see the format / palette / address chain
        // for the Pak's draws. Outside the block, fall back to the
        // original "first 8" capture for SET_TEXTURE/DEPTH/COLOR_IMAGE.
        // Per-command volume is high (200+ lines per cinematic frame);
        // gated on ROGUESQ_LOG_DPC.
        if (g_in_pak_combiner && log_dpc()) {
            const char *name = nullptr;
            switch (op6) {
                case 0x30: name = "G_LOADTLUT";      break;
                case 0x32: name = "G_SETTILESIZE";   break;
                case 0x33: name = "G_LOADBLOCK";     break;
                case 0x34: name = "G_LOADTILE";      break;
                case 0x35: name = "G_SETTILE";       break;
                case 0x3D: name = "G_SETTIMG";       break;
                case 0x3F: name = "G_SETCIMG";       break;
                case 0x05: case 0x07: case 0x08: case 0x09:
                case 0x0A: case 0x0B: case 0x0C: case 0x0D:
                case 0x0E: case 0x0F: name = "G_TRI"; break;
                default: break;
            }
            if (name) {
                uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(3, mips));
                uint32_t w1 = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(7, mips));
                if (op6 == 0x3D) {
                    // SET_TEXTURE_IMAGE: w0 has fmt(31-29)/siz(20-19)/width(11-0)
                    uint32_t fmt = (w0 >> 21) & 0x7;
                    uint32_t siz = (w0 >> 19) & 0x3;
                    uint32_t width = (w0 & 0xFFF) + 1;
                    uint32_t addr = w1 & 0x00FFFFFF;
                    const char *fmt_name = "?";
                    switch (fmt) {
                        case 0: fmt_name = "RGBA"; break;
                        case 1: fmt_name = "YUV";  break;
                        case 2: fmt_name = "CI";   break;
                        case 3: fmt_name = "IA";   break;
                        case 4: fmt_name = "I";    break;
                    }
                    const char *siz_name =
                        (siz == 0) ? "4b" : (siz == 1) ? "8b" :
                        (siz == 2) ? "16b" : "32b";
                    fprintf(stderr,
                        "[dpc-pak] %s fmt=%s siz=%s width=%u addr=0x%08X\n",
                        name, fmt_name, siz_name, width, addr);
                    // Cache for upcoming LOADTLUT — TLUT data lives at the
                    // RGBA16 SETTIMG that immediately precedes a LOADTLUT.
                    if (fmt == 0 /* RGBA */) {
                        g_pak_last_tlut_addr = addr;
                    }
                } else if (op6 == 0x35) {
                    // SET_TILE: w0 = fmt/siz/line/tmem/tile, w1 = palette/clamp/mask/shift
                    uint32_t fmt = (w0 >> 21) & 0x7;
                    uint32_t siz = (w0 >> 19) & 0x3;
                    uint32_t line = (w0 >> 9) & 0x1FF;
                    uint32_t tmem = w0 & 0x1FF;
                    uint32_t tile = (w1 >> 24) & 0x7;
                    uint32_t palette = (w1 >> 20) & 0xF;
                    fprintf(stderr,
                        "[dpc-pak] %s tile=%u fmt=%u siz=%u line=%u tmem=0x%X palette=%u\n",
                        name, tile, fmt, siz, line, tmem, palette);
                } else if (op6 == 0x33) {
                    // LOAD_BLOCK
                    uint32_t uls = (w0 >> 12) & 0xFFF;
                    uint32_t ult = w0 & 0xFFF;
                    uint32_t tile = (w1 >> 24) & 0x7;
                    uint32_t lrs = (w1 >> 12) & 0xFFF;
                    uint32_t dxt = w1 & 0xFFF;
                    fprintf(stderr,
                        "[dpc-pak] %s tile=%u uls=%u ult=%u texels=%u dxt=%u\n",
                        name, tile, uls, ult, lrs + 1, dxt);
                } else if (op6 == 0x30) {
                    uint32_t tile = (w1 >> 24) & 0x7;
                    fprintf(stderr,
                        "[dpc-pak] %s tile=%u w0=0x%08X w1=0x%08X\n",
                        name, tile, w0, w1);
                    // Dump the 16 TLUT entries we just loaded. They live
                    // at the most-recent SETTIMG address (RGBA16 format).
                    extern uint32_t g_pak_last_tlut_addr;
                    if (g_pak_last_tlut_addr != 0) {
                        fprintf(stderr, "[dpc-pak] TLUT @ 0x%08X (RGBA16):\n",
                            g_pak_last_tlut_addr);
                        // 16 entries × 2 bytes each = 32 bytes
                        uint8_t* base = rdram + (g_pak_last_tlut_addr ^ 3);
                        // RDRAM is BE; ^3 flips to RSP-side aligned reads.
                        for (int i = 0; i < 16; i++) {
                            uint32_t off = g_pak_last_tlut_addr + i * 2;
                            uint8_t hi = rdram[(off + 0) ^ 3];
                            uint8_t lo = rdram[(off + 1) ^ 3];
                            uint16_t rgba16 = ((uint16_t)hi << 8) | lo;
                            int r = (rgba16 >> 11) & 0x1F;
                            int g = (rgba16 >> 6) & 0x1F;
                            int b = (rgba16 >> 1) & 0x1F;
                            int a = rgba16 & 0x1;
                            fprintf(stderr,
                                "  [%2d] rgba16=0x%04X  R=%2d G=%2d B=%2d A=%d  "
                                "(R8=%3d G8=%3d B8=%3d)\n",
                                i, rgba16, r, g, b, a,
                                (r << 3) | (r >> 2),
                                (g << 3) | (g >> 2),
                                (b << 3) | (b >> 2));
                        }
                        (void)base;
                    }
                } else {
                    fprintf(stderr,
                        "[dpc-pak] %s w0=0x%08X w1=0x%08X\n", name, w0, w1);
                }
                fflush(stderr);
            }
        }
        // Track latest SETTIMG / SETCIMG GLOBALLY (regardless of which
        // combiner is active) so we have correlation context when the
        // next texrect fires inside any block.
        if (op6 == 0x3D) {
            uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                          ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                          ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                          ((uint32_t)(uint8_t)MEM_B(3, mips));
            uint32_t w1 = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                          ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                          ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                          ((uint32_t)(uint8_t)MEM_B(7, mips));
            g_cine_last_settimg_fmt = (w0 >> 21) & 0x7;
            g_cine_last_settimg_siz = (w0 >> 19) & 0x3;
            g_cine_last_settimg_addr = w1 & 0x00FFFFFF;
        }
        if (op6 == 0x3F) {
            uint32_t w1 = ((uint32_t)MEM_B(4, mips) << 24) |
                          ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                          ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                          ((uint32_t)(uint8_t)MEM_B(7, mips));
            g_cine_last_cimg_addr = w1 & 0x00FFFFFF;
        }
        // Cinematic particle combiner — log first ~32 texrects + their
        // tile/texture/CIMG context so we can see whether particles are
        // on-screen, what fb they target, what texture they sample.
        if (g_in_cine_combiner) {
            // TEXRECT (0xE4 -> low6 = 0x24): log geometry + context.
            if (op6 == 0x24) {
                static std::atomic<uint64_t> s_rect{0};
                uint64_t v = ++s_rect;
                if (v <= 32) {
                    uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                                  ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                                  ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                                  ((uint32_t)(uint8_t)MEM_B(3, mips));
                    uint32_t w1 = ((uint32_t)MEM_B(4, mips) << 24) |
                                  ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                                  ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                                  ((uint32_t)(uint8_t)MEM_B(7, mips));
                    // Standard RDP TEXRECT: lrx/lry/tile/ulx/uly in 10.2 fixed
                    uint32_t lrx = (w0 >> 12) & 0xFFF;
                    uint32_t lry = w0 & 0xFFF;
                    uint32_t tile = (w1 >> 24) & 0x7;
                    uint32_t ulx = (w1 >> 12) & 0xFFF;
                    uint32_t uly = w1 & 0xFFF;
                    fprintf(stderr,
                        "[dpc-cine] TEXRECT #%llu ul=(%u,%u) lr=(%u,%u) "
                        "size=(%u,%u) tile=%u  fb=0x%08X texSrc=0x%08X fmt=%u siz=%u\n",
                        (unsigned long long)v,
                        ulx >> 2, uly >> 2, lrx >> 2, lry >> 2,
                        (lrx - ulx) >> 2, (lry - uly) >> 2,
                        tile, g_cine_last_cimg_addr, g_cine_last_settimg_addr,
                        g_cine_last_settimg_fmt, g_cine_last_settimg_siz);
                    fflush(stderr);
                }
            }
        }
        if (!g_in_pak_combiner && (op6 == 0x3D || op6 == 0x3E || op6 == 0x3F)) {
            static std::atomic<uint64_t> s_seen{0};
            uint64_t n = ++s_seen;
            if (log_dpc() && n <= 8) {
                uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(3, mips));
                uint32_t w1 = ((uint32_t)(uint8_t)MEM_B(4, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(7, mips));
                const char *name = (op6 == 0x3F) ? "SET_COLOR_IMAGE" :
                                   (op6 == 0x3E) ? "SET_DEPTH_IMAGE" : "SET_TEXTURE_IMAGE";
                fprintf(stderr, "[dpc] %s #%llu w0=0x%08X w1=0x%08X (addr=0x%08X)\n",
                    name, (unsigned long long)n, w0, w1, w1 & 0x00FFFFFF);
                fflush(stderr);
            }
        }
    }

    // 16-byte LLE TEXRECT chunk: Factor5 emits these for particle billboards
    // (~522/cinematic-frame). The 8-byte block above misses them because they
    // arrive as a single 16-byte submission. Log geometry + texture context
    // when the cinematic combiner is active so we can see whether particles
    // are on-screen, hitting the right framebuffer, etc.
    if ((submit_hi - submit_lo) == 16 && g_in_cine_combiner) {
        int64_t mips = (int64_t)(int32_t)(submit_lo + 0x80000000);
        uint8_t b0 = MEM_B(0, mips);
        uint8_t op6 = b0 & 0x3F;
        if (op6 == 0x24 || op6 == 0x25) { // TEXRECT or TEXRECTFLIP
            static std::atomic<uint64_t> s_rect{0};
            uint64_t v = ++s_rect;
            if (v <= 32) {
                uint32_t w0 = ((uint32_t)MEM_B(0, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(1, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(2, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(3, mips));
                uint32_t w1 = ((uint32_t)MEM_B(4, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(5, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(6, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(7, mips));
                uint32_t w2 = ((uint32_t)MEM_B(8, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(9, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(10, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(11, mips));
                uint32_t w3 = ((uint32_t)MEM_B(12, mips) << 24) |
                              ((uint32_t)(uint8_t)MEM_B(13, mips) << 16) |
                              ((uint32_t)(uint8_t)MEM_B(14, mips) <<  8) |
                              ((uint32_t)(uint8_t)MEM_B(15, mips));
                uint32_t lrx = (w0 >> 12) & 0xFFF;
                uint32_t lry = w0 & 0xFFF;
                uint32_t tile = (w1 >> 24) & 0x7;
                uint32_t ulx = (w1 >> 12) & 0xFFF;
                uint32_t uly = w1 & 0xFFF;
                fprintf(stderr,
                    "[dpc-cine] TEXRECT%s #%llu ul=(%u,%u) lr=(%u,%u) "
                    "size=(%u,%u) tile=%u  fb=0x%08X tex=0x%08X fmt=%u siz=%u "
                    "stuv=0x%08X dxdy=0x%08X\n",
                    (op6 == 0x25) ? "_FLIP" : "",
                    (unsigned long long)v,
                    ulx >> 2, uly >> 2, lrx >> 2, lry >> 2,
                    (lrx > ulx) ? ((lrx - ulx) >> 2) : 0,
                    (lry > uly) ? ((lry - uly) >> 2) : 0,
                    tile, g_cine_last_cimg_addr, g_cine_last_settimg_addr,
                    g_cine_last_settimg_fmt, g_cine_last_settimg_siz,
                    w2, w3);
                fflush(stderr);
            }
        }
    }

    (void)rdram;
    ultramodern::submit_rdp_range(submit_lo, submit_hi);
}

// Called from the factor5_ucode synthetic-halt site. Logs per-task RDP byte
// volume / command count / FULL_SYNC count, then resets the counters for the
// next task. Throttled to first 16 tasks + every 256 thereafter so we get the
// distribution without flooding the SDL message pump.
extern "C" void rsp_task_log_and_reset(uint32_t iters, uint32_t data_size, uint32_t r17,
                                        uint32_t cmd_w0, uint32_t cmd_w1) {
    uint32_t bytes = g_task_rdp_bytes.exchange(0, std::memory_order_acq_rel);
    uint32_t cmds  = g_task_rdp_cmds.exchange(0, std::memory_order_acq_rel);
    uint32_t fs    = g_task_rdp_fullsyncs.exchange(0, std::memory_order_acq_rel);
    static std::atomic<uint64_t> s_n{0};
    uint64_t n = ++s_n;
    bool capped_no_fs = (iters > 16000 && fs == 0);
    // Throttle CAPPED-NO-FS spam — at 13/s with histogram dump it's ~117
    // lines/sec, enough to starve the SDL pump and stall GFX scheduling.
    static std::atomic<uint64_t> s_capped_count{0};
    bool log_capped = false;
    if (capped_no_fs) {
        uint64_t cn = ++s_capped_count;
        log_capped = (cn <= 8 || (cn & 4095) == 0);
    }
    // Targeted task numbers for particle-explosion phase (per user
    // observation). Mirror the dump_full list below so the outer gate
    // doesn't suppress these full dumps.
    const bool target_task_outer = (n == 256 || n == 384 || n == 512 ||
                                    n == 640 || n == 768 || n == 896 ||
                                    n == 290 || n == 320 || n == 350);
    // Brief per-task summary on EVERY task — lets us spot stages where
    // triangle/texrect submission goes to zero (e.g. blank cinematic stages
    // where particles SHOULD be rendering but the foreground is missing).
    if (log_dpc()) {
        // Brief per-task summary. Useful for spotting cinematic stages
        // where triangle/texrect submission goes to zero or spikes (e.g.
        // tasks #948-949 with tri=8000+ are the explosion sphere bursts).
        const uint32_t tri_count = g_task_gfx_op_count[0x05] + g_task_gfx_op_count[0x07];
        const uint32_t texrect_count = g_task_gfx_op_count[0xE4] + g_task_gfx_op_count[0xE5];
        fprintf(stderr,
            "[task-brief] #%llu tri=%u texrect=%u fs=%u iters=%u\n",
            (unsigned long long)n, tri_count, texrect_count, fs, iters);
    }
    if (log_dpc() && (n <= 16 || (n & 255) == 0 || target_task_outer || log_capped)) {
        // Detailed per-task dump (cmd words + rdp bytes + opcode histogram).
        // Triggered for first 16 tasks, every 256th, target tasks, and
        // any iteration-capped task. Heavy output; gated on ROGUESQ_LOG_DPC.
        fprintf(stderr,
            "[task] #%llu iters=%u data_size=%u r17=0x%X cmd=[%08X %08X] rdp_bytes=%u cmds=%u fullsyncs=%u%s\n",
            (unsigned long long)n, iters, data_size, r17, cmd_w0, cmd_w1,
            bytes, cmds, fs,
            capped_no_fs ? " CAPPED-NO-FS" : "");
        // Top-8 gfx opcode dump (full byte 0xB0..0xFF range plus low ops).
        // Identifies what Factor5 actually dispatches per task — including
        // lighting-related ops (G_MOVEMEM 0x03/0xDB, G_SETLIGHTS macros).
        // First capped task ALSO dumps every non-zero op for full coverage.
        {
            uint32_t copy[256];
            for (int i = 0; i < 256; i++) copy[i] = g_task_gfx_op_count[i];
            for (int slot = 0; slot < 8; slot++) {
                int max_idx = 0;
                for (int i = 0; i < 256; i++) {
                    if (copy[i] > copy[max_idx]) max_idx = i;
                }
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  [task#%llu gfx] op 0x%02X = %u\n",
                    (unsigned long long)n, (unsigned)max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            // Full non-zero dump for first 4 tasks (attribution screen) +
            // first task with substantial work (memory module phase) +
            // first capped-no-fs task (cinematic). Three phases captured.
            static bool s_first_capped_dumped = false;
            static bool s_first_substantial_dumped = false;
            const bool substantial = (iters > 1000 && !s_first_substantial_dumped);
            if (substantial) s_first_substantial_dumped = true;
            // Targeted task numbers — user observation that #512 / #768 are
            // when particle explosions are expected. Force a full op +
            // setcombine dump for those frames so we can identify what
            // muxes/triangles the explosion phase emits beyond the
            // already-known fb-blit (mux 0xFC11FE23) and 64-tri (mux
            // 0xFC127FFF) pair seen in task #290.
            const bool target_task = (n == 512 || n == 768 || n == 256 ||
                                      n == 384 || n == 640 || n == 896 ||
                                      n == 290 || n == 320 || n == 350);
            const bool dump_full = (n <= 4) || substantial || target_task ||
                                   (capped_no_fs && !s_first_capped_dumped);
            if (dump_full) {
                if (capped_no_fs) s_first_capped_dumped = true;
                fprintf(stderr, "  [task#%llu gfx-FULL] all non-zero ops:\n",
                    (unsigned long long)n);
                for (int i = 0; i < 256; i++) {
                    if (g_task_gfx_op_count[i] > 0) {
                        fprintf(stderr, "    op 0x%02X = %u\n",
                            (unsigned)i, g_task_gfx_op_count[i]);
                    }
                }
                // Distinct G_SETCOMBINE muxes for this task. Multi-pass
                // rendering shows up here as 2+ entries with different muxes.
                fprintf(stderr,
                    "  [task#%llu setcombine] total=%u uniq=%u overflow=%u\n",
                    (unsigned long long)n, g_task_combine_total,
                    g_task_combine_uniq, g_task_combine_overflow);
                for (uint32_t i = 0; i < g_task_combine_uniq; i++) {
                    fprintf(stderr, "    mux #%u w0=0x%08X w1=0x%08X count=%u\n",
                        i, g_task_combine[i].w0, g_task_combine[i].w1,
                        g_task_combine[i].count);
                }
                // G_MOVEMEM index breakdown: which kinds of state are being
                // loaded (lights, matrices, viewport, etc.).
                fprintf(stderr, "  [task#%llu movemem-idx]\n",
                    (unsigned long long)n);
                for (int i = 0; i < 256; i++) {
                    if (g_task_movemem_idx_count[i] > 0) {
                        const char *name = "?";
                        switch (i) {
                            case 0x80: name = "VIEWPORT"; break;
                            case 0x82: name = "LOOKAT_Y"; break;
                            case 0x84: name = "LOOKAT_X"; break;
                            case 0x86: name = "L0"; break;
                            case 0x88: name = "L1"; break;
                            case 0x8A: name = "L2"; break;
                            case 0x8C: name = "L3"; break;
                            case 0x8E: name = "L4"; break;
                            case 0x90: name = "L5"; break;
                            case 0x92: name = "L6"; break;
                            case 0x94: name = "L7"; break;
                            case 0x96: name = "TXTATT"; break;
                            case 0x98: name = "MTX_2"; break;
                            case 0x9A: name = "MTX_3"; break;
                            case 0x9C: name = "MTX_4"; break;
                            case 0x9E: name = "MTX_1"; break;
                        }
                        fprintf(stderr, "    idx 0x%02X (%s) = %u\n",
                            (unsigned)i, name, g_task_movemem_idx_count[i]);
                    }
                }
            }
            fflush(stderr);
        }
        fflush(stderr);
        // For cap-without-fullSync tasks, dump the opcode histogram (top 8).
        // Helps identify the loop: if one opcode dominates, that's the stuck
        // command. Reset is unconditional below.
        if (log_capped) {
            uint32_t copy[64];
            for (int i = 0; i < 64; i++) copy[i] = g_task_op_count[i];
            // Find top 8 by count.
            for (int slot = 0; slot < 8; slot++) {
                int max_idx = 0;
                for (int i = 0; i < 64; i++) {
                    if (copy[i] > copy[max_idx]) max_idx = i;
                }
                if (copy[max_idx] == 0) break;
                fprintf(stderr, "  [task#%llu hist] op 0x%02X = %u\n",
                    (unsigned long long)n, (unsigned)max_idx, copy[max_idx]);
                copy[max_idx] = 0;
            }
            fflush(stderr);
        }
    }
    for (int i = 0; i < 64; i++) g_task_op_count[i] = 0;
    for (int i = 0; i < 256; i++) g_task_gfx_op_count[i] = 0;
    for (int i = 0; i < 256; i++) g_task_movemem_idx_count[i] = 0;
    for (int i = 0; i < 16; i++) { g_task_combine[i].w0 = 0; g_task_combine[i].w1 = 0; g_task_combine[i].count = 0; }
    g_task_combine_uniq = 0;
    g_task_combine_total = 0;
    g_task_combine_overflow = 0;
}

// Submit a synthetic FULL_SYNC by re-using the RDRAM bytes of the most recent
// real FULL_SYNC. Called from factor5_ucode at synthetic-halt for tasks that
// don't naturally emit op 0x29 (cinematic phase). The action_queue is FIFO,
// so all prior submit_rdp_range calls process first; this fullSync runs after
// state has all the task's geometry. Paired with the LoadOperation validity
// check in rt64_state.cpp so partial tile state is skipped, not crashed.
//
// Env var ROGUESQ_NO_SYNTH_FULLSYNC=1 disables this and reverts to the slow
// "wait for real FULL_SYNC bytes" path (~5 fps cinematic) — useful for A/B
// comparison against the higher-rate-but-stalls-at-60s default behavior.
extern "C" void rsp_force_fullsync() {
    static const bool disabled = []() {
        const char *e = std::getenv("ROGUESQ_NO_SYNTH_FULLSYNC");
        bool d = (e != nullptr && *e != '\0' && *e != '0');
        if (d) {
            fprintf(stderr, "[dpc] synthetic FULL_SYNC injection DISABLED via env\n");
            fflush(stderr);
        }
        return d;
    }();
    if (disabled) return;
    uint32_t addr = g_last_fullsync_addr.load(std::memory_order_acquire);
    if (addr == 0xFFFFFFFFu) {
        return;
    }
    ultramodern::submit_rdp_range(addr, addr + 8);
}
