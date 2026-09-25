#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <cstring>
#include <csignal>
#include <vector>
#include <cinttypes>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <exception>
#include <typeinfo>
static unsigned g_rs64_audio_underruns = 0;   // dry-queue arrivals (see queue_samples)

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/events.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/mods.hpp"
#include "common/rt64_common.h"
#include "imgui/imgui.h"
#include "rhi/rt64_render_hooks.h"
#include "input_bindings.h"
#include "debug_logs.h"
#include "main.h"                 // this file's own exports (fullscreen/quit hooks)
#include "upstream_compat.h"      // rs64_vi_driven
#include "hook_helpers.h"         // g_vi_tick, g_boot_pulse_start
#include "nav_sequencer.h"        // rs64_nav_consume, rs64_nav_tick
#include "rt64_render_context.h"  // recomp::create_render_context
#include "../rsp/dpc_bridge.h"    // rs64_dpc_drain_histogram
#include <mutex>

using recomp::dbg::env_on;
using recomp::dbg::env_int;

#define SDL_MAIN_HANDLED
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#pragma comment(lib, "Dbghelp.lib")
#include <crtdbg.h>
#include "SDL.h"
#include "SDL_syswm.h"
#else
#include "SDL2/SDL.h"
// SDL_syswm.h is only needed for the Win32 HWND path in create_window(); on
// Linux it pulls X11 <Xlib.h>, whose None/Bool/Status macros collide with C++
// enum members (e.g. ultramodern::input::Pak::None), so it is not included here.
static inline uint64_t GetTickCount64() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

// N64 button bitmasks (from libultra PR/controller.h)
#define N64_A_BUTTON     0x8000
#define N64_B_BUTTON     0x4000
#define N64_Z_TRIG       0x2000
#define N64_START_BUTTON 0x1000
#define N64_U_JPAD       0x0800
#define N64_D_JPAD       0x0400
#define N64_L_JPAD       0x0200
#define N64_R_JPAD       0x0100
#define N64_L_TRIG       0x0020
#define N64_R_TRIG       0x0010
#define N64_U_CBUTTONS   0x0008
#define N64_D_CBUTTONS   0x0004
#define N64_L_CBUTTONS   0x0002
#define N64_R_CBUTTONS   0x0001

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void rs64_register_overlays();
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// Big-endian RDRAM accessors (RDRAM is host-LE-stored; XOR-3 swizzles byte order).
// One definition for the whole file — the diagnostic blocks below used to re-inline
// this byteswap (or redeclare local rd32/wr32 lambdas) at a dozen sites.
static inline uint32_t rdram_be32(const uint8_t* rdram, uint32_t off) {
    return (uint32_t(rdram[(off + 0) ^ 3]) << 24) | (uint32_t(rdram[(off + 1) ^ 3]) << 16) |
           (uint32_t(rdram[(off + 2) ^ 3]) <<  8) |  uint32_t(rdram[(off + 3) ^ 3]);
}
static inline uint16_t rdram_be16(const uint8_t* rdram, uint32_t off) {
    return (uint16_t)((uint16_t(rdram[(off + 0) ^ 3]) << 8) | uint16_t(rdram[(off + 1) ^ 3]));
}
static inline void rdram_wr32(uint8_t* rdram, uint32_t off, uint32_t v) {
    rdram[(off + 0) ^ 3] = (uint8_t)(v >> 24); rdram[(off + 1) ^ 3] = (uint8_t)(v >> 16);
    rdram[(off + 2) ^ 3] = (uint8_t)(v >>  8); rdram[(off + 3) ^ 3] = (uint8_t)v;
}

// Wrapper: captures rdram into the watchdog global on first call so the
// poll/diagnostic threads can attach. Forwards to the real entrypoint.
extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw;
extern "C" void rs64_entrypoint_with_rdram_capture(uint8_t* rdram, recomp_context* ctx) {
    g_recomp_rdram_for_wp_raw = rdram;
    recomp_entrypoint(rdram, ctx);
}
// The game's N64 "main" function — renamed to avoid clash with C main()
extern "C" void rs_main(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// ---------------------------------------------------------------------------
// RSP microcode dispatch
// ---------------------------------------------------------------------------
extern RspExitReason factor5_ucode(uint8_t* rdram, uint32_t ucode_addr);
extern RspExitReason factor5_boot (uint8_t* rdram, uint32_t ucode_addr);
extern RspExitReason musyx_audio  (uint8_t* rdram, uint32_t ucode_addr);
extern uint8_t dmem[];
// Recompiled audio funcs, for the offline song-render trigger (ROGUESQ_RENDER_SONG).
extern "C" void loadSongAssetByName(uint8_t*, recomp_context*);
extern "C" void findAudioChannelById(uint8_t*, recomp_context*);
extern "C" void playSongById(uint8_t*, recomp_context*);

// Cached for the next ucode invocation. get_rsp_microcode is called with the
// OSTask immediately before the ucode runs on the same thread.
static thread_local uint32_t s_pending_task_data_ptr  = 0;
static thread_local uint32_t s_pending_task_ucode_data = 0;
static thread_local uint32_t s_pending_task_ucode_data_size = 0;

// Rogue Squadron uses Factor5's MusyX audio ucode, NOT stock aspMain. Running
// aspMain on MusyX-formatted task data produces garbage or hangs the audio
// thread (no shared format). Until MusyX has a real recomp pass (see
// project_audio_musyx.md), stub all audio tasks: return Broke immediately so
// the game thinks the task completed, sp_complete() fires, and play continues.
// Cost: no audio. Trade-off: keeps the rest of the game responsive.
extern "C" uint32_t g_audio_ucode_data_addr;
extern "C" uint32_t g_audio_ucode_data_size;
uint32_t g_audio_ucode_data_addr = 0;
uint32_t g_audio_ucode_data_size = 0;
static uint32_t g_audio_boot_addr = 0;
static uint32_t g_audio_boot_size = 0;
// Snapshot of the last M_AUDTASK OSTask, for the runner to write into
// DMEM[0xFC0] (the synth reads data_ptr/data_size/output_buff from there).
static OSTask g_audio_task{};
static volatile uint32_t g_audtask_n = 0;  // M_AUDTASK submission count (production-rate diag)

static RspExitReason musyx_stub(uint8_t* rdram, uint32_t ucode_addr) {
    // Count audio-task submissions: proves the CPU MusyX sequencer is alive and
    // dispatching M_AUDTASK (only the RSP synth is stubbed) vs audio never driven.
    static int s_n = 0; ++s_n;
    if (s_n <= 8 || (s_n & 255) == 0) {
        fprintf(stderr, "[musyx-stub] M_AUDTASK #%d (sequencer is submitting audio tasks)\n", s_n);
        fflush(stderr);
    }
    // (2026-09-07) The "permanent sample bank" copy that lived here is gone: hardware never has the
    // bank in RDRAM (voices stream it from cartridge), so there is nothing to keep resident.
    // FIX (ROGUESQ_SONGTABLE_FIX, default on): the song-table at 0x80139A00 (16 entries x 8B,
    // key@+0) is uninit-to-ZERO at boot but the loader treats only 0xFFFFFFFF as empty — so a
    // songKey=0 load spuriously matches slot 0 → "already loaded" → the intro song never loads.
    // The game inits the table to 0xFFFFFFFF later (~task#150), too late for the intro. While the
    // table is still all-zero (uninit), pre-fill the empty slots to 0xFFFFFFFF so the intro load
    // sees real empties. No-ops once any slot is populated (game took over).
    { static int s_fix = -1;
      if (s_fix < 0) s_fix = env_on("ROGUESQ_SONGTABLE_FIX", true);
      if (s_fix) { const uint32_t T = 0x139A00u; bool allzero = true;
        for (int i = 0; i < 16; ++i) { uint32_t a = T + i*8;
          if (rdram_be32(rdram, a) != 0) { allzero = false; break; } }
        if (allzero) { for (int i = 0; i < 16; ++i) rdram_wr32(rdram, T + i*8, 0xFFFFFFFFu);
          static int once=0; if(!once){once=1; fprintf(stderr,"[songtable-fix] pre-filled empty song-table to 0xFFFFFFFF at task#%d\n",s_n); fflush(stderr);} } } }
    // ROGUESQ_DUMP_AUDIO_UCODE=1: dump the MusyX ucode TEXT (0x1000B from
    // ucode_addr) + DATA to files for an offline RSPRecomp pass. Once only.
    static int s_dump_en = -1;
    if (s_dump_en < 0) s_dump_en = env_on("ROGUESQ_DUMP_AUDIO_UCODE");
    // Check voice-struct population at several time points (boot → ~25s) to see
    // if the MusyX sample/voice data EVER loads into RDRAM. s_n counts M_AUDTASK
    // (~60/s). At each checkpoint: follow cmdlist voice pointers (+0x08, stride
    // 0x4C) and report whether each voice struct is non-zero (= sample loaded).
    if (s_dump_en && (s_n == 1 || s_n % 150 == 0)) {
        if (g_audio_task.t.data_ptr) {
            uint32_t cl = (uint32_t)g_audio_task.t.data_ptr & 0x00FFFFFFu;
            for (int v = 0; v < 3; ++v) {
                uint32_t e = cl + v * 0x4C + 0x08;
                uint32_t vp = rdram_be32(rdram, e);
                int nz = 0; uint32_t vphys = vp & 0x00FFFFFFu;
                if (vphys) for (uint32_t k = 0; k < 0x80; ++k) if (vphys+k < 0x800000u && rdram[(vphys+k)^3]) ++nz;
                fprintf(stderr, "[voice-check task#%d] voice%d ptr=0x%08X nonzero_bytes=%d/128\n", s_n, v, vp, nz);
            }
            fflush(stderr);
        }
        // samp-buffer content check: is the loaded sample bank actually filled in RAM?
        // (samp_SND.bin starts FC 97 FF 63). Tells us if silence is a missing-samp vs
        // a voice-wiring/volume problem.
        {
            static const uint8_t sig[4] = { 0xFC,0x97,0xFF,0x63 };
            uint32_t at = 0; int nz = 0;
            for (uint32_t o = 0; o + 4 <= 0x800000u; ++o) {
                if (rdram[o^3]==sig[0] && rdram[(o+1)^3]==sig[1] && rdram[(o+2)^3]==sig[2] && rdram[(o+3)^3]==sig[3]) { at = o; break; }
            }
            if (at) for (uint32_t k = 0; k < 0x1000; ++k) if (rdram[(at+k)^3]) ++nz;
            fprintf(stderr, "[samp-content task#%d] sig@0x80%06X nonzero/4096=%d\n", s_n, at, nz); fflush(stderr);
        }
        // Song-table state (0x80139A00, ≤16 entries × 8B). Empty slots SHOULD be
        // -1 (0xFFFFFFFF); if they're 0, loadSongAssetByName(songKey=0) spuriously
        // matches an empty slot → early-returns "already loaded" → intro song
        // never loads. Log first 6 entries' key field (+0).
        {
            uint32_t st = 0x139A00u; char buf[160]; int o = 0;
            for (int i = 0; i < 6; ++i) {
                uint32_t a = st + i * 8;
                uint32_t k = rdram_be32(rdram, a);
                o += snprintf(buf+o, sizeof(buf)-o, "%s0x%X", i?",":"", k);
            }
            fprintf(stderr, "[song-table task#%d] keys[0..5]=%s\n", s_n, buf); fflush(stderr);
        }
        // Song REGISTRY (loadSongAssetByName 3rd loop, base 0x8009FCD4, 8B entries
        // {key,param}). This is what songKey is looked up in to build the asset
        // filename. Log first 10 entries' key+param → is it populated? with what keys?
        {
            uint32_t rg = 0x09FCD4u - 8*8;  // start 8 entries earlier to catch keys 0-5
            for (int i = 0; i < 22; ++i) {
                uint32_t a = rg + i * 8;
                uint32_t k = rdram_be32(rdram, a);
                uint32_t p = rdram_be32(rdram, a+4);
                char nm[20]={0}; uint32_t pp=p&0x00FFFFFFu; if(pp&&pp<0x800000u){ for(int j=0;j<19;j++){uint8_t b=rdram[(pp+j)^3]; if(!b)break; nm[j]=(b>=32&&b<127)?b:'?';} }
                fprintf(stderr, "[song-reg task#%d] key=0x%X name='%s'\n", s_n, k, nm);
            }
            fflush(stderr);
        }
    }
    return RspExitReason::Broke;
}

// Audio-ucode runner (ROGUESQ_AUDIO_UCODE=1). Mirrors factor5_gfx_runner:
// mimic SP_BOOT — zero DMEM + DMA the ucode_data section to DMEM[0] — then run
// the RSPRecomp'd MusyX synth. The synthesized PCM flows out through the game's
// own osAiSetNextBuffer → runtime AI emulation → queue_samples → SDL, so no
// manual output capture is needed. WORK IN PROGRESS: DMEM/boot/command-list
// setup is not yet verified correct; gated off by default so normal runs keep
// the silent musyx_stub. See project_audio_state_2026_05_24.md step 3.
// (dma_rdram_to_dmem comes from librecomp/rsp.hpp, already included.)
static RspExitReason musyx_audio_runner(uint8_t* rdram, uint32_t ucode_addr) {
    // ROGUESQ_LOG_AUDIO_OUT=1: one-time scan of RDRAM for the .samp waveform signature
    // (first 8 bytes of samp_SND.bin: FC 97 FF 63 01 58 00 81). Tells us if the sample
    // bank is loaded in RAM and at what base (= .samp_base for voice sample ptrs).
    {
        static int s_en = -1, s_found = 0, s_n = 0;
        if (s_en < 0) s_en = env_on("ROGUESQ_LOG_AUDIO_OUT");
        if (s_en && !s_found && ((++s_n) <= 1 || (s_n % 256) == 0)) {
            static const uint8_t sig[8] = { 0xFC,0x97,0xFF,0x63,0x01,0x58,0x00,0x81 };
            int hitsS = 0, hitsR = 0; uint32_t firstS = 0, firstR = 0;
            for (uint32_t o = 0; o + 8 <= 0x800000u; ++o) {
                bool ms = true, mr = true;
                for (int k = 0; k < 8; ++k) {
                    if (rdram[(o + k) ^ 3] != sig[k]) ms = false;
                    if (rdram[o + k] != sig[k]) mr = false;
                    if (!ms && !mr) break;
                }
                if (ms) { if (!hitsS) firstS = o; ++hitsS; }
                if (mr) { if (!hitsR) firstR = o; ++hitsR; }
                if (hitsS >= 2 && hitsR >= 2) break;
            }
            fprintf(stderr, "[samp-scan #%d] swizzled hits=%d@0x%08X  raw hits=%d@0x%08X (%s)\n",
                s_n, hitsS, hitsS ? (0x80000000u | firstS) : 0u, hitsR, hitsR ? (0x80000000u | firstR) : 0u,
                (hitsS || hitsR) ? "<-LOADED" : "not in RAM"); fflush(stderr);
            if (hitsS || hitsR) s_found = 1;
        }
    }
    std::memset(dmem, 0, 0xFC0);
    uint32_t ud  = g_audio_ucode_data_addr & 0x00FFFFFFu;
    uint32_t uds = g_audio_ucode_data_size;
    if (ud && uds && uds <= 0xFC0) {
        dma_rdram_to_dmem(rdram, /*dmem*/0, /*dram*/ud, /*rd_len*/uds - 1);
    }
    // Write the OSTask to DMEM[0xFC0..0x1000] big-endian (i^3 swizzle). The synth
    // reads data_ptr @+0x30, data_size @+0x34, output_buff @+0x28 from r1=0xFC0.
    {
        const OSTask* t = &g_audio_task;
        auto poke = [](uint32_t off, uint32_t val) {
            for (int j = 0; j < 4; ++j) dmem[(off + j) ^ 3] = (uint8_t)(val >> (24 - j * 8));
        };
        poke(0xFC0 + 0x00, (uint32_t)t->t.type);          poke(0xFC0 + 0x04, (uint32_t)t->t.flags);
        poke(0xFC0 + 0x08, (uint32_t)t->t.ucode_boot);     poke(0xFC0 + 0x0C, (uint32_t)t->t.ucode_boot_size);
        poke(0xFC0 + 0x10, (uint32_t)t->t.ucode);          poke(0xFC0 + 0x14, (uint32_t)t->t.ucode_size);
        poke(0xFC0 + 0x18, (uint32_t)t->t.ucode_data);     poke(0xFC0 + 0x1C, (uint32_t)t->t.ucode_data_size);
        poke(0xFC0 + 0x20, (uint32_t)t->t.dram_stack);     poke(0xFC0 + 0x24, (uint32_t)t->t.dram_stack_size);
        poke(0xFC0 + 0x28, (uint32_t)t->t.output_buff);    poke(0xFC0 + 0x2C, (uint32_t)t->t.output_buff_size);
        poke(0xFC0 + 0x30, (uint32_t)t->t.data_ptr);       poke(0xFC0 + 0x34, (uint32_t)t->t.data_size);
        poke(0xFC0 + 0x38, (uint32_t)t->t.yield_data_ptr); poke(0xFC0 + 0x3C, (uint32_t)t->t.yield_data_size);
    }
    static int s_n = 0; ++s_n;
    // Compare the LIVE synth ucode (RAM at OSTask.ucode) vs what the recomp decoded (ROM 0x9ABE0).
    // IMEM 0x3E8 (jal target of the header DMA) is nop+COP2 in ROM; if live RAM shows mtc0/DMA here
    // the recomp decoded the WRONG source (toml text_offset stale) — that's the synth-silence root.
    if (recomp::os::getenv("ROGUESQ_LOG_UCODESRC") && s_n <= 2) {
        uint32_t uc = (uint32_t)g_audio_task.t.ucode & 0x00FFFFFFu;
        auto dump = [&](uint32_t off){ char b[80]; int o=0;
            for(int k=0;k<16;k++) o+=snprintf(b+o,sizeof(b)-o,"%s%02X",(k%4==0)?" ":"",rdram[(uc+off+k)^3]);
            fprintf(stderr,"[ucodesrc] live RAM ucode(0x%06X)+0x%X:%s\n", uc, off, b); };
        dump(0x3E8);  // ROM = 00000000 4B271094 C9231800 4B070850 (nop+vector)
        dump(0x3B0);  // ROM = 40850800 03E00008 40871000 400A3000 (mtc0 DMA)
        fflush(stderr);
    }
    // Probe the command list the synth will process — does it actually contain voice commands?
    {
        static int s_lo = -1;
        if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");
        if (s_lo && (s_n <= 8 || (s_n % 128) == 0)) {
            uint32_t dp = (uint32_t)g_audio_task.t.data_ptr & 0x00FFFFFFu; uint32_t ds = (uint32_t)g_audio_task.t.data_size;
            char hx[260]; int o = 0;
            { uint32_t ca=0x149700; unsigned vcount=((unsigned)rdram[ca^3]<<8)|rdram[(ca+1)^3]; uint32_t cb=0x149702; unsigned bcnt=rdram[cb^3];
              fprintf(stderr,"[voicetable #%d] synthVoiceCount@0x80149700=%u  byteCnt@0x80149702=%u\n", s_n, vcount, bcnt); }
            // Descriptor-buffer ptrs at 0x80149708 (-0x68F8, double-buffered by activeCount). musyxMixActiveVoices
            // writes the DSP voice descriptors here. Compare to the command's word[2] to detect a buffer desync.
            { auto rdw=[&](uint32_t a){return rdram_be32(rdram, a);};
              for(int b=0;b<2;b++){ uint32_t bp=rdw(0x149708+b*4); uint32_t bpp=bp&0x00FFFFFFu; int nz=0,nz100=0; char h0[80],h1[80]; int o0=0,o1=0;
                if(bpp && bpp+0x300<=0x800000u){ for(uint32_t k=0;k<0x300;k++) if(rdram[(bpp+k)^3]) ++nz;
                  for(uint32_t k=0x100;k<0x130;k++) if(rdram[(bpp+k)^3]) ++nz100;
                  for(int k=0;k<0x18;k++) o0+=snprintf(h0+o0,sizeof(h0)-o0,"%s%02X",(k%4==0)?" ":"",rdram[(bpp+k)^3]);
                  for(int k=0;k<0x18;k++) o1+=snprintf(h1+o1,sizeof(h1)-o1,"%s%02X",(k%4==0)?" ":"",rdram[(bpp+0x100+k)^3]); }
                fprintf(stderr,"[descbuf #%d] [%d]=0x%08X nz=%d @+0x0:%s | nz@+0x100=%d @+0x100:%s\n", s_n, b, bp, nz, h0, nz100, h1); }
              // outBuf array @0x80149758 (-0x68A8), outBuf voice count @0x80149760 (-0x68A0, halfword),
              // and the per-descBuf active-flag (+0x22) count. word2 should == one of the outBufs.
              { uint32_t ob0=rdw(0x149758), ob1=rdw(0x14975C);
                unsigned obc=((unsigned)rdram[0x149760^3]<<8)|rdram[0x149761^3];
                int af0=0,af1=0;
                for(int b=0;b<2;b++){ uint32_t bp=rdw(0x149708+b*4)&0x00FFFFFFu; int*af=b?&af1:&af0;
                  if(bp&&bp+20*0x88<=0x800000u) for(int s=0;s<20;s++) if(rdram[(bp+s*0x88+0x22)^3]) ++(*af); }
                fprintf(stderr,"[outbuf #%d] outBuf[0]=0x%08X [1]=0x%08X voiceCount=%u | activeFlag(+0x22) descBuf0=%d descBuf1=%d\n",
                  s_n, ob0, ob1, obc, af0, af1);
                // control-struct globals: word3 of the header should = -0x6898 (0x149768)
                fprintf(stderr,"[ctrl #%d] -0x6898=0x%08X -0x6894=0x%08X -0x6890=0x%08X -0x689C=0x%08X\n",
                  s_n, rdw(0x149768), rdw(0x14976C), rdw(0x149770), rdw(0x149764)); }
              // also dump the command's referenced buffer (word[2]) for comparison
              { uint32_t dp=(uint32_t)g_audio_task.t.data_ptr&0x00FFFFFFu;
                uint32_t w2=(dp&&dp<0x800000u)?rdw(dp+8):0; uint32_t w2p=w2&0x00FFFFFFu; char hb[160]; int ho=0;
                if(w2p&&w2p+0x30<=0x800000u) for(int k=0;k<0x30;k++) ho+=snprintf(hb+ho,sizeof(hb)-ho,"%s%02X",(k%4==0)?" ":"",rdram[(w2p+k)^3]);
                fprintf(stderr,"[cmdw2 #%d] word[2]=0x%08X:%s\n", s_n, w2, hb);
                // is the CPU-written voice data at data_ptr+0x100 (where synth would read if word2==data_ptr)?
                char hd[160]; int hk=0; int nzdp=0;
                if(dp&&dp+0x300<=0x800000u){ for(uint32_t k=0x100;k<0x2A0;k++) if(rdram[(dp+k)^3]) ++nzdp;
                  for(int k=0;k<0x30;k++) hk+=snprintf(hd+hk,sizeof(hd)-hk,"%s%02X",(k%4==0)?" ":"",rdram[(dp+0x100+k)^3]); }
                fprintf(stderr,"[dpdata #%d] data_ptr=0x%08X +0x100 nz(0x1A0)=%d:%s\n", s_n, (uint32_t)g_audio_task.t.data_ptr, nzdp, hd); } }
            o += snprintf(hx + o, sizeof(hx) - o, "[cmdlist #%d] data_ptr=0x%08X size=0x%X:", s_n, (uint32_t)g_audio_task.t.data_ptr, ds);
            if (dp && dp < 0x800000u) for (int k = 0; k < 16; k++) o += snprintf(hx + o, sizeof(hx) - o, "%s%02X", (k % 4 == 0) ? " " : "", rdram[(dp + k) ^ 3]);
            // also dump the voice block the command points to (data_ptr+0x08) — is it ever populated?
            uint32_t vp = dp && dp < 0x800000u ? rdram_be32(rdram, dp+8) : 0;
            uint32_t vpp = vp & 0x00FFFFFFu; int vnz = 0, vnz2 = 0;
            if (vpp && vpp < 0x800000u) {
                for (uint32_t k = 0; k < 0x100; k++) if (rdram[(vpp + k) ^ 3]) ++vnz;            // base..+0x100
                uint32_t vp2 = vpp + 0x100;                                                       // where the synth actually reads (r5+0x100)
                if (vp2 + 0x198 <= 0x800000u) for (uint32_t k = 0; k < 0x198; k++) if (rdram[(vp2 + k) ^ 3]) ++vnz2;
                o += snprintf(hx + o, sizeof(hx) - o, " | voiceHdr@0x%08X nz=%d  voiceData@+0x100 nz=%d:", vp, vnz, vnz2);
                for (int k = 0; k < 24; k++) o += snprintf(hx + o, sizeof(hx) - o, "%s%02X", (k % 4 == 0) ? " " : "", rdram[(vp2 + k) ^ 3]);
            }
            fprintf(stderr, "%s\n", hx); fflush(stderr);
        }
    }
    // EXPERIMENT (ROGUESQ_BRIDGE_WORD2): the CPU pipeline writes the synth voice data to
    // data_ptr+0x100, but sets word2 (data_ptr+0x8) to the separate, unfilled -0x689C buffer.
    // Point word2 back at data_ptr so the synth reads the real voice data it wrote.
    if (recomp::os::getenv("ROGUESQ_BRIDGE_WORD2")) {
        uint32_t dp = (uint32_t)g_audio_task.t.data_ptr;
        uint32_t dpp = dp & 0x00FFFFFFu;
        if (dpp && dpp + 0xC <= 0x800000u)
            rdram_wr32(rdram, dpp + 8, dp);
    }
    // Run the shared Factor5 boot ucode first — it's the SAME ucode at 0x800825D0
    // that the GFX task uses (factor5_boot_rsp.toml), and it initializes the DMEM
    // state the synth depends on (beyond just the ucode_data DMA). Reading the
    // audio OSTask from DMEM[0xFC0], it DMAs the audio ucode_data → DMEM 0. Boot
    // exits via UnhandledJumpTarget/Broke on its `jr 0x1080` handoff = expected.
    factor5_boot(rdram, ucode_addr);
    auto t1 = std::chrono::steady_clock::now();
    // ROGUESQ_DUMP_SYNTH_FRAME=<n>: capture the EXACT synth input (post-boot RDRAM + OSTask) at the
    // n-th M_AUDTASK so tools/musyx_replay can run the synth offline on a POPULATED command list.
    // Writes <PATH>.bin (plain big-endian image, file[i]=rdram[i^3]) + <PATH>.task (OSTask, 0x40 BE bytes).
    {
        static int s_tgt = -2;
        if (s_tgt == -2) { const char* e = recomp::os::getenv("ROGUESQ_DUMP_SYNTH_FRAME"); s_tgt = e ? atoi(e) : -1; }
        if (s_tgt >= 0 && s_n == s_tgt) {
            const char* base = recomp::os::getenv("ROGUESQ_DUMP_SYNTH_PATH");
            if (!base || !base[0]) base = "dumps/synth_frame";
            char pb[512]; snprintf(pb, sizeof(pb), "%s.bin", base);
            FILE* f = recomp::os::fopen(pb, "wb");
            if (f) { std::vector<uint8_t> img(0x800000); for (uint32_t i = 0; i < 0x800000u; ++i) img[i] = rdram[i ^ 3];
                     fwrite(img.data(), 1, img.size(), f); fclose(f); }
            char pt[512]; snprintf(pt, sizeof(pt), "%s.task", base);
            FILE* g = recomp::os::fopen(pt, "wb");
            if (g) { const OSTask* t = &g_audio_task; uint8_t tb[0x40]; std::memset(tb, 0, sizeof(tb));
                     auto put = [&](int off, uint32_t v){ tb[off]=v>>24; tb[off+1]=v>>16; tb[off+2]=v>>8; tb[off+3]=v; };
                     put(0x00,(uint32_t)t->t.type);        put(0x04,(uint32_t)t->t.flags);
                     put(0x10,(uint32_t)t->t.ucode);       put(0x14,(uint32_t)t->t.ucode_size);
                     put(0x18,(uint32_t)t->t.ucode_data);  put(0x1C,(uint32_t)t->t.ucode_data_size);
                     put(0x28,(uint32_t)t->t.output_buff); put(0x2C,(uint32_t)t->t.output_buff_size);
                     put(0x30,(uint32_t)t->t.data_ptr);    put(0x34,(uint32_t)t->t.data_size);
                     fwrite(tb, 1, sizeof(tb), g); fclose(g); }
            fprintf(stderr, "[synth-frame] dumped task#%d data_ptr=0x%08X size=0x%X -> %s(.bin/.task)\n",
                    s_n, (uint32_t)g_audio_task.t.data_ptr, (uint32_t)g_audio_task.t.data_size, base); fflush(stderr);
        }
    }
    RspExitReason r = musyx_audio(rdram, ucode_addr);
    auto t2 = std::chrono::steady_clock::now();
    // Did the synth produce ANY internal signal? Scan its DMEM work area (accumulation/voice
    // buffers live in DMEM 0x600..0xFC0). All-zero => synth bailed early (no active voice). This
    // isolates "ucode not processing voices" from "output not reaching RDRAM/AI".
    {
        static int s_dm = -1;
        if (s_dm < 0) s_dm = env_on("ROGUESQ_LOG_DMEM");
        if (s_dm && (s_n <= 8 || (s_n % 64) == 0)) {
            uint32_t nz = 0; int mx = 0;
            for (uint32_t i = 0x600; i < 0xFC0; ++i) { uint8_t b = dmem[i]; if (b) { ++nz; if (b > mx) mx = b; } }
            // also scan the voice block DMEM region 0xCB0..0xE48 specifically (where word2+0x100 was DMA'd)
            uint32_t nzv = 0; for (uint32_t i = 0xCB0; i < 0xE48; ++i) if (dmem[i]) ++nzv;
            fprintf(stderr, "[dmem #%d] work(0x600-0xFC0) nz=%u peak=%d | voiceblk(0xCB0-0xE48) nz=%u  exit=%d\n",
                s_n, nz, mx, nzv, (int)r); fflush(stderr);
        }
    }
    // Probe the synth's OWN output buffer right after synthesis: does it write ANY non-zero PCM?
    // Scans raw bytes so it's swizzle-independent (a non-zero check is order-independent). This
    // isolates "synth produces silence" from "output buffer never reaches SDL" (queue_samples).
    {
        static int s_lo = -1;
        if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");
        if (s_lo && (s_n <= 8 || (s_n % 128) == 0)) {
            uint32_t ob  = (uint32_t)g_audio_task.t.output_buff & 0x00FFFFFFu;
            uint32_t obs = (uint32_t)g_audio_task.t.output_buff_size;
            uint8_t mx = 0; uint32_t nz = 0;
            if (ob && obs && (uint64_t)ob + obs <= 0x800000u) {
                for (uint32_t i = 0; i < obs; ++i) { uint8_t b = rdram[ob + i]; if (b) { ++nz; if (b > mx) mx = b; } }
            }
            fprintf(stderr, "[synth-out #%d] outBuf=0x%08X size=0x%X peakByte=%d nonzero=%u %s\n",
                s_n, (uint32_t)g_audio_task.t.output_buff, obs, (int)mx, nz, nz ? "<-SYNTH HAS SIGNAL" : "(synth silent)"); fflush(stderr);
            // MusyX gets its work from the command list at data_ptr (NOT output_buff). RDRAM is stored
            // byte-swapped, so read words with the ^3 swizzle to get true N64 values. word[2] is a
            // pointer to the real audio data/output buffer; follow it and scan for non-zero PCM.
            auto rd_w = [&](uint32_t off) -> uint32_t {
                return rdram_be32(rdram, off);
            };
            uint32_t dp = (uint32_t)g_audio_task.t.data_ptr & 0x00FFFFFFu;
            uint32_t dsz = (uint32_t)g_audio_task.t.data_size;
            if (dp && dp + 0x40 <= 0x800000u) {
                char line[256]; int o = 0;
                o += snprintf(line + o, sizeof(line) - o, "[synth-cmd #%d] data_ptr=0x%08X size=0x%X words:",
                    s_n, (uint32_t)g_audio_task.t.data_ptr, dsz);
                for (int w = 0; w < 8; ++w) o += snprintf(line + o, sizeof(line) - o, " %08X", rd_w(dp + w * 4));
                fprintf(stderr, "%s\n", line); fflush(stderr);
                // Follow word[2] as a pointer and scan its target for non-zero audio data.
                uint32_t ptr = rd_w(dp + 8) & 0x00FFFFFFu;
                if (ptr && ptr + 0x100 <= 0x800000u) {
                    uint8_t mx = 0; uint32_t nz = 0;
                    for (uint32_t i = 0; i < 0x100; ++i) { uint8_t b = rdram[ptr + i]; if (b) { ++nz; if (b > mx) mx = b; } }
                    fprintf(stderr, "[synth-ptr #%d] target=0x%08X peakByte=%d nonzero=%u/256 %s\n",
                        s_n, rd_w(dp + 8), (int)mx, nz, nz ? "<-DATA PRESENT" : "(empty)"); fflush(stderr);
                }
            }
        }
    }
    {
        double aud_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        static double s_sum = 0, s_max = 0; static int s_cnt = 0;
        s_sum += aud_ms; ++s_cnt; if (aud_ms > s_max) s_max = aud_ms;
        // Gap between synth ticks (retrace-thread starvation shows as gaps far above one VI).
        static auto s_last = t1; static double s_gap_max = 0; static int s_gap_over = 0;
        { const double gap = std::chrono::duration<double, std::milli>(t1 - s_last).count(); s_last = t1; if (gap > s_gap_max) s_gap_max = gap; if (gap > 40.0) ++s_gap_over; }
        if (s_n <= 6 || (s_n % 64) == 0) {
            fprintf(stderr, "[musyx-run #%d] audio=%.1fms avg=%.1fms max=%.1fms (n=%d) tick-gap max=%.0fms over40=%d underruns=%u\n",
                s_n, aud_ms, s_sum / s_cnt, s_max, s_cnt, s_gap_max, s_gap_over, g_rs64_audio_underruns); fflush(stderr);
            s_gap_max = 0;
        }
    }
    // Treat UnhandledJumpTarget/Broke as task-complete so sp_complete fires and
    // the audio thread keeps running (same contract as the GFX runner).
    return RspExitReason::Broke;
}

// Catch-all stub for unrecognised task types. Returning Broke (instead of
// QUICK_EXIT'ing) avoids tearing down the process when a stale/uninitialised
// task struct gets dispatched — observed early in boot with type fields like
// 0x21E50ADF that don't match M_GFXTASK/M_AUDTASK.
static RspExitReason unknown_task_stub(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

// Factor 5 GFX ucode runner (LLE side of the hybrid pipeline).
//
// Runs the boot ucode to set up DMEM + DMA the data section, emulates
// L_112C's first DL fetch by hand (the original ucode normally calls L_112C
// from inside the dispatch loop, but on first invocation that hasn't happened
// yet so DMEM has no real DL bytes), then runs the main ucode. The main ucode
// processes DL commands and, for vertex-pipeline ops, emits raw RDP triangle
// bytes via mtc0 DPC_END writes that flow through src/rsp/dpc_bridge.cpp into
// RT64 (isHLE=false).
static RspExitReason factor5_gfx_runner(uint8_t* rdram, uint32_t ucode_addr) {
    uint32_t dl_ptr           = s_pending_task_data_ptr;
    uint32_t ucode_data_addr  = s_pending_task_ucode_data;
    uint32_t ucode_data_size  = s_pending_task_ucode_data_size;

    // Mimic SP_BOOT (silicon-level RSP boot ucode): zero DMEM[0..0xFC0] then
    // populate DMEM[0..ucode_data_size]. Leave DMEM[0xFC0..0x1000] alone —
    // that's the OSTask region the boot ucode reads.
    //
    // Snapshot/restore: the first task to use a given ucode_data address gets
    // a fresh DMA from RDRAM. Subsequent tasks restore from the cached
    // snapshot taken AT THAT FIRST DMA. RDRAM at ucode_data is modified
    // mid-flight by some part of the engine (gfx CPU code or background DMA),
    // which is why task #N>1 was producing garbage DMEM[0..0x10] from the
    // same source addr and hitting UnhandledJumpTarget 0xFF7E. Replaying
    // from the snapshot bypasses the corruption.
    static thread_local std::vector<uint8_t> s_udata_snap;
    static thread_local uint32_t s_udata_snap_addr = 0;
    static thread_local uint32_t s_udata_snap_size = 0;
    std::memset(dmem, 0, 0xFC0);
    if (ucode_data_addr != 0 && ucode_data_size != 0 && ucode_data_size <= 0xFC0) {
        const bool snap_match = (s_udata_snap_addr == ucode_data_addr &&
                                  s_udata_snap_size == ucode_data_size &&
                                  s_udata_snap.size() == ucode_data_size);
        if (snap_match) {
            std::memcpy(dmem, s_udata_snap.data(), ucode_data_size);
        } else {
            dma_rdram_to_dmem(rdram, /*dmem*/0, /*dram*/ucode_data_addr & 0x00FFFFFF,
                              /*rd_len*/ucode_data_size - 1);
            s_udata_snap_addr = ucode_data_addr;
            s_udata_snap_size = ucode_data_size;
            s_udata_snap.assign(dmem, dmem + ucode_data_size);
        }
    }

    // Boot exits via UnhandledJumpTarget on its `jr $7=0x1080` (jumping into
    // the main ucode it just DMA'd to IMEM 0x80) — that's expected.
    static thread_local int s_runner_step_log = 0;
    const bool log_step = (++s_runner_step_log) <= 16;
    if (log_step) {
        fprintf(stderr, "[runner-step #%d] entering factor5_boot ucode_addr=0x%08X\n",
                s_runner_step_log, ucode_addr);
        fflush(stderr);
    }
    RspExitReason boot_r = factor5_boot(rdram, ucode_addr);
    if (log_step) {
        fprintf(stderr, "[runner-step #%d] factor5_boot returned %d\n",
                s_runner_step_log, (int)boot_r);
        fflush(stderr);
    }
    if (boot_r != RspExitReason::UnhandledJumpTarget && boot_r != RspExitReason::Broke) {
        fprintf(stderr, "[RSP] factor5_boot returned unexpected %d, abandoning task\n", (int)boot_r);
        return RspExitReason::Broke;
    }

    auto poke_be32 = [](uint32_t off, uint32_t val) {
        for (int i = 0; i < 4; ++i) {
            dmem[(off + i) ^ 3] = (uint8_t)(val >> (24 - 8*i));
        }
    };
    if (dl_ptr) {
        // Stage the first 0x110 bytes of DL into DMEM at 0x170 (where the
        // main ucode's L_112C helper would normally DMA). Set DMEM[0x654] to
        // 0x178 so the dispatcher's first `lw $17, 0x654` lands past the
        // 8-byte header at the start of real commands.
        dma_rdram_to_dmem(rdram, /*dmem*/0x170, /*dram*/dl_ptr & 0x00FFFFFF, /*rd_len*/0x10F);
        // DMEM[$18+0x30] = the "current chunk RDRAM addr". With $18=0x100
        // (the value our fixup injects, matching L_1DB0's bootstrap), this
        // is DMEM[0x130]. L_11B0's chunk-fetch reads this slot for the next
        // re-DMA. L_1DB0 normally bootstraps it from DMEM[$1+0x30] = 0xFF0;
        // we mirror that bootstrap here in case L_1DB0 itself doesn't fire
        // every task. (Without this, tasks 2+ exit before emitting any RDP
        // because the static-data segment leaves 0x130 as bogus.)
        poke_be32(0x130, dl_ptr);
        poke_be32(0xFF0, dl_ptr);
        poke_be32(0x101C, dl_ptr);
        poke_be32(0x654,  0x178);
        // Reset the DL-stack-pointer byte. The ucode runs with $18 = 0x100
        // (set by L_1DB0's bootstrap), so $18+0x52 = DMEM[0x152]. Op_0F's
        // L_12C4 handler decrements this by 8 each call and exits the
        // dispatch loop when it goes negative. After task #1 underflows the
        // byte is left negative; subsequent tasks would exit immediately with
        // 0 RDP work emitted. Reset to 0x18 (3 stack entries) at task start.
        dmem[0x152 ^ 3] = 0x18;
        dmem[0x153 ^ 3] = 0;     // L_1DB0 also clears 0x53($18)
    } else {
        poke_be32(0x654, 0x270);
    }
    if (log_step) {
        fprintf(stderr, "[runner-step #%d] entering factor5_ucode\n", s_runner_step_log);
        fflush(stderr);
    }
    RspExitReason r = factor5_ucode(rdram, ucode_addr);
    if (log_step) {
        fprintf(stderr, "[runner-step #%d] factor5_ucode returned %d\n",
                s_runner_step_log, (int)r);
        fflush(stderr);
    }
    return r;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_GFXTASK:
        // Cache OSTask fields for factor5_gfx_runner. The runner needs to:
        //   - DMA the ucode_data segment to DMEM (mimicking SP_BOOT) so each
        //     task starts with fresh per-task data, not state carried over
        //     from the previous task's exit.
        //   - DMA the first chunk of the DL into DMEM[0x170] so the first
        //     dispatch loop iter has real bytes.
        s_pending_task_data_ptr        = (uint32_t)task->t.data_ptr;
        s_pending_task_ucode_data      = (uint32_t)task->t.ucode_data;
        s_pending_task_ucode_data_size = (uint32_t)task->t.ucode_data_size;
        return &factor5_gfx_runner;
    case M_AUDTASK:
        // Capture the MusyX audio ucode's OSTask fields once — these give the
        // text/data RDRAM addresses + sizes that RSPRecomp needs (see
        // project_audio_state). ucode = IMEM text source, ucode_data = DMEM data.
        {
            static int s_logged = 0;
            if (s_logged < 4) { ++s_logged;
                fprintf(stderr, "[audio-ucode] ucode=0x%08X size=0x%X  ucode_data=0x%08X data_size=0x%X  boot=0x%08X boot_size=0x%X  data_ptr=0x%08X data_size=0x%X\n",
                    (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_size,
                    (uint32_t)task->t.ucode_data, (uint32_t)task->t.ucode_data_size,
                    (uint32_t)task->t.ucode_boot, (uint32_t)task->t.ucode_boot_size,
                    (uint32_t)task->t.data_ptr, (uint32_t)task->t.data_size);
                fflush(stderr);
            }
            g_audio_ucode_data_addr = (uint32_t)task->t.ucode_data;
            g_audio_ucode_data_size = (uint32_t)task->t.ucode_data_size;
            g_audio_boot_addr = (uint32_t)task->t.ucode_boot;
            g_audio_boot_size = (uint32_t)task->t.ucode_boot_size;
            g_audio_task = *task;
        }
        {
            ++g_audtask_n;
            // Runs the RSPRecomp'd MusyX synth (now functional after the text_address=0x1080
            // fix). DEFAULT ON; opt out with ROGUESQ_NO_AUDIO_UCODE=1 to fall back to musyx_stub.
            static int s_au = -1;
            if (s_au < 0) s_au = env_on("ROGUESQ_NO_AUDIO_UCODE") ? 0 : 1;
            if (s_au) return &musyx_audio_runner;
        }
        return &musyx_stub;
    default:
        // Don't crash — log once per distinct type and stub it out.
        static thread_local uint32_t last_unknown = 0;
        if (task->t.type != last_unknown) {
            last_unknown = task->t.type;
            fprintf(stderr, "[RSP] Stubbing unknown task type: %" PRIu32 " (0x%08X)\n",
                task->t.type, task->t.type);
            fflush(stderr);
        }
        return &unknown_task_stub;
    }
}

// ---------------------------------------------------------------------------
// Audio (SDL2)
// ---------------------------------------------------------------------------
static SDL_AudioDeviceID audio_device = 0;
static uint32_t audio_sample_rate = 48000;

static void set_frequency(uint32_t freq) {
    // Don't churn the device when the rate hasn't changed — each close/reopen
    // flushes the SDL queue and clicks. The game re-sets the same AI rate
    // repeatedly (observed 22050 set 3x), so this removes those pops.
    if (audio_device && freq == audio_sample_rate) {
        return;
    }
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
    audio_sample_rate = freq;

    SDL_AudioSpec desired{};
    desired.freq     = (int)freq;
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples  = 1024;

    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if (!audio_device) {
        fprintf(stderr, "[Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(audio_device, 0);
    fprintf(stderr, "[Audio] device OPENED freq=%u (output path live)\n", freq); fflush(stderr);
}

static void queue_samples(int16_t* samples, size_t num_samples) {
    // ultramodern's queue_samples contract passes the int16 SAMPLE count (= AI
    // byte_count / 2), NOT a byte count. This function's body is written in bytes,
    // so convert once here. Previously the sample count was used directly as bytes,
    // so only HALF of every buffer reached SDL (the game submits 0x300=768-byte
    // buffers; SDL was getting 384) — the dominant cause of the underrun/crackle.
    const size_t num_bytes = num_samples * sizeof(int16_t);
    if (audio_device) {
        // Underrun gauge: the device queue was already dry when this buffer arrived (audible gap).
        { static int warm = 0;
          if (++warm > 64 && SDL_GetQueuedAudioSize(audio_device) == 0) ++g_rs64_audio_underruns; }
        // Diagnostic (ROGUESQ_LOG_AUDIO_OUT=1): confirm PCM flows + silence vs content.
        static int s_lo = -1;
        if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");
        static int s_q = 0; ++s_q;
        if (s_lo && (s_q <= 8 || (s_q & 255) == 0)) {
            size_t n = num_bytes / 2; int16_t mx = 0;
            for (size_t i = 0; i < n; ++i) { int16_t v = samples[i]; if (v < 0) v = -v; if (v > mx) mx = v; }
            fprintf(stderr, "[queue_samples #%d] %zu bytes, peak_amp=%d %s queued=%u bytes (%.1f ms)\n", s_q, num_bytes, (int)mx, mx ? "<-AUDIBLE" : "(silence)", (unsigned)SDL_GetQueuedAudioSize(audio_device), audio_sample_rate ? SDL_GetQueuedAudioSize(audio_device) / 4.0 * 1000.0 / audio_sample_rate : 0.0);
            fflush(stderr);
        }
        // Production-rate gauge: frames/sec actually produced vs the device rate. <100%
        // means the synth underproduces and any buffer eventually bleeds dry (the cut-out).
        // M_AUDTASK/s shows whether it's a cadence deficit (few tasks) or per-task size.
        if (s_lo) {
            static uint64_t s_tf = 0; static bool s_init = false; static std::chrono::steady_clock::time_point s_t0;
            auto nowt = std::chrono::steady_clock::now();
            if (!s_init) { s_init = true; s_t0 = nowt; }
            s_tf += num_bytes / 4;
            // Step-0 cadence probe (ROGUESQ_LOG_AUDIO_OUT): is the 89% a SUSTAINED slow cadence or JITTER
            // with dropouts? Bucket the inter-call gap of queue_samples (= buffer-production cadence) and
            // track per-call buffer frames. A tight ~1/60s cluster = sustained-slow; a bimodal spread with
            // a >25ms tail = jitter/starvation. Also count calls/window to get effective production Hz.
            static std::chrono::steady_clock::time_point s_last{}; static bool s_lhave = false;
            static uint32_t s_gh[7] = {0}; static uint32_t s_calls = 0; static uint32_t s_fmin = ~0u, s_fmax = 0; static uint64_t s_fsum = 0;
            if (s_lhave) {
                double gms = std::chrono::duration<double, std::milli>(nowt - s_last).count();
                int b = gms < 5 ? 0 : gms < 10 ? 1 : gms < 15 ? 2 : gms < 20 ? 3 : gms < 25 ? 4 : gms < 40 ? 5 : 6;
                s_gh[b]++;
            }
            s_last = nowt; s_lhave = true;
            { uint32_t f = num_bytes / 4; s_calls++; s_fsum += f; if (f < s_fmin) s_fmin = f; if (f > s_fmax) s_fmax = f; }
            double el = std::chrono::duration<double>(nowt - s_t0).count();
            if (el >= 2.0) {
                fprintf(stderr, "[audio-rate] %.0f frames/s (target %u, %.0f%%) | %.1f M_AUDTASK/s | calls=%.1f/s bufFrames min/avg/max=%u/%llu/%u\n",
                    s_tf / el, audio_sample_rate, 100.0 * (s_tf / el) / (audio_sample_rate ? audio_sample_rate : 1), g_audtask_n / el,
                    s_calls / el, s_fmin==~0u?0:s_fmin, (unsigned long long)(s_calls?s_fsum/s_calls:0), s_fmax);
                fprintf(stderr, "[audio-gap] queue_samples inter-call ms buckets <5|5-10|10-15|15-20|20-25|25-40|>40 = %u %u %u %u %u %u %u\n",
                    s_gh[0], s_gh[1], s_gh[2], s_gh[3], s_gh[4], s_gh[5], s_gh[6]);
                fflush(stderr);
                s_t0 = nowt; s_tf = 0; g_audtask_n = 0;
                for (int i=0;i<7;i++) s_gh[i]=0; s_calls=0; s_fmin=~0u; s_fmax=0; s_fsum=0;
            }
        }
        // Optional PCM->WAV capture for offline music encoding (ROGUESQ_DUMP_PCM=path or 1).
        // Header sizes are patched each buffer so the file stays valid if the run is killed.
        static FILE* s_wav = nullptr; static int s_wav_init = -1; static uint32_t s_wav_bytes = 0;
        if (s_wav_init < 0) {
            const char* e = recomp::os::getenv("ROGUESQ_DUMP_PCM");
            s_wav_init = (e && e[0] && e[0] != '0') ? 1 : 0;
            if (s_wav_init) {
                const char* path = (e[0] == '1' && e[1] == '\0') ? "dumps/wav/capture.wav" : e;
                s_wav = recomp::os::fopen(path, "wb");
                fprintf(stderr, "[pcm-dump] init path=%s fopen=%p\n", path, (void*)s_wav); fflush(stderr);
                if (s_wav) {
                    uint16_t ch = 2, bps = 16; uint32_t sr = audio_sample_rate;
                    uint32_t br = sr * ch * bps / 8; uint16_t ba = ch * bps / 8; uint32_t fmtlen = 16; uint16_t fmt = 1;
                    uint8_t hdr[44] = {0};
                    memcpy(hdr, "RIFF", 4); memcpy(hdr + 8, "WAVE", 4); memcpy(hdr + 12, "fmt ", 4);
                    memcpy(hdr + 16, &fmtlen, 4); memcpy(hdr + 20, &fmt, 2); memcpy(hdr + 22, &ch, 2);
                    memcpy(hdr + 24, &sr, 4); memcpy(hdr + 28, &br, 4); memcpy(hdr + 32, &ba, 2); memcpy(hdr + 34, &bps, 2);
                    memcpy(hdr + 36, "data", 4);
                    fwrite(hdr, 1, 44, s_wav); fflush(s_wav);
                    fprintf(stderr, "[pcm-dump] writing %s @ %uHz stereo s16\n", path, sr); fflush(stderr);
                }
            }
        }
        if (s_wav) {
            fwrite(samples, 1, num_bytes, s_wav);
            s_wav_bytes += (uint32_t)num_bytes;
            uint32_t riff = 36 + s_wav_bytes;
            fseek(s_wav, 4, SEEK_SET); fwrite(&riff, 4, 1, s_wav);
            fseek(s_wav, 40, SEEK_SET); fwrite(&s_wav_bytes, 4, 1, s_wav);
            fseek(s_wav, 0, SEEK_END); fflush(s_wav);
        }
        // Speaker-safety master gain: the MusyX mix clips hard (peaks slam the int16 rail)
        // during busy cinematic SFX. Scale down before SDL so it doesn't blast/distort.
        // Tunable via ROGUESQ_AUDIO_GAIN (0.0-1.0); default 0.5. Applied after the raw WAV dump.
        { static float g = -1.0f;
          if (g < 0.0f) { const char* e = recomp::os::getenv("ROGUESQ_AUDIO_GAIN"); g = (e && e[0]) ? (float)atof(e) : 0.35f; if (g < 0.0f || g > 1.0f) g = 0.35f; }
          if (g < 0.999f) { size_t n = num_bytes / 2; for (size_t i = 0; i < n; ++i) samples[i] = (int16_t)((float)samples[i] * g); } }
        SDL_QueueAudio(audio_device, samples, (Uint32)num_bytes);
    }
}

static size_t get_frames_remaining() {
    if (!audio_device) return 0;
    size_t frames = SDL_GetQueuedAudioSize(audio_device) / 4;  // 4 bytes/stereo frame
    // Deepen the game's audio target. ultramodern feeds this to the demand-paced game
    // audio loop, which keeps producing small (~96-frame) buffers until the reported
    // backlog reaches its target. The default target is only ~0.5 VI (~4ms), so host
    // scheduling jitter underruns it -> crackle. Under-reporting by a cushion makes the
    // game maintain a DEEPER real queue that rides through jitter. This is the src-side
    // equivalent of raising ultramodern's buffer_offset_frames (which is wrongly left at
    // the Godot value 0.5). Tunable via ROGUESQ_AUDIO_LATENCY_MS (default 60; 0 = off). 100 was
    // audibly late against the picture (2026-09-08); the synth now runs on the retrace thread each VI.
    static int ms = -1;
    if (ms < 0) {
        ms = env_int("ROGUESQ_AUDIO_LATENCY_MS", 60);
        if (ms < 0) ms = 0;
    }
    size_t cushion = (size_t)audio_sample_rate * (unsigned)ms / 1000u;
    return frames > cushion ? frames - cushion : 0;
}

#ifdef _WIN32
// Forward declaration so the F12 hotkey in poll_input() can write a dump.
static void write_minidump_safe(EXCEPTION_POINTERS* ep);
// Forward declaration so the crash handler can render the offending thread
// stack with symbols. Non-static so the RT64 d3d12 allocator-failure tracer
// can extern-declare and call it.
void print_stack_with_symbols(void** frames, USHORT count);
#else
// No DbgHelp/minidump support off-Windows; the F12 hotkey path is a no-op.
static void write_minidump_safe(void*) {}
#endif

// (Periodic mqdiag watchdog removed — mqdiag_dump lived in the librecomp
//  fork and isn't in upstream. Hangs are rare enough now that on-demand
//  dump-game.ps1 is sufficient.)

// Captured RDRAM base (set by the entrypoint wrapper) so detached diagnostic
// watchdog threads can read game memory.
extern "C" volatile uint8_t* volatile g_recomp_rdram_for_wp_raw = nullptr;

// Periodic poll of the scene-state struct at D_80130B10 + the per-frame
// callback array at D_8011A8A4. Both regions are documented in
// `project_thread_topology_2026_05_09.md`. Gated by ROGUESQ_LOG_STATE=1.
//
// Reports the first dump immediately, then logs changes only. The state byte
// at +0x14 of D_80130B10 indexes the level/scene jump-table at jtbl_8003A3E8.
// The 4 callback slots at D_8011A8A4 are the per-frame draw callbacks the
// main game thread iterates each frame.
static void start_state_poller() {
    if (!recomp::os::getenv("ROGUESQ_LOG_STATE")) return;
    static std::thread t{[]{
        uint8_t* rdram = nullptr;
        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // KSEG0 → RDRAM-relative offset (mask 0x00FFFFFF).
        const uint32_t state_off  = 0x130B10;  // D_80130B10..B40 (scene state)
        const uint32_t cbarr_off  = 0x11A8A4;  // D_8011A8A4..B4  (4 callbacks)
        // 12 words covers B10..B3F. asm/main/3EBA0.s reads bytes at +0x20,
        // +0x21, +0x22 (D_80130B30..B32) to gate state transitions, so the
        // first poll missed exactly the bytes needed for forward-progress
        // analysis. Now we cover the whole B10..B3F region.
        uint32_t prev[16] = {0xFFFFFFFFu};
        bool first = true;
        for (int i = 0; i < 1200; ++i) {  // ~60s @ 50ms
            uint32_t state[12];
            for (int j = 0; j < 12; ++j) state[j] = rdram_be32(rdram, state_off + j * 4);
            uint32_t cbs[4];    // 4 callback fn ptrs
            for (int j = 0; j < 4; ++j) cbs[j]   = rdram_be32(rdram, cbarr_off + j * 4);

            // Drain-loop diagnostic — boot thread is stuck waiting for these.
            // D_800A0F50 byte: drain-enable gate (0 = bypass drain, return ready)
            // D_80141AD0 byte: drain-entry count
            // D_801496F8 word: array base ptr (each entry 0x88 bytes; byte[0] = active flag)
            uint8_t  a0f50 = rdram[0xA0F50 ^ 3];
            uint8_t  c1ad0 = rdram[0x141AD0 ^ 3];
            uint32_t array_base = rdram_be32(rdram, 0x1496F8);
            // Sound-asset data dumps — boot deadlocks in func_80097518 walking
            // pool_SND, possibly because the asset loader leaves buffers zeroed.
            // Dump all 3 sound assets once each when they become non-null.
            //   D_80139B4C = sound/proj_SND  (first asset)
            //   D_80139B54 = sound/pool_SND  (second asset, fed to func_80097518)
            //   D_80139B50 = sound/sdir_SND  (third asset)
            static bool snd_dumped[3] = {false, false, false};
            const uint32_t snd_addrs[3] = {0x139B4C, 0x139B54, 0x139B50};
            const char* snd_names[3] = {"proj_SND", "pool_SND", "sdir_SND"};
            for (int s = 0; s < 3; ++s) {
                if (snd_dumped[s]) continue;
                uint32_t ptr = rdram_be32(rdram, snd_addrs[s]);
                uint32_t off = ptr & 0x00FFFFFFu;
                if ((ptr & 0xF0000000u) == 0x80000000u && off + 0x40 < 0x800000u) {
                    fprintf(stderr, "[%s] t=%4d ms ptr=0x%08X first 0x40 bytes (BE):\n",
                            snd_names[s], i * 50, ptr);
                    for (int row = 0; row < 4; ++row) {
                        fprintf(stderr, "  %04X:", row * 16);
                        for (int b = 0; b < 16; ++b) {
                            fprintf(stderr, " %02X", rdram[(off + row * 16 + b) ^ 3]);
                        }
                        fprintf(stderr, "\n");
                    }
                    fflush(stderr);
                    snd_dumped[s] = true;
                    // EXPERIMENTAL: if the buffer is all zeros, write -1 sentinel
                    // into the first word. The user's hypothesis is that the
                    // attribution screen has no audio, so empty pool is correct
                    // and the game's pool walker should exit early on -1 sentinel.
                    // Our zero-init heap puts 0 instead. Test the hypothesis:
                    // ROGUESQ_FORCE_EMPTY_POOL_SENTINEL=1.
                    if (recomp::os::getenv("ROGUESQ_FORCE_EMPTY_POOL_SENTINEL")) {
                        bool all_zero = true;
                        for (int j = 0; j < 16; ++j) {
                            if (rdram[(off + j) ^ 3] != 0) { all_zero = false; break; }
                        }
                        if (all_zero) {
                            // Write 0xFFFFFFFF (BE) to first word.
                            for (int j = 0; j < 4; ++j) {
                                rdram[(off + j) ^ 3] = 0xFF;
                            }
                            fprintf(stderr,
                                "[%s-FIX] wrote sentinel -1 to first word at 0x%08X\n",
                                snd_names[s], ptr);
                            fflush(stderr);
                        }
                    }
                }
            }
            // Compute first-N active flags if array_base is a sane KSEG0 ptr
            uint8_t flags[8] = {0};
            uint32_t array_off = array_base & 0x00FFFFFFu;
            int n = c1ad0 < 8 ? c1ad0 : 8;
            if ((array_base & 0xF0000000u) == 0x80000000u && array_off + n * 0x88 < 0x800000u) {
                for (int j = 0; j < n; ++j) {
                    flags[j] = rdram[(array_off + j * 0x88) ^ 3];
                }
            }

            uint32_t cur[16];
            for (int j = 0; j < 12; ++j) cur[j] = state[j];
            for (int j = 0; j < 4; ++j) cur[12 + j] = cbs[j];
            bool changed = first;
            for (int j = 0; j < 16; ++j) if (cur[j] != prev[j]) { changed = true; break; }
            // Track drain state too — re-log when it changes.
            static uint8_t prev_a0f50 = 0xFF, prev_c1ad0 = 0xFF;
            static uint32_t prev_array_base = 0xFFFFFFFFu;
            static uint8_t prev_flags[8] = {0xFF};
            if (a0f50 != prev_a0f50 || c1ad0 != prev_c1ad0 || array_base != prev_array_base) changed = true;
            for (int j = 0; j < 8; ++j) if (flags[j] != prev_flags[j]) { changed = true; break; }
            if (changed) {
                first = false;
                fprintf(stderr,
                    "[state] t=%4d ms B10=%08X B14=%08X B18=%08X B1C=%08X B20=%08X B24=%08X B28=%08X B2C=%08X B30=%08X B34=%08X B38=%08X B3C=%08X | scene=%u | cb=[%08X,%08X,%08X,%08X] | drain.A0F50=%02X drain.cnt=%u arr=0x%08X flags=[%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X]\n",
                    i * 50,
                    state[0], state[1], state[2], state[3], state[4], state[5],
                    state[6], state[7], state[8], state[9], state[10], state[11],
                    (state[1] >> 24) & 0xFFu,
                    cbs[0], cbs[1], cbs[2], cbs[3],
                    a0f50, c1ad0, array_base,
                    flags[0], flags[1], flags[2], flags[3],
                    flags[4], flags[5], flags[6], flags[7]);
                fflush(stderr);
                for (int j = 0; j < 16; ++j) prev[j] = cur[j];
                prev_a0f50 = a0f50;
                prev_c1ad0 = c1ad0;
                prev_array_base = array_base;
                for (int j = 0; j < 8; ++j) prev_flags[j] = flags[j];
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }};
    t.detach();
}

// Periodic poll of cinematic phase state so we can see stage transitions
// and timing. Gated by ROGUESQ_LOG_PHASE=1. Reports on change only.
//
// State variables (per memory notes project_cinematic_data_structures.md
// + project_b50_b58_polled_state.md):
//   cineState  : MEM_W[0x800B0934] — master cinematic state word
//   gateCtr    : MEM_W[0x800B0B28] — cinematic frame counter (compared
//                vs cutscene[0x44])
//   cutscene   : MEM_W[0x800B1904] — pointer to currently-loaded cutscene
//                struct
//   cuts_44    : MEM_W[cutscene+0x44] — total-count / duration field
//   B50        : MEM_W[0x80130B50] — state word, bit 5 is inner-loop
//                advance gate
//   B58        : MEM_W[0x80130B58] — state word, bit 25 is outer gate
//   slot_dispatcher_ptr : MEM_W[0x80130BB0] — pointer to 6-slot table
//                (each slot is 8B, jalr handler at slot+0x00)
//   active_slot_indices : 6 halfwords at 0x80139560
static void start_phase_poller() {
    if (!recomp::os::getenv("ROGUESQ_LOG_PHASE")) return;
    static std::thread t{[]{
        uint8_t* rdram = nullptr;
        while (!(rdram = (uint8_t*)g_recomp_rdram_for_wp_raw)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        uint32_t prev_cineState = 0xFFFFFFFFu;
        uint32_t prev_gateCtr   = 0xFFFFFFFFu;
        uint32_t prev_cutscene  = 0xFFFFFFFFu;
        uint32_t prev_cuts44    = 0xFFFFFFFFu;
        uint32_t prev_b50       = 0xFFFFFFFFu;
        uint32_t prev_b58       = 0xFFFFFFFFu;
        uint32_t prev_slot_tbl  = 0xFFFFFFFFu;
        uint32_t prev_cur_ovl   = 0xFFFFFFFFu;
        uint32_t prev_star_word = 0xFFFFFFFFu;
        uint32_t prev_cb0 = 0xDEADBEEFu, prev_cb1 = 0xDEADBEEFu;
        uint32_t prev_cb2 = 0xDEADBEEFu, prev_cb3 = 0xDEADBEEFu;
        uint16_t prev_active[6] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
        uint64_t start_ms = GetTickCount64();
        uint64_t last_change_ms = start_ms;
        bool first = true;

        auto rdram_be32_at = [&](uint32_t off) -> uint32_t { return rdram_be32(rdram, off); };
        auto rdram_be16_at = [&](uint32_t off) -> uint16_t { return rdram_be16(rdram, off); };

        for (int i = 0; i < 1200; ++i) {  // ~60s @ 50ms
            uint32_t cur_ovl   = rdram_be32_at(0x375B0);  // gCurrentLoadedOverlay
            // Probe first 4 bytes of STAR WARS string at menu_overlay 0xA5E60
            // — if menu overlay is loaded, these should spell 'STAR' (0x53 54 41 52).
            uint32_t star_word = rdram_be32_at(0xA5E60);
            // Per-frame draw callbacks at 0x8011A8A4 (4 slots, fn ptrs)
            uint32_t cb0 = rdram_be32_at(0x11A8A4);
            uint32_t cb1 = rdram_be32_at(0x11A8A8);
            uint32_t cb2 = rdram_be32_at(0x11A8AC);
            uint32_t cb3 = rdram_be32_at(0x11A8B0);
            uint32_t cineState = rdram_be32_at(0xB0934);
            uint32_t gateCtr   = rdram_be32_at(0xB0B28);
            uint32_t cutscene  = rdram_be32_at(0xB1904);
            uint32_t cuts44 = 0;
            if ((cutscene & 0xF0000000u) == 0x80000000u &&
                (cutscene & 0x00FFFFFFu) + 0x44 < 0x800000u) {
                cuts44 = rdram_be32_at((cutscene & 0x00FFFFFFu) + 0x44);
            }
            uint32_t b50      = rdram_be32_at(0x130B50);
            uint32_t b58      = rdram_be32_at(0x130B58);
            uint32_t slot_tbl = rdram_be32_at(0x130BB0);
            uint16_t active[6];
            for (int j = 0; j < 6; ++j) active[j] = rdram_be16_at(0x139560 + j * 2);

            bool changed = first;
            if (cineState != prev_cineState) changed = true;
            if (gateCtr   != prev_gateCtr)   changed = true;
            if (cutscene  != prev_cutscene)  changed = true;
            if (cuts44    != prev_cuts44)    changed = true;
            if (b50       != prev_b50)       changed = true;
            if (b58       != prev_b58)       changed = true;
            if (slot_tbl  != prev_slot_tbl)  changed = true;
            if (cur_ovl   != prev_cur_ovl)   changed = true;
            if (star_word != prev_star_word) changed = true;
            if (cb0 != prev_cb0 || cb1 != prev_cb1 || cb2 != prev_cb2 || cb3 != prev_cb3) changed = true;
            for (int j = 0; j < 6; ++j) {
                if (active[j] != prev_active[j]) { changed = true; break; }
            }

            if (changed) {
                uint64_t now = GetTickCount64();
                uint64_t since_start = now - start_ms;
                uint64_t stable = now - last_change_ms;
                // Decode star_word as 4-char ASCII for at-a-glance
                // "is the menu overlay's STAR WARS string here" check.
                char sw[5]; sw[0]=(char)(star_word>>24); sw[1]=(char)(star_word>>16);
                sw[2]=(char)(star_word>>8); sw[3]=(char)star_word; sw[4]=0;
                for (int k=0;k<4;k++) if (sw[k]<0x20||sw[k]>0x7E) sw[k]='.';
                fprintf(stderr,
                    "[phase] t=%5llums (stable %4llums)  cineState=%08X gateCtr=%5u "
                    "cuts=%08X cuts44=%5u B50=%08X B58=%08X slotTbl=%08X "
                    "active=[%04X %04X %04X %04X %04X %04X] curOvl=%u starWord=0x%08X(%s) "
                    "cb=[%08X %08X %08X %08X]\n",
                    (unsigned long long)since_start, (unsigned long long)stable,
                    cineState, gateCtr,
                    cutscene, cuts44,
                    b50, b58,
                    slot_tbl,
                    active[0], active[1], active[2], active[3], active[4], active[5],
                    cur_ovl, star_word, sw,
                    cb0, cb1, cb2, cb3);
                fflush(stderr);

                // When slotTbl becomes valid, dump non-empty slot entries
                // so we can see which NPCs are populated. Compare to real
                // N64 RAM dumps (e.g. lucasArtsScreen has handlers in
                // 0x80206000-0x80209000 range; N64-logo state is different).
                if ((slot_tbl & 0xF0000000u) == 0x80000000u && slot_tbl != prev_slot_tbl) {
                    uint32_t pool_off = slot_tbl & 0x00FFFFFFu;
                    int populated = 0;
                    fprintf(stderr, "[phase] slot pool @ 0x%08X populated entries:\n", slot_tbl);
                    for (int s = 0; s < 2048 && populated < 40; ++s) {
                        uint32_t entry_off = pool_off + (uint32_t)s * 8;
                        if (entry_off + 8 > 0x800000u) break;
                        uint32_t h = rdram_be32_at(entry_off);
                        uint32_t w = rdram_be32_at(entry_off + 4);
                        if (h != 0 && h != 0xFFFFFFFFu) {
                            fprintf(stderr, "  slot[%4d]: handler=0x%08X  word2=0x%08X\n", s, h, w);
                            ++populated;
                        }
                    }
                    fprintf(stderr, "[phase] (showing first %d populated of pool 2048)\n", populated);
                    fflush(stderr);
                }
                first = false;
                prev_cineState = cineState;
                prev_gateCtr   = gateCtr;
                prev_cutscene  = cutscene;
                prev_cuts44    = cuts44;
                prev_b50       = b50;
                prev_b58       = b58;
                prev_slot_tbl  = slot_tbl;
                prev_cur_ovl   = cur_ovl;
                prev_star_word = star_word;
                prev_cb0 = cb0; prev_cb1 = cb1; prev_cb2 = cb2; prev_cb3 = cb3;
                for (int j = 0; j < 6; ++j) prev_active[j] = active[j];
                last_change_ms = now;
            }
            // Periodic slot-pool snapshot every ~1 second so we can see if
            // NPCs spawn over time (real N64 attribution has 30+ populated;
            // if we stay at 4, something's stopping further spawns).
            if ((i % 20) == 0 && (slot_tbl & 0xF0000000u) == 0x80000000u) {
                uint32_t pool_off = slot_tbl & 0x00FFFFFFu;
                int total_pop = 0;
                for (int s = 0; s < 2048; ++s) {
                    uint32_t entry_off = pool_off + (uint32_t)s * 8;
                    if (entry_off + 8 > 0x800000u) break;
                    uint32_t h = rdram_be32_at(entry_off);
                    if (h != 0 && h != 0xFFFFFFFFu) ++total_pop;
                }
                fprintf(stderr, "[phase] t=%5llums slot-pool count = %d\n",
                        (unsigned long long)(GetTickCount64() - start_ms), total_pop);
                fflush(stderr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }};
    t.detach();
}

// ---------------------------------------------------------------------------
// Input (SDL2 gamepad — one controller)
// ---------------------------------------------------------------------------
static SDL_GameController* controller = nullptr;

// ROGUESQ_FAKE_CONTROLLER=1 — report a connected controller and feed neutral
// input even with no physical gamepad attached. Lets headless/automated runs
// pass the game's "NO CONTROLLER" gate and reach the cinematic/menu for
// screenshot + DL-dump iteration (no controller = oracle is the user otherwise).
// ROGUESQ_AUTO_START=<ms>: once past the gate, pulse START every <ms> to advance
// through prompts (0/unset = never). Self-test aid only; default OFF.
static bool fake_controller_enabled() {
    static bool s = env_on("ROGUESQ_FAKE_CONTROLLER");
    return s;
}

// ROGUESQ_INPUT_SEQ="name:holdMs:afterMs,..." — a scripted virtual-controller
// timeline injected at get_n64_input (no window focus needed, unlike SendInput).
// ROGUESQ_INPUT_DELAY=<ms> waits before the first step (default 12000, to clear boot).
// Names: start a b z l r  dup ddown dleft dright  cup cdown cleft cright
//        up down left right (analog stick)  wait (neutral hold).
// Used for headless front-end navigation + state calibration. Default OFF.
struct InputStep { uint16_t mask; float sx, sy; uint32_t hold, after; };

static bool scripted_input(uint16_t* buttons, float* x, float* y) {
    static int s_init = -1;
    static std::vector<InputStep> s_steps;
    static uint32_t s_delay = 12000;
    static uint32_t s_t0 = 0;            // wall-clock at first eligible call
    static bool s_done_logged = false;
    if (s_init < 0) {
        s_init = 0;
        const char* seq = recomp::dbg::env_str("ROGUESQ_INPUT_SEQ");
        if (seq && *seq) {
            s_init = 1;
            s_delay = (uint32_t)env_int("ROGUESQ_INPUT_DELAY", 12000);
            auto tok = [](const std::string& n) -> InputStep {
                InputStep s{0, 0.f, 0.f, 90, 400};
                std::string name = n; size_t c1 = name.find(':');
                std::string hold, after;
                if (c1 != std::string::npos) {
                    size_t c2 = name.find(':', c1 + 1);
                    hold = name.substr(c1 + 1, (c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
                    if (c2 != std::string::npos) after = name.substr(c2 + 1);
                    name = name.substr(0, c1);
                }
                if (!hold.empty()) s.hold = (uint32_t)std::atoi(hold.c_str());
                if (!after.empty()) s.after = (uint32_t)std::atoi(after.c_str());
                if      (name == "start") s.mask = N64_START_BUTTON;
                else if (name == "a")     s.mask = N64_A_BUTTON;
                else if (name == "b")     s.mask = N64_B_BUTTON;
                else if (name == "z")     s.mask = N64_Z_TRIG;
                else if (name == "l")     s.mask = N64_L_TRIG;
                else if (name == "r")     s.mask = N64_R_TRIG;
                else if (name == "dup")   s.mask = N64_U_JPAD;
                else if (name == "ddown") s.mask = N64_D_JPAD;
                else if (name == "dleft") s.mask = N64_L_JPAD;
                else if (name == "dright")s.mask = N64_R_JPAD;
                else if (name == "cup")   s.mask = N64_U_CBUTTONS;
                else if (name == "cdown") s.mask = N64_D_CBUTTONS;
                else if (name == "cleft") s.mask = N64_L_CBUTTONS;
                else if (name == "cright")s.mask = N64_R_CBUTTONS;
                else if (name == "up")    s.sy = 1.0f;
                else if (name == "down")  s.sy = -1.0f;
                else if (name == "left")  s.sx = -1.0f;
                else if (name == "right") s.sx = 1.0f;
                // "wait" / unknown -> neutral hold
                return s;
            };
            std::string all(seq), cur;
            for (size_t i = 0; i <= all.size(); ++i) {
                if (i == all.size() || all[i] == ',') { if (!cur.empty()) s_steps.push_back(tok(cur)); cur.clear(); }
                else cur.push_back(all[i]);
            }
        }
    }
    if (s_init != 1) return false;

    uint32_t now = SDL_GetTicks();
    if (s_t0 == 0) s_t0 = now;
    uint32_t elapsed = now - s_t0;
    if (elapsed < s_delay) { *buttons = 0; *x = 0.f; *y = 0.f; return true; }
    uint32_t t = elapsed - s_delay;
    uint32_t acc = 0;
    for (const InputStep& st : s_steps) {
        uint32_t span = st.hold + st.after;
        if (t < acc + span) {
            bool holding = (t - acc) < st.hold;
            *buttons = holding ? st.mask : 0;
            *x = holding ? st.sx : 0.f;
            *y = holding ? st.sy : 0.f;
            return true;
        }
        acc += span;
    }
    if (!s_done_logged) { s_done_logged = true; fprintf(stderr, "[input-seq] sequence complete at t=%ums\n", elapsed); fflush(stderr); }
    *buttons = 0; *x = 0.f; *y = 0.f;
    return true;
}

// Requested by the custom main-menu QUIT entry (menuControllerInput hook).
// Pushes SDL_QUIT so the event loop runs the graceful shutdown path.
extern "C" void rs64_menu_request_quit(void) {
    SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q);
}

// Active bindings + mouse-steering runtime state. poll_input and get_n64_input
// run on the same game thread and in a paired sequence per controller read, so
// the mouse accumulators are plain game-thread statics. g_mouse_capture is read
// from update_gfx (main thread) to toggle relative-mouse mode, so it is atomic.
static rs64::input::Bindings g_bindings = rs64::input::default_bindings();
static std::mutex            g_bindings_mtx;    // UI thread mutates, game thread reads
static std::atomic<bool>     g_show_controls{false};
static std::atomic<bool>     g_mouse_capture{false};
static float                 g_mouse_ax = 0.0f;   // accumulated relative motion this frame
static float                 g_mouse_ay = 0.0f;
static uint32_t              g_mouse_btn = 0;

// Fullscreen: set/toggled from any thread (menu, UI); applied on the main thread
// in poll_input via SDL_SetWindowFullscreen (RT64 resizes its swapchain off the
// resulting resize event). g_sdl_window is set in create_window.
SDL_Window*                  g_sdl_window = nullptr;
static std::atomic<bool>     g_fullscreen{false};
static std::atomic<bool>     g_fullscreen_dirty{false};

extern "C" void rs64_set_fullscreen(int on)   { g_fullscreen.store(on != 0); g_fullscreen_dirty.store(true); }
extern "C" int  rs64_get_fullscreen(void)     { return g_fullscreen.load() ? 1 : 0; }
extern "C" void rs64_toggle_fullscreen(void)  { g_fullscreen.store(!g_fullscreen.load()); g_fullscreen_dirty.store(true); }

static void apply_fullscreen_if_requested() {
    if (g_fullscreen_dirty.exchange(false) && g_sdl_window) {
        SDL_SetWindowFullscreen(g_sdl_window, g_fullscreen.load() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
}

static void poll_input() {
    apply_fullscreen_if_requested();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        // F6 toggles the controls/rebind window. Mouse-steering capture is
        // automatic (focus-driven, in update_gfx) -- no manual toggle.
        if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            if (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
                rs64_toggle_fullscreen();   // Alt+Enter: standard fullscreen toggle
            } else if (e.key.keysym.scancode == SDL_SCANCODE_F6) {
                g_show_controls.store(!g_show_controls.load());
            } else if (e.key.keysym.scancode == SDL_SCANCODE_F5) {
                // Toggle Factor 5's built-in frame-profiler/debug-text HUD.
                // Gate byte at virtual 0x80038CE0 (retail leaves it 0); the
                // emitter early-returns when zero. ^3 = N64 byte order.
                if (uint8_t* rd = (uint8_t*)g_recomp_rdram_for_wp_raw) {
                    uint8_t& gate = rd[0x38CE0 ^ 3];
                    gate = gate ? 0 : 1;
                    fprintf(stderr, "[F5] profiler HUD gate 0x80038CE0 -> %u\n", gate);
                    fflush(stderr);
                }
            } else if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                // Esc closes the controls window; otherwise it falls through to
                // the game (bound to Start/pause).
                if (g_show_controls.load()) g_show_controls.store(false);
            }
        }
        if (e.type == SDL_QUIT) {
            fprintf(stderr, "[main] SDL_QUIT received, exiting\n");
            fflush(stderr);
            // _Exit, not exit: the recomp game threads and RT64 are still live, so
            // running the C++/atexit static-destructor table here throws (-> terminate,
            // the Release crash-on-close) or deadlocks (the hang). Terminate now; the OS
            // reclaims SDL/GPU/process resources. Matches the crash handlers' _Exit.
            _Exit(EXIT_SUCCESS);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            if (!controller) {
                controller = SDL_GameControllerOpen(e.cdevice.which);
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && controller) {
            if (SDL_GameControllerGetJoystick(controller) ==
                SDL_JoystickFromInstanceID(e.cdevice.which)) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            }
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
            fprintf(stderr, "[F12] manual minidump requested\n");
            fflush(stderr);
            write_minidump_safe(nullptr);
        }
        // Diagnostic: log F1-F4 to confirm the keys are reaching the SDL
        // queue at all. If we see these logs, SDL is delivering keypresses
        // to our process — RT64's filter (which runs before SDL_PollEvent)
        // may have already consumed them, or it may not be installed.
        if (e.type == SDL_KEYDOWN) {
            const auto& sym = e.key.keysym.sym;
            if (sym == SDLK_F1 || sym == SDLK_F2 || sym == SDLK_F3 || sym == SDLK_F4) {
                fprintf(stderr, "[input] SDL_KEYDOWN sym=%d (F%d) reached PollEvent\n",
                        (int)sym, (int)(sym - SDLK_F1 + 1));
                fflush(stderr);
            }
        }
    }
    // Drain SDL's relative-motion accumulator once per poll. Read from the
    // internal state (updated before RT64's event filter), so capture is robust
    // in dev mode. Only feed the resolver while capture is engaged.
    int rdx = 0, rdy = 0;
    uint32_t rbtn = SDL_GetRelativeMouseState(&rdx, &rdy);
    if (g_mouse_capture.load()) {
        g_mouse_ax += (float)rdx;
        g_mouse_ay += (float)rdy;
    } else {
        g_mouse_ax = g_mouse_ay = 0.0f;
    }
    g_mouse_btn = rbtn;

    // ROGUESQ_PROFILER_DUMP=1: sample the 7 F5-HUD timing slots (microseconds)
    // so each bar can be labelled. Addresses are the absolute lw sources in
    // drawFrameProfilerBars; words are stored host-order in the raw RDRAM buf.
    static int s_prof_dump = -1;
    if (s_prof_dump < 0) s_prof_dump = env_on("ROGUESQ_PROFILER_DUMP");
    if (s_prof_dump) {
        if (const uint8_t* rd = (const uint8_t*)g_recomp_rdram_for_wp_raw) {
            auto W = [rd](uint32_t va) { return *reinterpret_cast<const uint32_t*>(rd + (va & 0x7FFFFF)); };
            static int n = 0; ++n;
            if (n % 15 == 0) {
                fprintf(stderr, "[prof] yel=%u blu=%u red=%u mag=%u wht=%u grn=%u cyn=%u\n",
                        W(0x8011A914), W(0x80128EE0), W(0x80128EDC),
                        W(0x8011A920), W(0x8011DC54), W(0x8011DC4C), W(0x8011A918));
                fflush(stderr);
            }
        }
    }
}

// BOOT_TARGET: while the FrontEnd attract-title is up, the present hook sets this
// so we inject the native START title-skip. Pulsed (~120ms on / off) because the
// game's skip reads NEW button presses -- a held START would register only once.
static inline uint16_t boot_start_pulse() {
    if (!g_boot_pulse_start) return 0;
    return ((SDL_GetTicks() % 240u) < 120u) ? N64_START_BUTTON : 0;
}

static bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        *buttons = 0; *x = 0.0f; *y = 0.0f;
        return false;
    }

    // While the controls window is open, suppress gameplay input so rebinding
    // or navigating the UI doesn't also drive the ship. Still report connected.
    if (g_show_controls.load()) {
        *buttons = 0; *x = 0.0f; *y = 0.0f;
        return true;
    }

    // Headless automation overrides resolved input and must run BEFORE resolve(): keyboard is
    // enabled by default, so resolve() reports "active" even with no key pressed, which would
    // otherwise skip the fake-controller branch and swallow injected input.
    if (fake_controller_enabled()) {
        uint16_t nb = 0; float nx = 0.f, ny = 0.f;
        if (rs64_nav_consume(&nb, &nx, &ny)) { *buttons = nb; *x = nx; *y = ny; return true; }
        uint16_t sb = 0; float sx = 0.f, sy = 0.f;
        if (scripted_input(&sb, &sx, &sy)) { *buttons = sb; *x = sx; *y = sy; return true; }
    }

    // Resolve keyboard + gamepad + mouse through the bindings profile.
    rs64::input::RawState st;
    st.keys = SDL_GetKeyboardState(&st.keys_len);
    st.pad = controller;
    st.mouse_active = g_mouse_capture.load();
    // Fire buttons only while steering; middle (Start) works in menus too.
    st.mouse_buttons = st.mouse_active ? g_mouse_btn : (g_mouse_btn & SDL_BUTTON_MMASK);
    float raw_dx = st.mouse_active ? g_mouse_ax : 0.0f;
    float raw_dy = st.mouse_active ? g_mouse_ay : 0.0f;
    g_mouse_ax = g_mouse_ay = 0.0f;  // consume this frame's accumulated motion

    uint16_t btn = 0;
    bool active;
    { std::lock_guard<std::mutex> lk(g_bindings_mtx);
      // Frame-rate-aware exponential low-pass on the mouse velocity so fast
      // flicks ramp in and stop-motion eases back to center, instead of the raw
      // per-frame delta snapping the stick to full/zero each frame.
      static uint32_t s_last_ms = 0;
      static float s_sm_dx = 0.0f, s_sm_dy = 0.0f;
      uint32_t now = SDL_GetTicks();
      float dt = s_last_ms ? (float)(now - s_last_ms) : 16.0f;
      s_last_ms = now;
      float tau = g_bindings.mouse_smoothing * 100.0f;  // smoothing amount -> ms time constant
      float alpha = tau > 0.0f ? dt / (dt + tau) : 1.0f;
      s_sm_dx += (raw_dx - s_sm_dx) * alpha;
      s_sm_dy += (raw_dy - s_sm_dy) * alpha;
      st.mouse_dx = s_sm_dx; st.mouse_dy = s_sm_dy;
      active = rs64::input::resolve(g_bindings, st, &btn, x, y); }

    if (!active) {
        // No keyboard and no gamepad: keep the fake-controller path so headless
        // runs still clear the "NO CONTROLLER" gate.
        if (fake_controller_enabled()) {
            // nav/scripted injection already handled above (before resolve).
            static int s_auto = -1;
            if (s_auto < 0) s_auto = env_int("ROGUESQ_AUTO_START", 0);
            uint16_t fb = 0;
            if (s_auto > 0 && (SDL_GetTicks() % (uint32_t)s_auto) < 120u) fb |= N64_START_BUTTON;
            fb |= boot_start_pulse();
            *buttons = fb; *x = 0.0f; *y = 0.0f;
            return true;
        }
        *buttons = 0; *x = 0.0f; *y = 0.0f;
        return false;
    }

    static int s_auto = -1;
    if (s_auto < 0) s_auto = env_int("ROGUESQ_AUTO_START", 0);
    if (s_auto > 0 && (SDL_GetTicks() % (uint32_t)s_auto) < 120u) btn |= N64_START_BUTTON;
    btn |= boot_start_pulse();
    *buttons = btn;
    return true;
}

static void set_rumble(int, bool) {}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0 && (controller || g_bindings.keyboard_enabled || fake_controller_enabled())) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// Controls / rebind window. Runs on the RT64 graphics thread via the ImGui hook
// (SetRenderHookImgui), between NewFrame and Render. All g_bindings mutation is
// under g_bindings_mtx since get_n64_input reads it on the game thread. Only
// active in developer mode (the inspector frame); F6 toggles visibility.
static void draw_controls_ui() {
    if (!g_show_controls.load()) return;
    namespace ri = rs64::input;

    static int capture_target = -1;                 // Target awaiting a new bind
    static uint8_t prev_keys[SDL_NUM_SCANCODES] = {0};
    static uint32_t prev_pad = 0, prev_mouse = 0;

    ImGui::SetNextWindowSize(ImVec2(540, 470), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Controls", &open)) {
        // Action row at the top so it stays visible even if the window is taller
        // than the game viewport.
        if (ImGui::Button("Save")) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore defaults")) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            g_bindings = rs64::input::default_bindings();
            rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) g_show_controls.store(false);
        ImGui::Separator();
        ImGui::TextWrapped("Click Rebind, then press a key, gamepad button, or mouse button. Esc cancels a rebind.");
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ImGui::SliderFloat("Mouse sensitivity", &g_bindings.mouse_sensitivity, 0.005f, 0.30f, "%.3f");
            ImGui::SliderFloat("Mouse smoothing", &g_bindings.mouse_smoothing, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Mouse response curve", &g_bindings.mouse_curve, 1.0f, 3.0f, "%.2f");
            ImGui::Checkbox("Invert X", &g_bindings.mouse_invert_x); ImGui::SameLine();
            ImGui::Checkbox("Invert Y", &g_bindings.mouse_invert_y);
        }
        ImGui::TextDisabled("Mouse-steering is automatic while the window is focused; F6 or Esc closes this.");
        {
            bool fs = rs64_get_fullscreen() != 0;
            if (ImGui::Checkbox("Fullscreen", &fs)) rs64_set_fullscreen(fs);
            ImGui::SameLine(); ImGui::TextDisabled("(Alt+Enter)");
        }
        ImGui::Separator();

        // Edge-detect a captured input while a rebind is pending.
        if (capture_target >= 0) {
            int nk = 0; const uint8_t* ks = SDL_GetKeyboardState(&nk);
            if (ks[SDL_SCANCODE_ESCAPE]) {
                capture_target = -1;
            } else {
                ri::Source got; bool have = false;
                for (int sc = 0; sc < nk && !have; ++sc)
                    if (sc != SDL_SCANCODE_ESCAPE && ks[sc] && !prev_keys[sc]) { got = { ri::SourceKind::Key, sc, 0 }; have = true; }
                if (!have && controller)
                    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && !have; ++b) {
                        bool now = SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b);
                        if (now && !((prev_pad >> b) & 1)) { got = { ri::SourceKind::PadButton, b, 0 }; have = true; }
                    }
                if (!have) {
                    uint32_t mb = SDL_GetMouseState(nullptr, nullptr);
                    for (int b = 1; b <= 5 && !have; ++b)
                        if ((mb & SDL_BUTTON(b)) && !(prev_mouse & SDL_BUTTON(b))) { got = { ri::SourceKind::MouseButton, b, 0 }; have = true; }
                }
                if (have) {
                    std::lock_guard<std::mutex> lk(g_bindings_mtx);
                    g_bindings.targets[capture_target].clear();
                    g_bindings.targets[capture_target].push_back(got);
                    capture_target = -1;
                }
            }
        }
        { int nk = 0; const uint8_t* ks = SDL_GetKeyboardState(&nk);
          int n = nk < (int)SDL_NUM_SCANCODES ? nk : (int)SDL_NUM_SCANCODES; memcpy(prev_keys, ks, n); }
        prev_pad = 0;
        if (controller) for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)b)) prev_pad |= (1u << b);
        prev_mouse = SDL_GetMouseState(nullptr, nullptr);

        ImGui::BeginChild("binds", ImVec2(0, 0), true);
        {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            for (int t = 0; t < ri::target_count(); ++t) {
                ImGui::PushID(t);
                ImGui::Text("%s", ri::target_label((ri::Target)t));
                ImGui::SameLine(120);
                if (capture_target == t) {
                    ImGui::TextColored(ImVec4(1, 1, 0, 1), "[press an input...]");
                } else {
                    std::string b;
                    for (const auto& s : g_bindings.targets[t]) { if (!b.empty()) b += ", "; b += ri::source_label(s); }
                    ImGui::TextUnformatted(b.empty() ? "(unbound)" : b.c_str());
                }
                ImGui::SameLine(330);
                if (ImGui::SmallButton("Rebind")) capture_target = t;
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) g_bindings.targets[t].clear();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (!open) g_show_controls.store(false);
}

extern "C" void rs64_menu_config_mark_dirty(void);   // menu_config.cpp
extern "C" void rs64_menu_config_init(void);         // menu_config.cpp

// Render one mod config-schema option as an ImGui widget and persist edits.
static void draw_mod_config_option(const std::string& mod_id, const recomp::mods::ConfigOption& opt) {
    using namespace recomp::mods;
    ConfigValueVariant v = get_mod_config_value(mod_id, opt.id);
    bool changed = false;
    switch (opt.type) {
        case ConfigOptionType::Enum: {
            const auto& e = std::get<ConfigOptionEnum>(opt.variant);
            int cur = std::holds_alternative<uint32_t>(v) ? (int)std::get<uint32_t>(v) : (int)e.default_value;
            std::vector<const char*> items; items.reserve(e.options.size());
            for (const auto& s : e.options) items.push_back(s.c_str());
            if (ImGui::Combo(opt.name.c_str(), &cur, items.data(), (int)items.size())) {
                set_mod_config_value(mod_id, opt.id, (uint32_t)cur); changed = true;
            }
            break;
        }
        case ConfigOptionType::Number: {
            const auto& n = std::get<ConfigOptionNumber>(opt.variant);
            double dv = std::holds_alternative<double>(v) ? std::get<double>(v) : n.default_value;
            float f = (float)dv;
            if (ImGui::SliderFloat(opt.name.c_str(), &f, (float)n.min, (float)n.max)) {
                set_mod_config_value(mod_id, opt.id, (double)f); changed = true;
            }
            break;
        }
        case ConfigOptionType::String: {
            const auto& s = std::get<ConfigOptionString>(opt.variant);
            std::string cur = std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : s.default_value;
            char buf[128]; std::snprintf(buf, sizeof buf, "%s", cur.c_str());
            if (ImGui::InputText(opt.name.c_str(), buf, sizeof buf)) {
                set_mod_config_value(mod_id, opt.id, std::string(buf)); changed = true;
            }
            break;
        }
        default: break;
    }
    if (!opt.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", opt.description.c_str());
    if (changed) rs64_menu_config_mark_dirty();
}

// A "Mods" panel: lists loaded mods, an enable toggle, and each mod's
// config-schema options (rendered from librecomp's mod system).
static void draw_mods_ui() {
    if (!g_show_controls.load()) return;
    ImGui::SetNextWindowSize(ImVec2(460, 380), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Mods")) {
        auto mods = recomp::mods::get_all_mod_details("rs64");
        if (mods.empty()) ImGui::TextDisabled("No mods loaded. Put mods in the 'mods' folder.");
        for (const auto& d : mods) {
            ImGui::PushID(d.mod_id.c_str());
            bool enabled = recomp::mods::is_mod_enabled(d.mod_id);
            if (ImGui::Checkbox("##enabled", &enabled)) {
                recomp::mods::enable_mod(d.mod_id, enabled);
                rs64_menu_config_mark_dirty();
            }
            ImGui::SameLine();
            const char* title = d.display_name.empty() ? d.mod_id.c_str() : d.display_name.c_str();
            if (ImGui::CollapsingHeader(title)) {
                if (!d.description.empty()) ImGui::TextWrapped("%s", d.description.c_str());
                const auto& schema = recomp::mods::get_mod_config_schema(d.mod_id);
                if (schema.options.empty()) ImGui::TextDisabled("(no options)");
                for (const auto& opt : schema.options) {
                    ImGui::PushID(opt.id.c_str());
                    draw_mod_config_option(d.mod_id, opt);
                    ImGui::PopID();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

// The single ImGui render hook draws both dev panels.
static void draw_dev_ui() {
    draw_controls_ui();
    draw_mods_ui();
}

// ---------------------------------------------------------------------------
// Graphics (SDL2 window creation — rt64 takes over from here)
// ---------------------------------------------------------------------------
ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#ifndef NDEBUG
    // Debug builds: don't steal focus from the editor when the window first shows.
    SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    return nullptr;
}


ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // On non-Windows RT64 renders through Vulkan and needs the window created
    // with SDL_WINDOW_VULKAN so SDL_Vulkan_CreateSurface can bind to it. On
    // Windows the backend is D3D12 via the HWND, so the flag is omitted.
    // ROGUESQ_HIDE_WINDOW=1: create the window hidden so headless runs are a true background
    // process (no visible window). The HWND/surface is still valid, so RT64 keeps rendering and
    // presenting and the VI-paced loop proceeds; only on-screen display + window screenshots are lost.
    const bool hide_window = env_on("ROGUESQ_HIDE_WINDOW");
    Uint32 window_flags = SDL_WINDOW_RESIZABLE | (hide_window ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    if (env_on("ROGUESQ_MAXIMIZED"))
        window_flags |= SDL_WINDOW_MAXIMIZED;
#ifndef _WIN32
    window_flags |= SDL_WINDOW_VULKAN;
#endif
    int window_w = 640, window_h = 480;
    if (const char* ws = recomp::dbg::env_str("ROGUESQ_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(ws, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) {
            window_w = w;
            window_h = h;
        }
    }
    SDL_Window* sdl_window = SDL_CreateWindow(
        "Star Wars: Rogue Squadron 64 Recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        window_w, window_h,
        window_flags
    );
    if (!sdl_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    extern SDL_Window* g_sdl_window; g_sdl_window = sdl_window;
#if defined(_WIN32)
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(sdl_window, &wm);
    return { wm.info.win.window };
#else
    return sdl_window;
#endif
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // Window messages only dispatch on the thread that owns the window, so events (and RT64's F1-F4 filter) must be pumped here.
    SDL_PumpEvents();

    // Build the menu config on the main thread; doing it lazily from a game-thread menu hook races menu-audio init.
    { static bool s_cfg = false;
      if (!s_cfg) { s_cfg = true; rs64_menu_config_init(); } }

    // Capture the mouse for steering while focused, unless the F6 controls window or the F1 inspector is open.
    // Drain the relative-motion accumulator on each transition so enabling doesn't jump.
    bool want = (SDL_GetKeyboardFocus() != nullptr)
             && !g_show_controls.load()
             && !rs64_rt64_inspector_open();
    g_mouse_capture.store(want);
    static bool s_rel = false;
    if (want != s_rel) {
        SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
        SDL_GetRelativeMouseState(nullptr, nullptr);
        s_rel = want;
    }
}

// ---------------------------------------------------------------------------
// Per-VI frame-barrier signal (step 1 of the frame-pacing root fix)
// ---------------------------------------------------------------------------
// The game's per-frame gfx barrier — submitGfxFrame's osRecvMesg on
// D_8011A7E8 (0x8011A7E8) and D_8011A818 (0x8011A818) — was BLOCK→NOBLOCK
// patched to dodge a boot deadlock, which removed the loop's 60Hz pacing (see
// memory: frame-pacing root cause). This callback fires once per HOST VI
// retrace (60Hz, from the VI thread) and posts a token to those queues, so the
// barrier always has a frame token available at the true display cadence.
//
// While the recvs are still NOBLOCK this is purely additive and observable only
// as "no regression": extra tokens fill the size-1 queues then drop
// (requeue_if_blocked=false), and do_send treats an uninitialized queue as full
// and no-ops — so it's safe even before the game creates these queues. It
// primes the barriers so the NOBLOCK instruction patches can later be reverted
// (step 2) and the game loop will pace to the real VI instead of running free.
// Disable with ROGUESQ_VI_BARRIER_SIGNAL=0.
static void rs64_vi_callback() {
    ++g_vi_tick;
    if (rs64_vi_driven()) return;   // hardware protocol: no host-injected tokens (they double-signal size-1 queues)
    --g_vi_tick;
    static int s_on = -1;
    if (s_on < 0) s_on = env_on("ROGUESQ_VI_BARRIER_SIGNAL", true);
    // Tick for rs64_attrib_wait_vi (attribution loop paces to the real VI).
    ++g_vi_tick;
    if (!s_on) {
        return;
    }
    ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x8011A7E8u, (OSMesg)0, false, false);
    ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x8011A818u, (OSMesg)0, false, false);
    // [FRAME-PACING] Host-signal viRetraceHandlerThread's VI queue (0x80114388) per VI. Its osViSetEvent
    // registration isn't delivering retrace messages during the cinematic (is_game_started gate / latch),
    // so the buffer-drain consumer sleeps -> cinematic video buffers never return to free -> the
    // bufferArbiterProducerScanWait drain-barrier hangs (boot "freeze" at iter ~903). Posting the
    // registered message (0x8011A4CC) every VI wakes it to drain buffers. ROGUESQ_VI_RETRACE_SIGNAL=0 to disable.
    static int s_vr = -1;
    if (s_vr < 0) s_vr = env_on("ROGUESQ_VI_RETRACE_SIGNAL", true);
    if (s_vr) {
        ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80114388u, (OSMesg)0x8011A4CCu, false, false);
    }
    // [FRAME-PACING] Host-signal the frame-sync barrier queue (0x80128D10) per VI. The cinematic
    // producer (bufferArbiterProducerScanWait → awaitFrameSyncMesgBlock) BLOCKs here waiting for a
    // frame-sync token that signalFrameSyncMesgNonblock would post — but that signaller isn't
    // firing during the cinematic, so the producer hangs (watchdog: cinematicLoopBody freeze at
    // iter ~891). Posting a token each VI gives it a 60Hz frame-sync pulse so it proceeds (the
    // recv discards the message value). ROGUESQ_VI_FRAMESYNC_SIGNAL=0 to disable.
    static int s_fs = -1;
    if (s_fs < 0) s_fs = env_on("ROGUESQ_VI_FRAMESYNC_SIGNAL", true);
    if (s_fs) {
        ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80128D10u, (OSMesg)0, false, false);
    }
    // [FRAME-PACING] Host-signal the video-buffer drain queue (0x80128CF0) per VI. Near the END of
    // the cinematic the producer blocks in waitOnVideoQueue (funcs_5.c:5421, osRecvMesg BLOCK on
    // 0x80128CF0) waiting for a video buffer to return to free; bufferArbiterProducerScanWait
    // loops there forever (watchdog: cinematicLoopBody freeze at iter ~1118). The retrace consumer
    // isn't posting the free-token during the cinematic, so pulse it each VI like the sibling
    // barriers above. ROGUESQ_VI_VIDEOQ_SIGNAL=0 to disable.
    // Default OFF: unproven and correlated with the mode-2 framebuffer-flood crash in testing
    // (poking the producer to not wait for buffers seems to let it submit garbage-CIMG DLs).
    // The op_b5 DL-walk bound (rt64_gbi_f3dfactor5.cpp) is what stabilized the cinematic; this
    // signal is opt-in for further frame-pacing experiments. ROGUESQ_VI_VIDEOQ_SIGNAL=1 to enable.
    static int s_vq = -1;
    if (s_vq < 0) s_vq = env_on("ROGUESQ_VI_VIDEOQ_SIGNAL");
    if (s_vq) {
        ultramodern::enqueue_external_message((PTR(OSMesgQueue))0x80128CF0u, (OSMesg)0, false, false);
    }
}

// ---------------------------------------------------------------------------
// Thread naming
// ---------------------------------------------------------------------------
static std::string get_game_thread_name(const OSThread* t) {
    switch (t->id) {
    case 1:  return "[Game] IDLE";
    case 3:  return "[Game] MAIN";
    case 4:  return "[Game] EEPROM";  // entry func_8006F2CC: save writer (osEepromLongWrite), not GRAPH
    case 5:  return "[Game] SCHED";
    case 10: return "[Game] AUDIOMGR";
    case 18: return "[Game] DMAMGR";
    default: return "[Game] " + std::to_string(t->id);
    }
}

// ---------------------------------------------------------------------------
// Game registration
// ---------------------------------------------------------------------------
// ROM hash: xxHash3-64 of rogue_squadron.z64 (USA v1.0, 16MB)
static constexpr uint64_t RS64_ROM_HASH = 0x6B66A44153594DEAULL;

std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash             = RS64_ROM_HASH,
        .internal_name        = "ROGUE SQUADRON",
        .game_id              = u8"rs64.n64.us.1.0",
        .mod_game_id          = "rs64",
        .save_type            = recomp::SaveType::Eep4k,
        .is_enabled           = true,
        .entrypoint_address   = get_entrypoint_address(),
        .entrypoint           = rs64_entrypoint_with_rdram_capture,
    },
};

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
// One-shot init of DbgHelp symbol resolution — done lazily on first crash.
#ifdef _WIN32
static void ensure_dbghelp_init() {
    static bool init = false;
    if (init) return;
    init = true;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

// Print stack frames with symbol resolution. Each frame becomes:
//   [ N] 0xADDR  module!function+0xOFF  (file:line)
void print_stack_with_symbols(void** frames, USHORT count) {
    ensure_dbghelp_init();
    HANDLE proc = GetCurrentProcess();
    HMODULE exe_base = GetModuleHandleW(nullptr);
    constexpr DWORD kNameMax = 512;
    char buf[sizeof(SYMBOL_INFO) + kNameMax];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (USHORT i = 0; i < count; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)exe_base;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = kNameMax - 1;
        DWORD64 disp = 0;
        const char* name = "?";
        if (SymFromAddr(proc, addr, &disp, sym)) {
            name = sym->Name;
        }
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
            fprintf(stderr, "  [%2u] 0x%llX rva 0x%llX  %s+0x%llX  (%s:%lu)\n",
                (unsigned)i, (unsigned long long)addr, (unsigned long long)rva,
                name, (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
        } else {
            fprintf(stderr, "  [%2u] 0x%llX rva 0x%llX  %s+0x%llX\n",
                (unsigned)i, (unsigned long long)addr, (unsigned long long)rva,
                name, (unsigned long long)disp);
        }
    }
}

// Full-memory minidump so we can inspect rdram contents post-mortem.
// ep may be null (SIGABRT path) — we still capture process+thread state.
static void write_minidump_safe(EXCEPTION_POINTERS* ep) {
    char path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryA("dumps", NULL);
    CreateDirectoryA("dumps/crash-dumps", NULL);
    snprintf(path, sizeof(path),
        "dumps/crash-dumps/crash_%04u%02u%02u_%02u%02u%02u.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[CRASH] CreateFile(%s) failed err=%lu\n",
            path, GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    PMINIDUMP_EXCEPTION_INFORMATION pmei = nullptr;
    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        pmei = &mei;
    }
    // Default to a lighter dump type. The earlier MiniDumpWithFullMemory
    // captured the full process address space (≈5 GB on this app, dominated
    // by D3D12/Vulkan render targets and texture caches). The symbolicated
    // stack trace in the .log already covers the common debug case; the
    // dump only needs threads + stacks + indirectly-referenced memory.
    // Set ROGUESQ_FULL_DUMP=1 to restore the full 5 GB dump for deep-dives.
    static const bool s_full_dump = env_on("ROGUESQ_FULL_DUMP");
    MINIDUMP_TYPE dumpType = s_full_dump
        ? MiniDumpWithFullMemory
        : (MINIDUMP_TYPE)(MiniDumpNormal
                          | MiniDumpWithIndirectlyReferencedMemory
                          | MiniDumpWithDataSegs
                          | MiniDumpWithThreadInfo
                          | MiniDumpWithUnloadedModules);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
        hFile, dumpType, pmei, NULL, NULL);
    CloseHandle(hFile);
    if (ok) {
        fprintf(stderr, "[CRASH] Minidump written: %s%s\n",
            path, s_full_dump ? " (full memory)" : " (lite)");
    } else {
        fprintf(stderr, "[CRASH] MiniDumpWriteDump failed err=%lu\n", GetLastError());
    }
    fflush(stderr);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "[CRASH] Exception 0x%08lX at 0x%llX\n", code, (unsigned long long)addr);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "[CRASH] Access violation %s address 0x%llX\n",
            ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    // Capture stack BEFORE minidump — if the process is killed mid-minidump
    // (e.g. by an external watchdog), we still want the trace in stderr.
    void* frames[32];
    USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    fprintf(stderr, "[CRASH] Stack trace (%u frames):\n", (unsigned)count);
    print_stack_with_symbols(frames, count);
    fflush(stderr);
    write_minidump_safe(ep);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif // _WIN32

// Set an env var so the getenv-based knobs below pick it up. The whole program
// is configured through the environment, not argv, so CLI args are translated here.
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// User-facing runtime flags. Each maps a --flag to a ROGUESQ_* env var that the
// getenv-based knobs elsewhere read. The full debug/trace catalog stays env-only
// (docs/debug-trace-env-vars.md); reach any of those with --set NAME=VALUE.
struct CliFlag {
    const char* flag;   // long name without leading "--"
    const char* env;    // target ROGUESQ_* variable
    enum Kind { Bool, Value } kind;
    const char* on;     // Bool: value for --flag; Value: unused
    const char* off;    // Bool: value for --no-flag; Value: unused
    const char* help;
};
static const CliFlag kCliFlags[] = {
    {"gfx-api",          "ROGUESQ_GFX_API",          CliFlag::Value, "",  "",  "graphics API: vulkan | d3d12 (default auto)"},
    {"hle-dev-mode",     "ROGUESQ_HLE_DEV_MODE",     CliFlag::Bool,  "1", "0", "RT64 ImGui inspector on F1 (default on in Debug)"},
    {"vi-driven-loop",   "ROGUESQ_VI_DRIVEN_LOOP",   CliFlag::Bool,  "1", "0", "hardware VI/SP/DP frame protocol (default on); --no- restores host-paced loop"},
    {"f5-native",        "ROGUESQ_F5_NATIVE",        CliFlag::Bool,  "1", "0", "emit F5 geometry (default on); --no- parses without emitting"},
    {"audio-ucode",      "ROGUESQ_NO_AUDIO_UCODE",   CliFlag::Bool,  "0", "1", "MusyX synth (default); --no-audio-ucode uses the silent stub"},
    {"audio-gain",       "ROGUESQ_AUDIO_GAIN",       CliFlag::Value, "",  "",  "master gain multiplier (e.g. 0.35; 0 = silent)"},
    {"audio-latency-ms", "ROGUESQ_AUDIO_LATENCY_MS", CliFlag::Value, "",  "",  "audio buffer latency in milliseconds"},
    {"dump-pcm",         "ROGUESQ_DUMP_PCM",         CliFlag::Value, "",  "",  "write synth output to a 22050 Hz stereo WAV at <path>"},
    {"render-song",      "ROGUESQ_RENDER_SONG",      CliFlag::Value, "",  "",  "force a specific song key (0 = N64-logo music)"},
    {"fake-controller",  "ROGUESQ_FAKE_CONTROLLER",  CliFlag::Bool,  "1", "0", "fake a connected controller (headless runs)"},
    {"auto-start",       "ROGUESQ_AUTO_START",       CliFlag::Value, "",  "",  "pulse START after <ms> (headless runs)"},
    {"hide-window",      "ROGUESQ_HIDE_WINDOW",      CliFlag::Bool,  "1", "0", "create the window hidden (background process; no display/screenshots)"},
    {"maximized",        "ROGUESQ_MAXIMIZED",        CliFlag::Bool,  "1", "0", "start with the window maximized"},
    {"window-size",      "ROGUESQ_WINDOW_SIZE",      CliFlag::Value, "",  "",  "initial window client size WxH (e.g. 1280x720)"},
    {"widescreen",       "ROGUESQ_WIDESCREEN",       CliFlag::Bool,  "1", "0", "expand the aspect ratio to fill the window"},
    {"draw-distance",    "ROGUESQ_DRAW_DIST",        CliFlag::Value, "",  "",  "draw distance multiplier (e.g. 1.3; terrain and fog capped at 2.5)"},    {"boot-target",      "ROGUESQ_BOOT_TARGET",      CliFlag::Value, "",  "",  "skip the intro to a target: menu | demo:N (N=0-5)"},
};

static void print_cli_usage() {
    fprintf(stderr, "Usage: RogueSquadron64Recomp [options]\n\nOptions:\n");
    fprintf(stderr, "  -m, --mute                 silence output (master gain 0)\n");
    for (const CliFlag& f : kCliFlags) {
        char name[64];
        if (f.kind == CliFlag::Bool)
            snprintf(name, sizeof(name), "--[no-]%s", f.flag);
        else
            snprintf(name, sizeof(name), "--%s <value>", f.flag);
        fprintf(stderr, "  %-26s %s\n", name, f.help);
    }
    fprintf(stderr, "  --set NAME=VALUE           set any ROGUESQ_* variable directly\n");
    fprintf(stderr, "  -h, --help                 show this help and exit\n");
    fprintf(stderr,
        "\nEvery option maps to a ROGUESQ_* environment variable; NAME=VALUE (bare) works too.\n"
        "Full debug/trace variable list: docs/debug-trace-env-vars.md\n");
}

// Translate argv into the ROGUESQ_* env vars the rest of main reads. Returns an
// exit code to stop startup (0 for --help, 2 for a bad option), or -1 to continue.
static int apply_cli_args(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_cli_usage();
            return 0;
        }
        if (!strcmp(a, "--mute") || !strcmp(a, "-m")) {
            set_env("ROGUESQ_AUDIO_GAIN", "0");
            continue;
        }
        if (!strcmp(a, "--set")) {
            const char* kv = (i + 1 < argc) ? argv[++i] : nullptr;
            const char* eq = kv ? strchr(kv, '=') : nullptr;
            if (!eq) {
                fprintf(stderr, "error: --set expects NAME=VALUE\n");
                return 2;
            }
            std::string name(kv, eq - kv);
            set_env(name.c_str(), eq + 1);
            continue;
        }
        // Long flag: --flag, --flag=value, or --no-flag.
        if (a[0] == '-' && a[1] == '-') {
            const char* body = a + 2;
            const char* eq = strchr(body, '=');
            std::string name = eq ? std::string(body, eq - body) : std::string(body);
            bool negated = false;
            if (name.rfind("no-", 0) == 0) { negated = true; name.erase(0, 3); }
            const CliFlag* found = nullptr;
            for (const CliFlag& f : kCliFlags) {
                if (name == f.flag) { found = &f; break; }
            }
            if (!found) {
                fprintf(stderr, "error: unknown option '%s' (try --help)\n", a);
                return 2;
            }
            if (found->kind == CliFlag::Bool) {
                set_env(found->env, negated ? found->off : found->on);
            } else {
                if (negated) {
                    fprintf(stderr, "error: '--no-%s' is not a toggle\n", found->flag);
                    return 2;
                }
                const char* val = eq ? eq + 1 : ((i + 1 < argc) ? argv[++i] : nullptr);
                if (!val) {
                    fprintf(stderr, "error: --%s expects a value\n", found->flag);
                    return 2;
                }
                set_env(found->env, val);
            }
            continue;
        }
        // Bare NAME=VALUE passthrough (scripts / any env var).
        if (const char* eq = strchr(a, '=')) {
            std::string name(a, eq - a);
            set_env(name.c_str(), eq + 1);
            continue;
        }
        fprintf(stderr, "error: unexpected argument '%s' (try --help)\n", a);
        return 2;
    }
    return -1;
}

int main(int argc, char* argv[]) {
    if (int rc = apply_cli_args(argc, argv); rc >= 0) {
        return rc;
    }

    // Input bindings: load roguesq_input.json next to the exe, or write the
    // defaults as an editable template if it's absent. ROGUESQ_INPUT_RESET=1
    // overwrites it with defaults.
    {
        std::string cfg = rs64::input::default_config_path();
        const char* reset = recomp::os::getenv("ROGUESQ_INPUT_RESET");
        bool force = reset && reset[0] && reset[0] != '0';
        if (force || !rs64::input::load_bindings(g_bindings, cfg)) {
            g_bindings = rs64::input::default_bindings();
            rs64::input::save_bindings(g_bindings, cfg);
            fprintf(stderr, "[input] wrote default bindings to %s\n", cfg.c_str());
        } else {
            fprintf(stderr, "[input] loaded bindings from %s\n", cfg.c_str());
        }
        fflush(stderr);
    }
    RT64::SetRenderHookImgui(&draw_dev_ui);

#ifdef _WIN32
    // Log PE base so we can resolve absolute addresses from SEH logs to RVAs
    // (RVA = abs_addr - pe_base). Logged once on startup; helps with the
    // cinematic-loop AV diagnostics.
    HMODULE hSelf = GetModuleHandleW(NULL);
    fprintf(stderr, "[main] PE base=%p\n", (void*)hSelf);
    fflush(stderr);
#endif

    // RT64's RT64_LOG_PRINTF macro (debug builds) writes to GlobalLogFile via
    // unchecked fprintf + fflush. Pristine RT64 expects RT64::Application::start
    // to open it (we bypass Application), so without redirection the pointer
    // stays NULL and any RT64 debug-log call asserts in the CRT.
    //
    // Routing to stderr works but RT64 calls these macros at high rate (every
    // fullSync), and the per-call fflush starves the SDL message pump → game
    // window goes "Not Responding". Send writes to NUL instead — same effect
    // as the fork's `if(false) fprintf` cruft, but stays in our repo.
    // GlobalLogFile only exists in RT64 debug builds (under !NDEBUG); in Release
    // the RT64_LOG_* macros are no-ops, so the redirect is both unneeded and uncompilable.
#ifndef NDEBUG
    if (FILE *nul = recomp::os::fopen("NUL", "w")) {
        RT64::GlobalLogFile = nul;
    } else {
        RT64::GlobalLogFile = stderr;  // last-resort fallback
    }
#endif

    start_state_poller();
    start_phase_poller();

#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
    // Pinpoints uncaught C++ throws (notably ultramodern::thread_terminated, which
    // must be caught in _thread_func/entrypoint but escapes on any thread lacking
    // that catch). Runs on the throwing thread before abort, stack not yet unwound,
    // so the symbolized trace names the escape site. Falls through to the dump + exit.
    std::set_terminate([]{
        // Write to a file too: the Release build is /SUBSYSTEM:WINDOWS, so stderr
        // is not attached to a console and a shell redirect captures nothing.
        CreateDirectoryA("dumps", NULL);
        CreateDirectoryA("dumps/crash-dumps", NULL);
        char path[MAX_PATH]; SYSTEMTIME st; GetLocalTime(&st);
        snprintf(path, sizeof(path), "dumps/crash-dumps/terminate_%04u%02u%02u_%02u%02u%02u.txt",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        FILE* tf = fopen(path, "w");
        uintptr_t modbase = (uintptr_t)GetModuleHandleW(NULL);
        char line[512];
        snprintf(line, sizeof(line),
            "[TERMINATE] tid=%lu is_game_thread=%d thread_self=0x%08X modbase=0x%p\n",
            GetCurrentThreadId(), (int)ultramodern::is_game_thread(),
            (uint32_t)ultramodern::this_thread(), (void*)modbase);
        fputs(line, stderr);
        if (tf) fputs(line, tf);
        const char* extype = "no in-flight exception";
        char exbuf[256] = {0};
        if (std::exception_ptr ep = std::current_exception()) {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) {
                snprintf(exbuf, sizeof(exbuf), "uncaught %s: %s", typeid(e).name(), e.what());
                extype = exbuf;
            }
            catch (...) { extype = "uncaught non-std exception"; }
        }
        snprintf(line, sizeof(line), "[TERMINATE] %s\n", extype);
        fputs(line, stderr);
        if (tf) fputs(line, tf);
        void* frames[62];
        USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
        fputs("[TERMINATE] stack RVAs:", stderr);
        if (tf) fputs("[TERMINATE] stack RVAs:", tf);
        for (USHORT i = 0; i < count; i++) {
            snprintf(line, sizeof(line), " 0x%llX",
                (unsigned long long)((uintptr_t)frames[i] - modbase));
            fputs(line, stderr);
            if (tf) fputs(line, tf);
        }
        fputs("\n", stderr);
        if (tf) { fputs("\n", tf); fflush(tf); fclose(tf); }
        print_stack_with_symbols(frames, count);  // symbolized to stderr when a PDB exists
        write_minidump_safe(nullptr);
        _Exit(3);
    });
    signal(SIGABRT, [](int){
        fprintf(stderr, "[ABORT] caught SIGABRT, dumping stack:\n");
        void* frames[32];
        USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
        print_stack_with_symbols(frames, count);
        // No EXCEPTION_POINTERS on the abort path — passing nullptr makes
        // VS open the dump without the "unhandled exception" dialog.
        write_minidump_safe(nullptr);
        _Exit(3);
    });
    // Route CRT debug asserts to stderr instead of the blocking dialog.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    // Hook fires BEFORE abort() runs — gives us a chance to dump the stack
    // for "vector subscript out of range" and similar STL bounds checks.
    _CrtSetReportHook([](int reportType, char* message, int*) -> int {
        fprintf(stderr, "[CRT_REPORT type=%d] %s\n", reportType,
            message ? message : "(null)");
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);
        for (USHORT i = 0; i < count; i++) {
            uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)base;
            fprintf(stderr, "  [%2u] 0x%llX  rva 0x%llX\n", (unsigned)i,
                (unsigned long long)(uintptr_t)frames[i],
                (unsigned long long)rva);
        }
        fflush(stderr);
        // Return 1 to SUPPRESS the abort. STL bounds-check assertions ("vector
        // subscript out of range") fire when a Factor5 ucode handler indexes
        // past a vector limit due to state we can't fully replicate yet. The
        // resulting abort kills the game even though continuing with whatever
        // garbage the out-of-bounds read returned often lets play continue.
        // Trade-off: occasional visual glitches over a hard crash. Print first.
        return 1;
    });
    // MSVC debug iterators call _invalid_parameter on bounds-check failure
    // (e.g. "vector subscript out of range"). Default handler aborts silently;
    // ours prints a stack trace first.
    _set_invalid_parameter_handler([](const wchar_t* expr, const wchar_t* func,
                                       const wchar_t* file, unsigned int line,
                                       uintptr_t) {
        fprintf(stderr, "[INVALID_PARAM] expr=%ls func=%ls file=%ls:%u\n",
            expr ? expr : L"(null)", func ? func : L"(null)",
            file ? file : L"(null)", line);
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HMODULE base = GetModuleHandleW(nullptr);
        for (USHORT i = 0; i < count; i++) {
            uintptr_t rva = (uintptr_t)frames[i] - (uintptr_t)base;
            fprintf(stderr, "  [%2u] 0x%llX  rva 0x%llX\n", (unsigned)i,
                (unsigned long long)(uintptr_t)frames[i],
                (unsigned long long)rva);
        }
        fflush(stderr);
        _Exit(4);
    });
#endif

    rs64_register_overlays();

    // Use the working directory as the config/data path (portable mode).
    recomp::register_config_path(std::filesystem::current_path());

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    // Check if the ROM is already stored; if not, try to import it from common filenames.
    recomp::check_all_stored_roms();
    std::u8string rs_game_id = supported_games[0].game_id;
    if (!recomp::is_rom_valid(rs_game_id)) {
        static const char* rom_candidates[] = {
            "rogue_squadron.z64",
            "rogue squadron.z64",
            "RogueSquadron.z64",
            "rs64.z64",
        };
        for (const char* name : rom_candidates) {
            std::filesystem::path p = std::filesystem::current_path() / name;
            auto result = recomp::select_rom(p, rs_game_id);
            if (result == recomp::RomValidationError::Good) {
                fprintf(stderr, "[ROM] Imported %s\n", name);
                break;
            }
        }
    }
    // Re-check after any import attempt so is_rom_valid reflects the new file.
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(rs_game_id)) {
        fprintf(stderr,
            "[ROM] Place your Rogue Squadron (USA v1.0) ROM named\n"
            "      'rogue_squadron.z64' next to the executable and restart.\n");
    }

    recomp::start(recomp::Configuration{
        .project_version = { 0, 1, 0 },
        .window_handle = {},
        .rsp_callbacks = {
            .get_rsp_microcode = get_rsp_microcode,
        },
        .renderer_callbacks = {
            .create_render_context = recomp::create_render_context,
        },
        .audio_callbacks = {
            .queue_samples        = queue_samples,
            .get_frames_remaining = get_frames_remaining,
            .set_frequency        = set_frequency,
        },
        .input_callbacks = {
            .poll_input                = poll_input,
            .get_input                 = get_n64_input,
            .set_rumble                = set_rumble,
            .get_connected_device_info = get_connected_device_info,
        },
        .gfx_callbacks = {
            .create_gfx    = create_gfx,
            .create_window = create_window,
            .update_gfx    = update_gfx,
        },
        .events_callbacks = {
            .vi_callback = rs64_vi_callback,
            .gfx_init_callback = []() {
                std::u8string game_id = u8"rs64.n64.us.1.0";
                if (recomp::is_rom_valid(game_id)) {
                    recomp::start_game(game_id);
                }
            },
        },
        .error_handling_callbacks = {
            .message_box = [](const char* msg) { fprintf(stderr, "[Error] %s\n", msg); },
        },
        .threads_callbacks = {
            .get_game_thread_name = get_game_thread_name,
        },
        .message_queue_control = {},
    });

    return EXIT_SUCCESS;
}
