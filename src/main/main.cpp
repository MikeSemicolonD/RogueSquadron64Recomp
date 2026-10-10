#include <cstdio>
#include <cstdlib>
#include "os_compat.h"
#include <cstring>
#include <csignal>
#include <vector>
#include <unordered_map>
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
#include "rumble.h"
#include "touch_input.h"
#include "touch_menu.h"
#include "touch_config.h"
#include "game_state.h"           // rs64_state_current_id
#include "debug_logs.h"
#include "host_api.h"
#include "main.h"                 // this file's own exports (fullscreen/quit hooks)
#include "upstream_compat.h"      // rs64_vi_driven
#include "hook_helpers.h"         // g_vi_tick, g_boot_pulse_start
#include "nav_sequencer.h"        // rs64_nav_consume, rs64_nav_tick

#include "rt64_render_context.h"  // recomp::create_render_context
#include "video_config.h"
#include "renderdoc_capture.h"
#include <mutex>

using recomp::dbg::env_on;
using recomp::dbg::env_int;

// Android keeps SDL's main -> SDL_main rename: SDLActivity calls SDL_main in libmain.so.
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif
#ifdef _WIN32
#define RS64_NULL_DEVICE "NUL"
#else
#define RS64_NULL_DEVICE "/dev/null"
#endif
#ifdef __ANDROID__
#include "android_host.h"
#endif
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
#ifdef __APPLE__
#include "SDL2/SDL_syswm.h"
#include "SDL2/SDL_metal.h"
#endif
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
// factor5_boot runs ahead of the MusyX synth.
extern RspExitReason factor5_boot (uint8_t* rdram, uint32_t ucode_addr);
extern RspExitReason musyx_audio  (uint8_t* rdram, uint32_t ucode_addr);
extern uint8_t dmem[];

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

// Mimics SP_BOOT (zero DMEM, DMA ucode_data to DMEM[0]) then runs the RSPRecomp'd MusyX synth; the PCM reaches SDL via osAiSetNextBuffer and queue_samples.
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
    // that the GFX task uses (rsp/factor5_boot_rsp.toml), and it initializes the DMEM
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
    // the audio thread keeps running.
    return RspExitReason::Broke;
}

// Catch-all stub for unrecognised task types. Returning Broke (instead of
// QUICK_EXIT'ing) avoids tearing down the process when a stale/uninitialised
// task struct gets dispatched — observed early in boot with type fields like
// 0x21E50ADF that don't match M_GFXTASK/M_AUDTASK.
static RspExitReason unknown_task_stub(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_GFXTASK:
        // Graphics tasks go to send_dl in events.cpp; this is never reached for them.
        return &unknown_task_stub;
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
            g_audtask_n = g_audtask_n + 1;
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
#ifdef __ANDROID__
// False from SDL_APP_WILLENTERBACKGROUND until SDL_APP_DIDENTERFOREGROUND. While false the game keeps running but its
// audio is dropped: SDL pauses the device, so queued samples would otherwise play seconds late on return.
static std::atomic<bool> g_android_foreground{true};
// Set on return to the foreground: the next buffer reopens the device, since Android's paused stream can resume holding stale sound.
static std::atomic<bool> g_android_audio_reopen{false};
#endif
static uint32_t audio_sample_rate = 48000;

static void open_audio_device(uint32_t freq);

static void set_frequency(uint32_t freq) {
    // Don't churn the device when the rate hasn't changed — each close/reopen
    // flushes the SDL queue and clicks. The game re-sets the same AI rate
    // repeatedly (observed 22050 set 3x), so this removes those pops.
    if (audio_device && freq == audio_sample_rate) {
        return;
    }
    open_audio_device(freq);
}

static void open_audio_device(uint32_t freq) {
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
#ifdef __ANDROID__
    if (!g_android_foreground.load()) {
        return;
    }
    static uint32_t s_probe_until = 0;
    if (g_android_audio_reopen.exchange(false)) {
        open_audio_device(audio_sample_rate);
        s_probe_until = SDL_GetTicks() + 10000;
        fprintf(stderr, "[Audio] reopened device after returning to the foreground\n");
    }
    // Measure what is still queued in SDL for a while after returning (a multi-second value would mean the backlog is on our side).
    if (audio_device && !SDL_TICKS_PASSED(SDL_GetTicks(), s_probe_until)) {
        static uint32_t s_last = 0;
        if (SDL_GetTicks() - s_last >= 1000) {
            s_last = SDL_GetTicks();
            fprintf(stderr, "[Audio] after resume: SDL queued %.1f ms\n", SDL_GetQueuedAudioSize(audio_device) / 4.0 * 1000.0 / audio_sample_rate);
        }
    }
#endif
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
extern "C" { volatile uint8_t* volatile g_recomp_rdram_for_wp_raw = nullptr; }

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
// Every open gamepad; `controller` is whichever one last produced input (Steam Input / DS4Windows can expose an idle virtual pad alongside the real one).
static std::vector<SDL_GameController*> g_pads;
// Set when a pad connects so the next lightbar tick re-sends the current color.
static std::atomic<bool> g_lightbar_resend{true};

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

// Raw joysticks (flight sticks, HOTAS throttles, pedals): every joystick SDL doesn't treat as a
// gamepad. g_joy_by_dev maps Bindings::joy_devices to the open handle. Both guarded by g_bindings_mtx.
struct OpenJoy {
    SDL_Joystick*  joy = nullptr;
    SDL_JoystickID id = -1;
    std::string    guid;
    int            ordinal = 0;
    SDL_Haptic*    haptic = nullptr;   // force-feedback sticks without SDL_JoystickRumble
};
static std::vector<OpenJoy>       g_open_joys;
static std::vector<SDL_Joystick*> g_joy_by_dev;

// Throttle lever position [0,1] from the last input poll, -1 when no throttle is bound or connected.
static std::atomic<float> g_throttle{-1.0f};

// Lever position the throttle hook would use without the lockstep harness; ROGUESQ_THROTTLE=<0..1> forces one (headless tests).
extern "C" float rs64_throttle_live(void) {
    static const float s_forced = []() { const char* e = recomp::dbg::env_str("ROGUESQ_THROTTLE"); return e ? (float)std::atof(e) : -1.0f; }();
    return s_forced >= 0.0f ? s_forced : g_throttle.load();
}

extern "C" float rs64_throttle_cruise_live(void) {
    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    return g_bindings.throttle_cruise;
}

// Per-craft speed hooks in rogue_squadron.toml call this just before the craft's current speed steps toward its
// target. Addresses are f32 game constants (base_addr 0 = unscaled, cruise_addr 0 = none); a nonzero f32 at
// skip_addr (a scripted boost) keeps the game's target.
extern "C" float rs64_throttle_hook(uint8_t* rdram, float target, uint32_t base_addr, uint32_t floor_addr, uint32_t cruise_addr, uint32_t cap_addr, uint32_t skip_addr) {
    const float p = rs64_ls_throttle(rs64_throttle_live());
    if (p < 0.0f) return target;
    auto f32 = [rdram](uint32_t addr) { float f; memcpy(&f, rdram + (addr - 0x80000000u), 4); return f; };
    if (skip_addr && f32(skip_addr) != 0.0f) return target;
    const float base = base_addr ? f32(base_addr) : 1.0f;
    const float min_speed = base * f32(floor_addr);
    const float max_speed = base * f32(cap_addr);
    const float cruise_speed = cruise_addr ? base * f32(cruise_addr) : -1.0f;
    if (!(max_speed > min_speed)) return target;
    const float cruise_at = rs64_ls_cruise(rs64_throttle_cruise_live());
    const float speed = rs64::input::throttle_speed(p, cruise_at, min_speed, cruise_speed, max_speed);
    if (recomp::dbg::log_throttle()) {
        static int n = 0;
        if ((++n % 60) == 1) fprintf(stderr, "[throttle] pos=%.2f range=%.3f/%.3f/%.3f game=%.3f -> %.3f\n", p, min_speed, cruise_speed, max_speed, target, speed);
    }
    return speed;
}

// Rumble Pak reported to the game (all 13 built-in effects). Set from rumble.enabled at startup or when it is
// turned on; never cleared, since the game only probes for the pak at mission start. Turning rumble off mutes the host output.
static std::atomic<bool> g_report_rumble_pak{false};

static void rebuild_joy_map_locked() {
    g_joy_by_dev.assign(g_bindings.joy_devices.size(), nullptr);
    for (const OpenJoy& oj : g_open_joys) {
        for (size_t d = 0; d < g_bindings.joy_devices.size(); ++d) {
            if (g_bindings.joy_devices[d].guid == oj.guid && g_bindings.joy_devices[d].ordinal == oj.ordinal) {
                g_joy_by_dev[d] = oj.joy;
            }
        }
    }
}

static void open_raw_joystick(int device_index) {
    if (SDL_IsGameController(device_index)) return;
    SDL_Joystick* j = SDL_JoystickOpen(device_index);
    if (!j) return;
    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    OpenJoy oj;
    oj.joy = j;
    oj.id = SDL_JoystickInstanceID(j);
    for (const OpenJoy& o : g_open_joys) {
        if (o.id == oj.id) return;
    }
    char guid[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof(guid));
    oj.guid = guid;
    for (const OpenJoy& o : g_open_joys) {
        if (o.guid == oj.guid) oj.ordinal++;
    }
    const char* name = SDL_JoystickName(j);
    const int axes = SDL_JoystickNumAxes(j), buttons = SDL_JoystickNumButtons(j), hats = SDL_JoystickNumHats(j);
    bool added = false;
    const int dev = rs64::input::find_or_add_joy_device(g_bindings, oj.guid, oj.ordinal, name ? name : "", &added);
    if (added && dev >= 0) {
        rs64::input::add_joystick_defaults(g_bindings, dev, axes, buttons, hats);
        rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
    }
    if (!SDL_JoystickHasRumble(j) && SDL_JoystickIsHaptic(j) == SDL_TRUE) {
        SDL_Haptic* h = SDL_HapticOpenFromJoystick(j);
        if (h && SDL_HapticRumbleSupported(h) == SDL_TRUE && SDL_HapticRumbleInit(h) == 0) {
            oj.haptic = h;
        } else if (h) {
            SDL_HapticClose(h);
        }
    }
    g_open_joys.push_back(oj);
    rebuild_joy_map_locked();
    fprintf(stderr, "[input] joystick J%d \"%s\" axes=%d buttons=%d hats=%d rumble=%s%s\n", dev + 1, name ? name : "?",
            axes, buttons, hats, SDL_JoystickHasRumble(j) ? "yes" : (oj.haptic ? "haptic" : "no"), added ? " (new: PC-style defaults bound)" : "");
    fflush(stderr);
}

static void close_raw_joystick(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    for (size_t i = 0; i < g_open_joys.size(); ++i) {
        if (g_open_joys[i].id != id) continue;
        if (g_open_joys[i].haptic) SDL_HapticClose(g_open_joys[i].haptic);
        SDL_JoystickClose(g_open_joys[i].joy);
        g_open_joys.erase(g_open_joys.begin() + (ptrdiff_t)i);
        rebuild_joy_map_locked();
        fprintf(stderr, "[input] joystick removed\n");
        fflush(stderr);
        return;
    }
}

static const char* gamepad_type_name(SDL_GameControllerType t) {
    switch (t) {
        case SDL_CONTROLLER_TYPE_XBOX360:                      return "Xbox 360/XInput";
        case SDL_CONTROLLER_TYPE_XBOXONE:                      return "Xbox One";
        case SDL_CONTROLLER_TYPE_PS3:                          return "PS3";
        case SDL_CONTROLLER_TYPE_PS4:                          return "PS4";
        case SDL_CONTROLLER_TYPE_PS5:                          return "PS5";
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:          return "Switch Pro";
        case SDL_CONTROLLER_TYPE_VIRTUAL:                      return "virtual";
        case SDL_CONTROLLER_TYPE_GOOGLE_STADIA:                return "Stadia";
        case SDL_CONTROLLER_TYPE_AMAZON_LUNA:                  return "Luna";
        case SDL_CONTROLLER_TYPE_NVIDIA_SHIELD:                return "Shield";
        default:                                               return "unknown";
    }
}

static void open_gamepad(int device_index) {
    SDL_GameController* pad = SDL_GameControllerOpen(device_index);
    if (!pad) {
        fprintf(stderr, "[input] gamepad %d open failed: %s\n", device_index, SDL_GetError());
        fflush(stderr);
        return;
    }
    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    for (SDL_GameController* p : g_pads) {
        if (p == pad) {
            SDL_GameControllerClose(pad);
            return;
        }
    }
    g_pads.push_back(pad);
    if (!controller) controller = pad;
    g_lightbar_resend.store(true);
    char guid[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(pad)), guid, sizeof(guid));
    const char* name = SDL_GameControllerName(pad);
    fprintf(stderr, "[input] gamepad \"%s\" type=%s vid=%04x pid=%04x guid=%s%s\n", name ? name : "?",
            gamepad_type_name(SDL_GameControllerGetType(pad)), SDL_GameControllerGetVendor(pad), SDL_GameControllerGetProduct(pad), guid,
            controller == pad ? " (active)" : "");
    fflush(stderr);
}

static void close_gamepad(SDL_JoystickID id) {
    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    for (size_t i = 0; i < g_pads.size(); ++i) {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[i])) != id) continue;
        if (controller == g_pads[i]) controller = nullptr;
        SDL_GameControllerClose(g_pads[i]);
        g_pads.erase(g_pads.begin() + (ptrdiff_t)i);
        if (!controller && !g_pads.empty()) controller = g_pads.front();
        fprintf(stderr, "[input] gamepad removed (%zu left)\n", g_pads.size());
        fflush(stderr);
        return;
    }
}

static bool gamepad_has_input(SDL_GameController* pad) {
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
        if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)b)) return true;
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a)
        if (std::abs((int)SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)a)) > 16000) return true;
    return false;
}

static void select_active_gamepad() {
    if (g_pads.size() < 2 || (controller && gamepad_has_input(controller))) return;
    for (SDL_GameController* p : g_pads) {
        if (p == controller || !gamepad_has_input(p)) continue;
        std::lock_guard<std::mutex> lk(g_bindings_mtx);
        controller = p;
        const char* name = SDL_GameControllerName(p);
        fprintf(stderr, "[input] active gamepad -> \"%s\"\n", name ? name : "?");
        fflush(stderr);
        return;
    }
}

struct LightRgb { float r, g, b; };

static LightRgb light_rgb(uint32_t c) {
    return { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f };
}

static LightRgb light_mix(LightRgb a, LightRgb b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

// DualShock 4 / DualSense lightbar: state colors crossfade; in a mission the color follows health, pulses when critical,
// flashes on hits, and strobes through the death spiral. Pads without an LED (XInput, Steam/DS4Windows virtual pads) are skipped.
static void lightbar_tick(const rs64::input::LightbarConfig& cfg, const char* st, float dt) {
    static LightRgb s_cur{};
    static bool     s_have = false;
    static float    s_last_health = -1.0f, s_flash = 0.0f, s_phase = 0.0f;
    static uint32_t s_sent = 0xFFFFFFFFu;
    static auto     s_sent_t = std::chrono::steady_clock::now();

    if (!cfg.enabled) {
        s_sent = 0xFFFFFFFFu;
        return;
    }
    s_phase = std::fmod(s_phase + dt, 60.0f);
    constexpr float TWO_PI = 6.2831853f;
    const bool in_menu = st && std::strncmp(st, "menu", 4) == 0;
    const bool in_mission = st && std::strcmp(st, "mission") == 0;

    LightRgb target = light_rgb(in_menu ? cfg.menu : cfg.cinematic);
    float bright = 1.0f;
    bool snap = false;
    if (in_mission) {
        const rs64::rumble::CraftStatus cs = rs64::rumble::craft_status((const uint8_t*)g_recomp_rdram_for_wp_raw);
        target = light_rgb(cfg.mission);
        if (cs.valid && cs.dead) {
            target = light_rgb(cfg.dead);
        } else if (cs.valid && cs.spiral) {
            target = light_rgb(cfg.death);
            bright = 0.15f + 0.85f * (0.5f + 0.5f * std::cos(s_phase * TWO_PI * 5.0f));
            snap = true;
        } else if (cs.valid && cfg.health) {
            const float h = cs.health;
            target = h >= 0.5f ? light_mix(light_rgb(cfg.damaged), light_rgb(cfg.mission), (h - 0.5f) * 2.0f)
                               : light_mix(light_rgb(cfg.critical), light_rgb(cfg.damaged), h * 2.0f);
            if (h < 0.25f) bright = 0.45f + 0.55f * (0.5f + 0.5f * std::cos(s_phase * TWO_PI * 1.4f));
        }
        if (cs.valid && cfg.hit_flash && !cs.dead && s_last_health >= 0.0f && cs.health < s_last_health - 0.005f) s_flash = 1.0f;
        s_last_health = cs.valid ? cs.health : -1.0f;
    } else {
        s_last_health = -1.0f;
        s_flash = 0.0f;
    }

    const float a = (snap || !s_have) ? 1.0f : 1.0f - std::exp(-dt / 0.12f);
    s_cur = light_mix(s_cur, target, a);
    s_have = true;
    LightRgb out = { s_cur.r * bright, s_cur.g * bright, s_cur.b * bright };
    if (s_flash > 0.0f) out = light_mix(out, light_rgb(cfg.hit), s_flash);
    s_flash = std::max(0.0f, s_flash - dt / 0.18f);

    auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    const uint32_t packed = (q(out.r) << 16) | (q(out.g) << 8) | q(out.b);
    const auto now = std::chrono::steady_clock::now();
    const bool resend = g_lightbar_resend.exchange(false);
    if (!resend && (packed == s_sent || now - s_sent_t < std::chrono::milliseconds(33))) return;
    s_sent = packed;
    s_sent_t = now;
    std::lock_guard<std::mutex> lk(g_bindings_mtx);
    for (SDL_GameController* p : g_pads) {
        if (SDL_GameControllerHasLED(p)) SDL_GameControllerSetLED(p, (Uint8)(packed >> 16), (Uint8)(packed >> 8), (Uint8)packed);
    }
}

// Converts the game's Rumble Pak motor pulses (plus the damage/death-spiral layers) into device rumble at ~60 Hz, and drives the lightbar.
static void rumble_thread_main() {
    bool prev_any = false;
    float haptic_last = 0.0f;
    auto haptic_t = std::chrono::steady_clock::now();
    auto tick_t = haptic_t;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        rs64::input::RumbleConfig cfg;
        rs64::input::LightbarConfig light;
        { std::lock_guard<std::mutex> lk(g_bindings_mtx);
          cfg = g_bindings.rumble;
          light = g_bindings.lightbar; }
        const char* st = rs64_state_current_id();
        const auto tick_now = std::chrono::steady_clock::now();
        lightbar_tick(light, st, std::chrono::duration<float>(tick_now - tick_t).count());
        tick_t = tick_now;
        const bool in_mission = st && std::strcmp(st, "mission") == 0;
        const rs64::rumble::Output out = rs64::rumble::tick(cfg, (const uint8_t*)g_recomp_rdram_for_wp_raw, in_mission);
        const bool any = out.lo > 0.0f || out.hi > 0.0f;
        if (!any && !prev_any) continue;
        prev_any = any;
        const Uint16 lo = (Uint16)(out.lo * 65535.0f), hi = (Uint16)(out.hi * 65535.0f);
        const Uint32 ms = any ? 100 : 0;
        const float mag = std::max(out.lo, out.hi);
        const auto now = std::chrono::steady_clock::now();
        const bool haptic_due = !any || std::fabs(mag - haptic_last) > 0.05f || now - haptic_t > std::chrono::milliseconds(100);
        std::lock_guard<std::mutex> lk(g_bindings_mtx);
        if (controller) SDL_GameControllerRumble(controller, lo, hi, ms);
        for (const OpenJoy& oj : g_open_joys) {
            if (!g_bindings.joystick_enabled) break;
            if (!oj.haptic) {
                SDL_JoystickRumble(oj.joy, lo, hi, ms);
                continue;
            }
            if (!haptic_due) continue;
            if (any) SDL_HapticRumblePlay(oj.haptic, mag, 150);
            else SDL_HapticRumbleStop(oj.haptic);
        }
        if (haptic_due) {
            haptic_last = any ? mag : 0.0f;
            haptic_t = now;
        }
    }
}

// Fullscreen: the roguesq_video.json setting, set/toggled from any thread (menu, UI); applied on the main thread
// in poll_input via SDL_SetWindowFullscreen (RT64 resizes its swapchain off the resulting resize event).
SDL_Window*                  g_sdl_window = nullptr;
static bool                  g_fullscreen_applied = false;

extern "C" void rs64_set_fullscreen(int on)   { rs64::video::set_fullscreen(on != 0); }
extern "C" int  rs64_get_fullscreen(void)     { return rs64::video::fullscreen() ? 1 : 0; }
extern "C" void rs64_toggle_fullscreen(void)  { rs64::video::set_fullscreen(!rs64::video::fullscreen()); }

// Touch engine: finger/sensor events arrive on the SDL pump thread, polls on the game thread.
static rs64::touch::Engine g_touch;
static std::mutex g_touch_mtx;
static rs64::touch::MenuSnapshot g_touch_menu_at_down{};
static SDL_Sensor* g_accel_sensor = nullptr;
static SDL_Sensor* g_gyro_sensor = nullptr;
// Set by the TOUCH LAYOUT menu action on the game thread; the next touch poll opens the editor.
static std::atomic<bool> g_touch_edit_request{false};

extern "C" void rs64_touch_layout_request(void) { g_touch_edit_request.store(true); }

static uint8_t* touch_rdram() {
    return (uint8_t*)g_recomp_rdram_for_wp_raw;
}

extern "C" int rs64_menu_page_shown(void);
namespace RT64 {
    extern std::atomic<float> presentShiftY;
}

static rs64::touch::Context touch_context() {
    uint8_t* r = touch_rdram();
    // A mod page is a list menu, though its title in host RAM leaves the classifier at "unknown".
    if (g_active_overlay == 1 && rs64_menu_page_shown()) {
        return rs64::touch::Context::ListMenu;
    }
    // The account menu (select game / level / craft) is carousel-like on every screen; its classifier sub-states are unreliable.
    if (r && g_active_overlay == 1 && rs64::touch::account_screen(r) != rs64::touch::AccountScreen::NotAccount) {
        return rs64::touch::Context::Carousel;
    }
    return rs64::touch::classify(g_active_overlay, rs64_state_current_id(), r && rs64::touch::read_paused(r), r && rs64::touch::read_demo(r));
}

static rs64::touch::AccountScreen touch_account_screen() {
    uint8_t* r = touch_rdram();
    if (!r || g_active_overlay != 1) {
        return rs64::touch::AccountScreen::NotAccount;
    }
    return rs64::touch::account_screen(r);
}

static bool touch_state_is(const char* want) {
    const char* id = rs64_state_current_id();
    return id && std::strcmp(id, want) == 0;
}

// Tap-to-select only where the resident menu data is what's on screen: list menus, Passcodes' ENTER CODE/BACK, SELECT GAME's ERASE GAME.
static bool touch_menu_hits_allowed() {
    return touch_context() == rs64::touch::Context::ListMenu || touch_state_is("menu.passcodes") ||
           touch_account_screen() == rs64::touch::AccountScreen::SelectGame;
}

// SOUND SETTINGS volume bar being edited (opened by a touch): touches on the bar set the value from the finger's x.
struct TouchSlider {
    bool active = false;
    int entry = -1;
    uint32_t opened_ms = 0;
    int64_t finger = -1;
    int last_value = -1;
    uint32_t close_ms = 0;   // a touch off the bar just closed it: that tap only closes the game's slider edit
};
static TouchSlider g_touch_slider;

static void window_size(float* w, float* h) {
    int iw = 0, ih = 0;
    if (g_sdl_window) {
        SDL_GetWindowSize(g_sdl_window, &iw, &ih);
    }
    *w = (float)iw;
    *h = (float)ih;
}

// Returns true if the finger event was on the open volume bar. The game steps 1 per held frame in its slider edit
// mode and applies the mixer itself, so write one step short of the target and inject one frame toward it.
static bool touch_slider_event(rs64::touch::FingerEvent ev, int64_t finger, float x, float y) {
    using rs64::touch::FingerEvent;
    TouchSlider& s = g_touch_slider;
    uint8_t* r = touch_rdram();
    float w, h;
    window_size(&w, &h);
    rs64::touch::SliderBar bar{};
    if (!s.active || !r || !touch_state_is("menu.sound_settings") || !rs64::touch::sound_slider(r, s.entry, w, h, &bar)) {
        s.active = false;
        return false;
    }
    if (ev == FingerEvent::Down) {
        if (y < bar.y0 || y > bar.y1) {
            // Off the bar: this touch closes the slider (an overlay B does it itself; a tap sends only A, see touch_menu_tap).
            s.active = false;
            s.close_ms = SDL_GetTicks();
            return false;
        }
        s.finger = finger;
        s.last_value = -1;
    }
    if (finger != s.finger) {
        return false;
    }
    if (ev == FingerEvent::Up) {
        s.finger = -1;
        return true;
    }
    // The bar fades in for about 1/3 s after opening and ignores input meanwhile.
    if (SDL_GetTicks() - s.opened_ms < 400) {
        return true;
    }
    const int target = rs64::touch::slider_value_at(bar, x);
    if (target != s.last_value) {
        s.last_value = target;
        if (target > 0) {
            rs64::touch::write_volume(r, bar.channel, (uint8_t)(target - 1));
            g_touch.inject(0, 1.0f, 0.0f, 1);
        } else {
            rs64::touch::write_volume(r, bar.channel, 1);
            g_touch.inject(0, -1.0f, 0.0f, 1);
        }
    }
    return true;
}

// Menu/carousel tap handling (called by the engine with g_touch_mtx held): per-screen targets, then menu entry hit-test.
static rs64::touch::TapAction touch_menu_tap(float x, float y) {
    using rs64::touch::TapAction;
    float w, h;
    window_size(&w, &h);
    uint8_t* r = touch_rdram();
    if (!r) {
        return TapAction::None;
    }
    // A tap on the picture slid up above the keyboard lands where that spot is drawn unshifted.
    y += RT64::presentShiftY.load();
    if (touch_context() == rs64::touch::Context::PauseMenu) {
        return rs64::touch::pause_tap_select(r, x, y, w, h) ? TapAction::Confirm : TapAction::None;
    }
    // PASSCODES: the engine steps on side touches itself; a tap on the wheel's center enters the letter.
    if (touch_context() == rs64::touch::Context::Wheel && rs64::touch::on_wheel(y)) {
        return TapAction::Confirm;
    }
    const rs64::touch::AccountScreen acct = touch_account_screen();
    if (acct == rs64::touch::AccountScreen::SelectGame) {
        const TapAction slot = rs64::touch::select_game_tap(x, y, w, h);
        if (slot != TapAction::None) {
            return slot;
        }
    } else if (acct == rs64::touch::AccountScreen::Levels) {
        return rs64::touch::level_select_tap(x, y);
    } else if (acct == rs64::touch::AccountScreen::Craft) {
        return rs64::touch::craft_select_tap(x, y);
    } else if (touch_context() == rs64::touch::Context::Carousel) {
        const TapAction arrow = rs64::touch::carousel_arrow_tap(x, y);
        if (arrow != TapAction::None) {
            return arrow;
        }
    }
    // The first tap after leaving an open volume bar only closes the game's slider edit (A), without moving the highlight.
    if (g_touch_slider.close_ms != 0 && SDL_GetTicks() - g_touch_slider.close_ms < 600) {
        g_touch_slider.close_ms = 0;
        return TapAction::Confirm;
    }
    g_touch_slider.close_ms = 0;
    g_touch_slider.active = false;
    if (!touch_menu_hits_allowed() || !rs64::touch::tap_select(r, g_touch_menu_at_down, x, y, w, h)) {
        return TapAction::None;
    }
    // Confirming a volume entry opens its bar; remember it so touches on the bar set the value.
    rs64::touch::MenuSnapshot snap{};
    std::vector<rs64::touch::Box> boxes;
    rs64::touch::SliderBar bar{};
    if (rs64::touch::read_menu(r, &snap, &boxes, w, h) && rs64::touch::sound_slider(r, snap.current, w, h, &bar)) {
        g_touch_slider.active = true;
        g_touch_slider.entry = snap.current;
        g_touch_slider.opened_ms = SDL_GetTicks();
        g_touch_slider.finger = -1;
    }
    return TapAction::Confirm;
}

// Pause volume bars: a touch on a bar highlights its row and sets the value from the finger's x while it drags.
// The pause slider steps by a frame-time-scaled amount, so write one short and inject one frame of right (lands within 1).
struct TouchPauseSlider {
    int64_t finger = -1;
    int channel = -1;
    int row = -1;
    int last_value = -1;
};
static TouchPauseSlider g_touch_pause_slider;

static bool touch_pause_slider_event(rs64::touch::FingerEvent ev, int64_t finger, float x, float y) {
    using rs64::touch::FingerEvent;
    TouchPauseSlider& s = g_touch_pause_slider;
    uint8_t* r = touch_rdram();
    if (!r || touch_context() != rs64::touch::Context::PauseMenu) {
        s.finger = -1;
        return false;
    }
    float w, h;
    window_size(&w, &h);
    std::vector<rs64::touch::PauseLine> lines;
    if (!rs64::touch::read_pause_lines(r, w, h, &lines)) {
        return false;
    }
    const rs64::touch::PauseLine* bar = nullptr;
    if (ev == FingerEvent::Down) {
        for (const rs64::touch::PauseLine& l : lines) {
            if (l.bar && l.channel >= 0 && x >= l.x0 - 0.03f && x <= l.x1 + 0.03f && y >= l.y0 - 0.02f && y <= l.y1 + 0.02f) {
                bar = &l;
            }
        }
        if (!bar) {
            return false;
        }
        s = { finger, bar->channel, bar->selectable, -1 };
        rs64::touch::pause_set_entry(r, s.row);
    } else {
        if (finger != s.finger) {
            return false;
        }
        if (ev == FingerEvent::Up) {
            s.finger = -1;
            return true;
        }
        for (const rs64::touch::PauseLine& l : lines) {
            if (l.bar && l.channel == s.channel) {
                bar = &l;
            }
        }
        if (!bar) {
            return true;
        }
    }
    const int target = rs64::touch::pause_slider_value(*bar, x);
    if (target != s.last_value) {
        s.last_value = target;
        rs64::touch::write_volume(r, s.channel, (uint8_t)(target > 0 ? target - 1 : 0));
        g_touch.inject(0, 1.0f, 0.0f, 1);
    }
    return true;
}

static void touch_event(const SDL_Event& e) {
    using rs64::touch::FingerEvent;
    const FingerEvent ev = (e.type == SDL_FINGERDOWN) ? FingerEvent::Down : (e.type == SDL_FINGERUP) ? FingerEvent::Up : FingerEvent::Move;
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    if (g_touch.editing()) {
        g_touch.finger((int64_t)e.tfinger.fingerId, ev, e.tfinger.x, e.tfinger.y, e.tfinger.timestamp);
        return;
    }
    if (touch_slider_event(ev, (int64_t)e.tfinger.fingerId, e.tfinger.x, e.tfinger.y)) {
        return;
    }
    if (touch_pause_slider_event(ev, (int64_t)e.tfinger.fingerId, e.tfinger.x, e.tfinger.y)) {
        return;
    }
    if (ev == FingerEvent::Down) {
        std::vector<rs64::touch::Box> unused;
        float w, h;
        window_size(&w, &h);
        if (uint8_t* r = touch_rdram()) {
            rs64::touch::read_menu(r, &g_touch_menu_at_down, &unused, w, h);
        }
    }
    g_touch.finger((int64_t)e.tfinger.fingerId, ev, e.tfinger.x, e.tfinger.y, e.tfinger.timestamp);
}

// Tilt steering: screen roll from gravity, low-passed so hand shake and linear acceleration don't jitter the turn.
static void accel_event(const SDL_Event& e) {
    static float s_roll = 0.0f;
    const bool flipped = SDL_GetDisplayOrientation(0) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    float roll = 0.0f;
    const bool valid = rs64::touch::tilt_angles(e.sensor.data[0], e.sensor.data[1], e.sensor.data[2], flipped, &roll);
    s_roll += (roll - s_roll) * 0.25f;
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    g_touch.tilt(s_roll, valid);
}

// Gyro rates for the steering assist and mouse-like up/down. Device axes are portrait-natural: the screen normal is device z (+ = counter-clockwise, so negate
// for clockwise), and tipping the top edge away turns about screen-right (device -y, flipped on the other landscape side).
static void gyro_event(const SDL_Event& e) {
    const bool flipped = SDL_GetDisplayOrientation(0) == SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    const float s = flipped ? -1.0f : 1.0f;
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    g_touch.gyro_rate(-e.sensor.data[2], s * e.sensor.data[1]);
}

static SDL_Sensor* open_sensor(SDL_SensorType type) {
    for (int i = 0; i < SDL_NumSensors(); ++i) {
        if (SDL_SensorGetDeviceType(i) == type) {
            return SDL_SensorOpen(i);
        }
    }
    return nullptr;
}

// Opens the accelerometer (tilt) and gyroscope (rate assist) while the GYRO STEERING toggle is on, closes them when off.
static void update_gyro_sensor() {
    const bool want = rs64::touch::gyro_enabled();
    if (want && !g_accel_sensor) {
        static bool s_warned = false;
        g_accel_sensor = open_sensor(SDL_SENSOR_ACCEL);
        g_gyro_sensor = open_sensor(SDL_SENSOR_GYRO);
        if ((!g_accel_sensor || !g_gyro_sensor) && !s_warned) {
            s_warned = true;
            fprintf(stderr, "[touch] accelerometer=%s gyroscope=%s\n", g_accel_sensor ? "yes" : "no", g_gyro_sensor ? "yes" : "no");
        }
    } else if (!want && g_accel_sensor) {
        SDL_SensorClose(g_accel_sensor);
        g_accel_sensor = nullptr;
        if (g_gyro_sensor) {
            SDL_SensorClose(g_gyro_sensor);
            g_gyro_sensor = nullptr;
        }
    }
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    rs64::touch::Config tc = g_touch.config();
    tc.gyro = want && g_accel_sensor;
    g_touch.set_config(tc);
    if (!g_gyro_sensor) {
        g_touch.gyro_rate(0.0f, 0.0f);
    }
}

static void apply_fullscreen_if_requested() {
#ifndef __ANDROID__
    const bool want = rs64::video::fullscreen();
    if (want != g_fullscreen_applied && g_sdl_window) {
        g_fullscreen_applied = want;
        SDL_SetWindowFullscreen(g_sdl_window, want ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
#endif
}

// While the lobby address is typed on a phone, slide the picture up just enough that the highlighted entry clears the on-screen keyboard.
static void update_keyboard_shift() {
#ifdef __ANDROID__
    float shift = 0.0f;
    uint8_t* r = touch_rdram();
    if (r && rs64::host::flag("text_entry")) {
        const float ime = rs64::android::ime_fraction();
        float w, h;
        window_size(&w, &h);
        rs64::touch::MenuSnapshot snap{};
        std::vector<rs64::touch::Box> boxes;
        if (ime > 0.0f && rs64::touch::read_menu(r, &snap, &boxes, w, h)) {
            for (const rs64::touch::Box& b : boxes) {
                if (b.entry == snap.current) {
                    shift = std::max(0.0f, b.y1 - (1.0f - ime - 0.03f));
                }
            }
        }
    }
    RT64::presentShiftY.store(shift);
#endif
}

static void poll_input() {
    apply_fullscreen_if_requested();
    update_keyboard_shift();
    static bool s_scanned = false;
    if (!s_scanned) {
        s_scanned = true;
        const int n = SDL_NumJoysticks();
        fprintf(stderr, "[input] %d device(s) at first poll\n", n);
        fflush(stderr);
        for (int i = 0; i < n; ++i) {
            if (SDL_IsGameController(i)) open_gamepad(i);
            else open_raw_joystick(i);
        }
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
            touch_event(e);
            continue;
        }
        if (e.type == SDL_SENSORUPDATE && g_gyro_sensor && e.sensor.which == SDL_SensorGetInstanceID(g_gyro_sensor)) {
            gyro_event(e);
            continue;
        }
        if (e.type == SDL_SENSORUPDATE && g_accel_sensor && e.sensor.which == SDL_SensorGetInstanceID(g_accel_sensor)) {
            accel_event(e);
            continue;
        }
        // Text entry (the lobby address): typed text and Backspace/Enter go to a key handler that takes them, not the game.
        if (e.type == SDL_TEXTINPUT && rs64::host::run_key_handlers(e.text.text, 0)) {
            continue;
        }
        if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_BACKSPACE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) &&
            rs64::host::run_key_handlers(nullptr, e.key.keysym.sym == SDLK_BACKSPACE ? '\b' : '\r')) {
            continue;
        }
        // Android's system back gesture/button arrives as AC_BACK and means B.
        if (e.type == SDL_KEYDOWN && e.key.repeat == 0 && e.key.keysym.scancode == SDL_SCANCODE_AC_BACK) {
            std::lock_guard<std::mutex> lk(g_touch_mtx);
            g_touch_slider.active = false;
            g_touch.back_pressed();
            continue;
        }
        // Any physical input hides the touch overlay until the next touch.
        if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_JOYBUTTONDOWN ||
            (e.type == SDL_CONTROLLERAXISMOTION && std::abs(e.caxis.value) > 16000)) {
            std::lock_guard<std::mutex> lk(g_touch_mtx);
            g_touch.physical_input();
        }
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
            rs64::host::run_quit_handlers();
            // _Exit, not exit: the recomp game threads and RT64 are still live, so
            // running the C++/atexit static-destructor table here throws (-> terminate,
            // the Release crash-on-close) or deadlocks (the hang). Terminate now; the OS
            // reclaims SDL/GPU/process resources. Matches the crash handlers' _Exit.
            _Exit(EXIT_SUCCESS);
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            open_gamepad(e.cdevice.which);
        }
        if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            close_gamepad(e.cdevice.which);
        }
        if (e.type == SDL_JOYDEVICEADDED) {
            open_raw_joystick(e.jdevice.which);
        }
        if (e.type == SDL_JOYDEVICEREMOVED) {
            close_raw_joystick(e.jdevice.which);
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
    select_active_gamepad();
    update_gyro_sensor();
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

static rs64::touch::Pad poll_touch() {
    float w, h;
    window_size(&w, &h);
    const rs64::touch::Context ctx = touch_context();
    float px0 = 0.0f, px1 = 1.0f;
    rs64::touch::picture_extent(touch_rdram(), g_active_overlay, w, h, &px0, &px1);
    std::lock_guard<std::mutex> lk(g_touch_mtx);
    g_touch.set_screen(w, h);
    g_touch.set_picture(px0, px1);
    if (g_touch_edit_request.exchange(false) && g_touch.config().enabled) {
        g_touch_slider.active = false;
        g_touch.begin_edit();
    }
    if (g_touch.take_layout_saved()) {
        rs64::touch::Config& shared = rs64::touch::shared_config();
        shared.layout = g_touch.config().layout;
        shared.stick_split = g_touch.config().stick_split;
        rs64::touch::save_config(shared, rs64::touch::config_path());
    }
    return g_touch.poll(ctx, SDL_GetTicks());
}

static bool get_n64_input_live(int controller_num, uint16_t* buttons, float* x, float* y) {
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
    // The BOOT_TARGET menu driver also drives with a real controller connected (it only reports input while its sequence runs).
    {
        uint16_t nb = 0; float nx = 0.f, ny = 0.f;
        if (rs64_nav_consume(&nb, &nx, &ny)) { *buttons = nb; *x = nx; *y = ny; return true; }
    }
    if (fake_controller_enabled()) {
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
      st.joys = g_joy_by_dev.data();
      st.joys_len = (int)g_joy_by_dev.size();
      active = rs64::input::resolve(g_bindings, st, &btn, x, y);
      g_throttle.store(rs64::input::throttle_position(g_bindings, st)); }

    if (!active) {
        // Touch counts as a connected controller (no "NO CONTROLLER" gate on a phone).
        if (g_touch.config().enabled) {
            rs64::touch::Pad tp = poll_touch();
            *buttons = tp.buttons | boot_start_pulse(); *x = tp.x; *y = tp.y;
            return true;
        }
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
    rs64::touch::merge(poll_touch(), &btn, x, y);
    *buttons = btn;
    return true;
}

// ROGUESQ_INPUT_RECORD=<file> records controller 0 from the first input poll after launch (menus included); ROGUESQ_INPUT_REPLAY=<file> plays it back from the same point.
// Lines after the header: "<ms> <vi> <poll> <buttons hex> <x> <y>". v4 (written now) has one line per poll; replay serves the recorded sample nearest the current wall-clock ms (within half a poll).
// v3 (older) was written on change and replays by wall-clock ms with the stick blended between close samples.
struct InputRec { uint32_t ms, vi, poll; uint16_t buttons; float x, y; };

static bool get_n64_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    static const char* s_rec_path = recomp::dbg::env_str("ROGUESQ_INPUT_RECORD");
    static const char* s_play_path = recomp::dbg::env_str("ROGUESQ_INPUT_REPLAY");
    static bool s_anchored = false;
    static uint32_t s_poll = 0;
    static unsigned s_vi0 = 0;
    static std::chrono::steady_clock::time_point s_t0;
    if ((controller_num == 0) && (s_rec_path || s_play_path)) {
        if (!s_anchored) {
            s_anchored = true;
            s_poll = 0;
            s_vi0 = g_vi_tick;
            s_t0 = std::chrono::steady_clock::now();
            fprintf(stderr, "[input-rec] first poll: %s\n", s_play_path ? "replaying" : "recording");
        }
    }
    const uint32_t vi = s_anchored ? (uint32_t)(g_vi_tick - s_vi0) : 0u;
    const uint32_t ms = s_anchored ? (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_t0).count() : 0u;
    if ((controller_num == 0) && s_play_path && s_anchored) {
        static std::vector<InputRec> s_recs;
        static size_t s_idx = 0;
        static bool s_loaded = false;
        static int s_version = 0;
        if (!s_loaded) {
            s_loaded = true;
            if (FILE* f = fopen(s_play_path, "r")) {
                char header[32] = {};
                if ((fscanf(f, "%31s v%d", header, &s_version) == 2) && (s_version >= 3)) {
                    InputRec r{};
                    unsigned b = 0;
                    while (fscanf(f, "%u %u %u %x %f %f", &r.ms, &r.vi, &r.poll, &b, &r.x, &r.y) == 6) {
                        r.buttons = (uint16_t)b;
                        s_recs.push_back(r);
                    }
                }
                fclose(f);
            }
            fprintf(stderr, "[input-rec] loaded %zu entries (v%d) from %s\n", s_recs.size(), s_version, s_play_path);
        }
        if ((s_version >= 4) && !s_recs.empty()) {
            // Every poll was recorded with its time, so serve the recorded sample nearest to now: at most half a poll off, and no lag builds up when the game polls at a slightly different rate.
            auto it = std::lower_bound(s_recs.begin() + (ptrdiff_t)s_idx, s_recs.end(), ms, [](const InputRec& r, uint32_t t) { return r.ms < t; });
            size_t j = (it == s_recs.end()) ? (s_recs.size() - 1) : (size_t)(it - s_recs.begin());
            if ((j > 0) && ((it == s_recs.end()) || ((s_recs[j].ms - ms) > (ms - s_recs[j - 1].ms)))) {
                j--;
            }
            s_idx = std::max(s_idx, j);
        }
        else {
            while ((s_idx + 1 < s_recs.size()) && (s_recs[s_idx + 1].ms <= ms)) {
                s_idx++;
            }
        }
        // Close the game once the recording has played out (the last entry is written when the recorded session ended).
        { static bool s_quit = false;
          if (!s_quit && !s_recs.empty() && (ms > s_recs.back().ms + 500u)) {
              s_quit = true;
              fprintf(stderr, "[input-rec] replay finished at ms=%u, closing\n", ms);
              rs64_menu_request_quit();
          } }
        const bool have = !s_recs.empty() && ((s_version >= 4) || (s_recs[s_idx].ms <= ms));
        *buttons = have ? s_recs[s_idx].buttons : 0;
        *x = have ? s_recs[s_idx].x : 0.0f;
        *y = have ? s_recs[s_idx].y : 0.0f;
        // v3 only: blend the stick between samples of continuous motion (mouse steering) so a poll that lands a few ms off still reads nearly the recorded value; held values and sudden changes still step.
        if (have && (s_version < 4) && (s_idx + 1 < s_recs.size())) {
            const InputRec& a = s_recs[s_idx];
            const InputRec& b = s_recs[s_idx + 1];
            if ((b.ms > a.ms) && (b.ms - a.ms <= 50u)) {
                const float t = (float)(ms - a.ms) / (float)(b.ms - a.ms);
                *x = a.x + (b.x - a.x) * t;
                *y = a.y + (b.y - a.y) * t;
            }
        }
        // Drift check: our VI/poll counts at this time vs the recorded ones.
        { static uint32_t s_last_log = 0;
          if (have && (ms >= s_last_log + 10000)) { s_last_log = ms; fprintf(stderr, "[input-rec] ms=%u vi=%u recorded_vi=%u poll=%u recorded_poll=%u\n", ms, vi, s_recs[s_idx].vi, s_poll, s_recs[s_idx].poll); } }
        s_poll++;
        return true;
    }
    const bool ok = get_n64_input_live(controller_num, buttons, x, y);
    if (controller_num == 0) {
        rs64::host::run_input_filters(0, buttons, x, y);
    }
    if ((controller_num == 0) && s_rec_path && s_anchored) {
        static FILE* s_file = [](const char* p) { FILE* f = fopen(p, "w"); if (f) fprintf(f, "rs64-input v4\n"); return f; }(s_rec_path);
        if (s_file) {
            fprintf(s_file, "%u %u %u %04X %.9g %.9g\n", ms, vi, s_poll, *buttons, *x, *y);
            fflush(s_file);
        }
        s_poll++;
    }
    return ok;
}

static void set_rumble(int controller_num, bool on) {
    rs64::rumble::motor(controller_num, on);
}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    bool any_joy = false;
    { std::lock_guard<std::mutex> lk(g_bindings_mtx);
      any_joy = g_bindings.joystick_enabled && !g_open_joys.empty(); }
    if (controller_num == 0 && (controller || any_joy || g_bindings.keyboard_enabled || fake_controller_enabled())) {
        const auto pak = g_report_rumble_pak.load() ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None;
        return { ultramodern::input::Device::Controller, pak };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// Controls / rebind window. Runs on the RT64 graphics thread via the ImGui hook
// (SetRenderHookImgui), between NewFrame and Render. All g_bindings mutation is
// under g_bindings_mtx since get_n64_input reads it on the game thread. Only
// active in developer mode (the inspector frame); F6 toggles visibility.
// Index into g_bindings.joy_devices for an open joystick, or -1.
static int joy_dev_locked(const OpenJoy& oj) {
    for (size_t d = 0; d < g_bindings.joy_devices.size(); ++d) {
        if (g_bindings.joy_devices[d].guid == oj.guid && g_bindings.joy_devices[d].ordinal == oj.ordinal) return (int)d;
    }
    return -1;
}

// After a profile reset, re-register connected joysticks so they get the PC-style defaults again.
static void rebind_open_joysticks_locked() {
    for (const OpenJoy& oj : g_open_joys) {
        bool added = false;
        const char* name = SDL_JoystickName(oj.joy);
        const int dev = rs64::input::find_or_add_joy_device(g_bindings, oj.guid, oj.ordinal, name ? name : "", &added);
        if (added && dev >= 0) {
            rs64::input::add_joystick_defaults(g_bindings, dev, SDL_JoystickNumAxes(oj.joy), SDL_JoystickNumButtons(oj.joy), SDL_JoystickNumHats(oj.joy));
        }
    }
    rebuild_joy_map_locked();
}

static void draw_controls_ui() {
    if (!g_show_controls.load()) return;
    namespace ri = rs64::input;

    static int capture_target = -1;                 // Target awaiting a new bind
    static bool capture_append = false;             // Add (keep existing binds) vs Rebind (replace)
    static bool capture_snap = false;               // joystick baseline not taken yet
    static uint8_t prev_keys[SDL_NUM_SCANCODES] = {0};
    static uint32_t prev_pad = 0, prev_mouse = 0;
    // Joystick state when the rebind started; a button/hat press or an axis moved >50% from here is captured.
    struct JoySnap { SDL_JoystickID id; std::vector<float> axes; std::vector<uint8_t> buttons, hats; };
    static std::vector<JoySnap> joy_snap;

    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
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
            rebind_open_joysticks_locked();
            rs64::input::save_bindings(g_bindings, rs64::input::default_config_path());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) g_show_controls.store(false);
        ImGui::Separator();
        ImGui::TextWrapped("Click Rebind (replace) or Add, then press a key, gamepad button, mouse button, joystick button or hat, or move a joystick axis. Esc cancels.");
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

        if (ImGui::CollapsingHeader("Joysticks / HOTAS")) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ImGui::Checkbox("Joystick input", &g_bindings.joystick_enabled);
            ImGui::SameLine(); ImGui::TextDisabled("(untick to ignore connected joysticks)");
            ImGui::SliderFloat("Analog stick range", &g_bindings.stick_range, 60.0f, 127.0f, "%.0f");
            ImGui::TextDisabled("Full gamepad/joystick deflection. The N64 stick reaches ~80 and the game saturates there.");
            if (g_bindings.joy_devices.empty()) ImGui::TextDisabled("No joysticks seen yet.");
            for (size_t d = 0; d < g_bindings.joy_devices.size(); ++d) {
                ri::JoyDevice& jd = g_bindings.joy_devices[d];
                SDL_Joystick* j = d < g_joy_by_dev.size() ? g_joy_by_dev[d] : nullptr;
                ImGui::PushID((int)d);
                ImGui::Text("J%d  %s%s", (int)d + 1, jd.name.c_str(), j ? "" : "  (disconnected)");
                ImGui::SliderFloat("Deadzone", &jd.deadzone, 0.0f, 0.5f, "%.2f");
                const int axes = j ? SDL_JoystickNumAxes(j) : 0;
                for (int a = 0; a < axes && a < 32; ++a) {
                    ImGui::PushID(a);
                    ri::RawState rs;
                    rs.joys = g_joy_by_dev.data();
                    rs.joys_len = (int)g_joy_by_dev.size();
                    const float v = ri::joy_axis(g_bindings, rs, (int)d, a);
                    char label[32];
                    snprintf(label, sizeof(label), "axis%d  %+.2f", a, v);
                    ImGui::ProgressBar((v + 1.0f) * 0.5f, ImVec2(220, 0), label);
                    ImGui::SameLine();
                    bool inv = (jd.invert_axes >> a) & 1u;
                    if (ImGui::Checkbox("Invert", &inv)) jd.invert_axes = inv ? (jd.invert_axes | (1u << a)) : (jd.invert_axes & ~(1u << a));
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            {
                const float tp = g_throttle.load();
                if (tp >= 0.0f) ImGui::ProgressBar(tp, ImVec2(220, 0), "Throttle");
                else ImGui::TextDisabled("Throttle: not bound (pull the lever back, click Rebind on Throttle, then push it forward).");
                ImGui::SliderFloat("Cruise at", &g_bindings.throttle_cruise, 0.05f, 0.95f, "%.2f");
                ImGui::TextDisabled("Lever back = slowest, forward = fastest; the \"Cruise at\" position gives the craft's normal speed.");
            }
        }

        if (ImGui::CollapsingHeader("Rumble")) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ri::RumbleConfig& r = g_bindings.rumble;
            static bool s_late_pak = false;
            if (ImGui::Checkbox("Rumble", &r.enabled) && r.enabled && !g_report_rumble_pak.exchange(true)) {
                s_late_pak = true;
                fprintf(stderr, "[rumble] Rumble Pak now reported; effects start with the next mission\n");
            }
            if (r.enabled && s_late_pak) {
                ImGui::SameLine();
                ImGui::TextDisabled("(starts with the next mission)");
            }
            ImGui::SliderFloat("Strength", &r.strength, 0.0f, 2.0f, "%.2f");
            ImGui::Checkbox("Scale hits by damage", &r.scale_hits_by_damage);
            ImGui::Checkbox("Sustain through death spiral", &r.sustain_death_spiral);
            ImGui::Checkbox("Hits", &r.hit); ImGui::SameLine();
            ImGui::Checkbox("Collisions", &r.collision); ImGui::SameLine();
            ImGui::Checkbox("Object collisions", &r.object_collision);
            ImGui::Checkbox("Terrain scrape", &r.terrain_scrape); ImGui::SameLine();
            ImGui::Checkbox("Weapons", &r.weapons); ImGui::SameLine();
            ImGui::Checkbox("Death spiral", &r.death_spiral); ImGui::SameLine();
            ImGui::Checkbox("Crash", &r.crash);
        }

        if (ImGui::CollapsingHeader("Lightbar (DualShock 4 / DualSense)")) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            ri::LightbarConfig& l = g_bindings.lightbar;
            ImGui::Checkbox("Lightbar", &l.enabled); ImGui::SameLine();
            ImGui::Checkbox("Follow health", &l.health); ImGui::SameLine();
            ImGui::Checkbox("Flash on hits", &l.hit_flash);
            auto color = [](const char* label, uint32_t& c) {
                float f[3] = { ((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f };
                if (ImGui::ColorEdit3(label, f, ImGuiColorEditFlags_NoInputs)) {
                    c = ((uint32_t)std::lround(f[0] * 255.0f) << 16) | ((uint32_t)std::lround(f[1] * 255.0f) << 8) | (uint32_t)std::lround(f[2] * 255.0f);
                }
            };
            color("Menus", l.menu); ImGui::SameLine();
            color("Cutscenes / demos", l.cinematic);
            color("Full health", l.mission); ImGui::SameLine();
            color("Damaged", l.damaged); ImGui::SameLine();
            color("Critical", l.critical);
            color("Hit flash", l.hit); ImGui::SameLine();
            color("Death spiral", l.death); ImGui::SameLine();
            color("Destroyed", l.dead);
            if (ImGui::Button("Default colors")) {
                const ri::LightbarConfig d;
                const bool en = l.enabled, hp = l.health, hf = l.hit_flash;
                l = d;
                l.enabled = en;
                l.health = hp;
                l.hit_flash = hf;
            }
            bool any_led = false;
            for (SDL_GameController* p : g_pads) any_led = any_led || SDL_GameControllerHasLED(p);
            if (!any_led) ImGui::TextDisabled("No connected pad has a controllable lightbar (Steam Input or DS4Windows hide it behind a virtual pad).");
        }
        ImGui::Separator();

        // Edge-detect a captured input while a rebind is pending.
        if (capture_target >= 0 && capture_snap) {
            std::lock_guard<std::mutex> lk(g_bindings_mtx);
            joy_snap.clear();
            for (const OpenJoy& oj : g_open_joys) {
                JoySnap s{ oj.id, {}, {}, {} };
                for (int a = 0; a < SDL_JoystickNumAxes(oj.joy); ++a) s.axes.push_back(SDL_JoystickGetAxis(oj.joy, a) / 32767.0f);
                for (int b = 0; b < SDL_JoystickNumButtons(oj.joy); ++b) s.buttons.push_back(SDL_JoystickGetButton(oj.joy, b));
                for (int h = 0; h < SDL_JoystickNumHats(oj.joy); ++h) s.hats.push_back(SDL_JoystickGetHat(oj.joy, h));
                joy_snap.push_back(std::move(s));
            }
            capture_snap = false;
        } else if (capture_target >= 0) {
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
                std::lock_guard<std::mutex> lk(g_bindings_mtx);
                for (const OpenJoy& oj : g_open_joys) {
                    if (have) break;
                    const int dev = joy_dev_locked(oj);
                    const JoySnap* s = nullptr;
                    for (const JoySnap& js : joy_snap) {
                        if (js.id == oj.id) s = &js;
                    }
                    if (dev < 0 || !s) continue;
                    for (int b = 0; b < (int)s->buttons.size() && !have; ++b) {
                        if (SDL_JoystickGetButton(oj.joy, b) && !s->buttons[b]) { got = { ri::SourceKind::JoyButton, b, 0, (int8_t)dev }; have = true; }
                    }
                    for (int h = 0; h < (int)s->hats.size() && !have; ++h) {
                        const uint8_t now_bits = SDL_JoystickGetHat(oj.joy, h) & (uint8_t)~s->hats[h];
                        for (int bit = 0; bit < 4 && !have; ++bit) {
                            if (now_bits & (1u << bit)) { got = { ri::SourceKind::JoyHat, h, (int8_t)(1 << bit), (int8_t)dev }; have = true; }
                        }
                    }
                    for (int a = 0; a < (int)s->axes.size() && !have; ++a) {
                        const float delta = SDL_JoystickGetAxis(oj.joy, a) / 32767.0f - s->axes[a];
                        if (std::fabs(delta) < 0.5f) continue;
                        int dir = delta > 0.0f ? 1 : -1;
                        if (a < 32 && ((g_bindings.joy_devices[dev].invert_axes >> a) & 1u)) dir = -dir;
                        got = { ri::SourceKind::JoyAxis, a, (int8_t)dir, (int8_t)dev };
                        have = true;
                    }
                }
                if (have) {
                    if (!capture_append) g_bindings.targets[capture_target].clear();
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
                if (ImGui::SmallButton("Rebind")) {
                    capture_target = t;
                    capture_append = false;
                    capture_snap = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Add")) {
                    capture_target = t;
                    capture_append = true;
                    capture_snap = true;
                }
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
        // Code mods load once at game start; toggling one is saved but only applies on the next launch.
        static std::unordered_map<std::string, bool> s_loaded_state;
        for (const auto& d : mods) {
            ImGui::PushID(d.mod_id.c_str());
            bool enabled = recomp::mods::is_mod_enabled(d.mod_id);
            if (!d.runtime_toggleable) {
                s_loaded_state.try_emplace(d.mod_id, enabled);
            }
            if (ImGui::Checkbox("##enabled", &enabled)) {
                recomp::mods::enable_mod(d.mod_id, enabled);
                rs64_menu_config_mark_dirty();
            }
            ImGui::SameLine();
            const char* title = d.display_name.empty() ? d.mod_id.c_str() : d.display_name.c_str();
            auto loaded = s_loaded_state.find(d.mod_id);
            if (loaded != s_loaded_state.end() && loaded->second != enabled) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "(restart to apply)");
                ImGui::SameLine();
            }
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

static void draw_touch_text(ImDrawList* dl, const char* s, ImVec2 c, float size, ImU32 col) {
    ImFont* f = ImGui::GetFont();
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
    dl->AddText(f, size, { c.x - ts.x / 2, c.y - ts.y / 2 }, col, s);
}

// N64 controller glyphs (not action icons: controller presets remap what each button does).
static void draw_touch_glyph(ImDrawList* dl, const rs64::touch::ButtonDef& b, float W, float H, float op, bool pressed) {
    const std::string_view id = b.id ? b.id : "";
    const ImVec2 c{ b.cx * W, b.cy * H };
    const float r = b.r * H;
    const int a = (int)(op * (pressed ? 255 : 150));
    const ImU32 ink = IM_COL32(255, 255, 255, (int)(op * 255));
    const ImU32 ring = IM_COL32(255, 255, 255, (int)(op * (pressed ? 255 : 170)));
    auto color = [&](int rr, int gg, int bb) {
        const float k = pressed ? 1.35f : 1.0f;
        return IM_COL32(std::min(255, (int)(rr * k)), std::min(255, (int)(gg * k)), std::min(255, (int)(bb * k)), a);
    };
    if (id == "R") {
        const ImVec2 p0{ c.x - r * 1.3f, c.y - r * 0.7f };
        const ImVec2 p1{ c.x + r * 1.3f, c.y + r * 0.7f };
        dl->AddRectFilled(p0, p1, color(120, 120, 128), r * 0.7f);
        dl->AddRect(p0, p1, ring, r * 0.7f, 0, 2.0f);
        draw_touch_text(dl, "R", c, r * 1.1f, ink);
        return;
    }
    if (id == "CU" || id == "CD" || id == "CL" || id == "CR") {
        dl->AddCircleFilled(c, r, color(235, 190, 30), 32);
        dl->AddCircle(c, r, ring, 32, 2.0f);
        const float t = r * 0.55f;
        const ImU32 arrow = IM_COL32(60, 40, 0, (int)(op * 255));
        if (id == "CU") {
            dl->AddTriangleFilled({ c.x, c.y - t }, { c.x - t, c.y + t * 0.6f }, { c.x + t, c.y + t * 0.6f }, arrow);
        } else if (id == "CD") {
            dl->AddTriangleFilled({ c.x, c.y + t }, { c.x + t, c.y - t * 0.6f }, { c.x - t, c.y - t * 0.6f }, arrow);
        } else if (id == "CL") {
            dl->AddTriangleFilled({ c.x - t, c.y }, { c.x + t * 0.6f, c.y + t }, { c.x + t * 0.6f, c.y - t }, arrow);
        } else {
            dl->AddTriangleFilled({ c.x + t, c.y }, { c.x - t * 0.6f, c.y - t }, { c.x - t * 0.6f, c.y + t }, arrow);
        }
        return;
    }
    if (id == "GEAR") {
        dl->AddCircleFilled(c, r, IM_COL32(70, 70, 80, a), 32);
        for (int i = 0; i < 8; ++i) {
            const float ang = i * 3.14159265f / 4.0f;
            const ImVec2 d{ std::cos(ang), std::sin(ang) };
            dl->AddLine({ c.x + d.x * r * 0.35f, c.y + d.y * r * 0.35f }, { c.x + d.x * r * 0.75f, c.y + d.y * r * 0.75f }, ink, r * 0.22f);
        }
        dl->AddCircleFilled(c, r * 0.5f, ink, 24);
        dl->AddCircleFilled(c, r * 0.22f, IM_COL32(70, 70, 80, 255), 16);
        dl->AddCircle(c, r, ring, 32, 2.0f);
        return;
    }
    ImU32 fill = color(130, 130, 138);
    if (id == "A") {
        fill = color(50, 90, 220);
    } else if (id == "B" || id == "MENU_B") {
        fill = color(40, 160, 70);
    } else if (id == "START" || id == "MENU_START") {
        fill = color(200, 45, 45);
    }
    dl->AddCircleFilled(c, r, fill, 32);
    dl->AddCircle(c, r, ring, 32, 2.0f);
    const bool word = std::string_view(b.label).size() > 1;
    draw_touch_text(dl, b.label, c, word ? r * 0.55f : r * 1.1f, ink);
}

// Touch controls over the game (or the layout editor); with debug_hitboxes, the menu entries' tap boxes.
static void draw_touch_overlay() {
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x;
    const float H = io.DisplaySize.y;
    const rs64::touch::Context ctx = touch_context();
    rs64::touch::OverlayState ds;
    rs64::touch::Config cfg;
    {
        std::lock_guard<std::mutex> lk(g_touch_mtx);
        ds = g_touch.draw_state(ctx);
        cfg = g_touch.config();
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Pause-menu calibration: log the HUD's line elements (at most once a second, only when they change).
    if (cfg.debug_hitboxes && ctx == rs64::touch::Context::PauseMenu) {
        static std::string s_last_pause;
        static uint32_t s_last_ms = 0;
        uint8_t* r = touch_rdram();
        const uint32_t now = SDL_GetTicks();
        if (r && now - s_last_ms > 1000) {
            std::string d = rs64::touch::describe_pause(r);
            if (d != s_last_pause) {
                s_last_pause = d;
                s_last_ms = now;
                fprintf(stderr, "[touch] pause %s win=%.0fx%.0f\n", d.c_str(), W, H);
            }
        }
        std::vector<rs64::touch::PauseLine> lines;
        if (r && rs64::touch::read_pause_lines(r, W, H, &lines)) {
            for (const rs64::touch::PauseRow& row : rs64::touch::pause_rows(lines)) {
                dl->AddRect({ row.x0 * W, row.y0 * H }, { row.x1 * W, row.y1 * H }, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
            }
        }
    }
    if (cfg.debug_hitboxes && touch_menu_hits_allowed()) {
        std::vector<rs64::touch::Box> boxes;
        rs64::touch::MenuSnapshot snap{};
        uint8_t* r = touch_rdram();
        if (r) {
            static std::string s_last;
            std::string d = rs64::touch::describe_menu(r);
            if (d != s_last) {
                s_last = d;
                fprintf(stderr, "[touch] menu %s win=%.0fx%.0f\n", d.c_str(), W, H);
            }
        }
        if (r && rs64::touch::read_menu(r, &snap, &boxes, W, H)) {
            for (const rs64::touch::Box& b : boxes) {
                dl->AddRect({ b.x0 * W, b.y0 * H }, { b.x1 * W, b.y1 * H }, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
            }
        }
    }
    if (!ds.visible) {
        return;
    }
    const float op = ds.editing ? 1.0f : cfg.opacity;
    const ImU32 edge = IM_COL32(255, 255, 255, (int)(op * 255));
    if (ds.editing) {
        dl->AddRectFilled({ 0, 0 }, { W, H }, IM_COL32(0, 0, 0, 150));
        const float sx = ds.stick_split * W;
        for (float y = 0; y < H; y += 24.0f) {
            dl->AddLine({ sx, y }, { sx, y + 12.0f }, IM_COL32(255, 255, 255, 200), 3.0f);
        }
        draw_touch_text(dl, "STICK", { sx * 0.5f, H * 0.5f }, H * 0.05f, IM_COL32(255, 255, 255, 160));
        draw_touch_text(dl, "TOUCH LAYOUT: drag to move, pinch or +/- to resize", { W * 0.5f, H * 0.17f }, H * 0.035f, edge);
    }
    for (const rs64::touch::ButtonDef& b : ds.buttons) {
        draw_touch_glyph(dl, b, W, H, op, (ds.pressed & b.mask) != 0);
        if (ds.editing && b.mask == ds.selected) {
            dl->AddCircle({ b.cx * W, b.cy * H }, b.r * H + 6.0f, IM_COL32(255, 220, 40, 255), 48, 4.0f);
        }
    }
    for (const rs64::touch::ButtonDef& t : ds.tools) {
        const ImVec2 c{ t.cx * W, t.cy * H };
        const float r = t.r * H;
        const float hw = std::string_view(t.label).size() > 1 ? r * 1.9f : r;
        dl->AddRectFilled({ c.x - hw, c.y - r }, { c.x + hw, c.y + r }, IM_COL32(40, 60, 110, 230), r);
        dl->AddRect({ c.x - hw, c.y - r }, { c.x + hw, c.y + r }, edge, r, 0, 2.0f);
        draw_touch_text(dl, t.label, c, r * 0.9f, edge);
    }
    if (ds.stick_down) {
        dl->AddCircleFilled({ ds.base_x * W, ds.base_y * H }, ds.stick_r * H, IM_COL32(40, 40, 48, (int)(op * 110)), 48);
        dl->AddCircle({ ds.base_x * W, ds.base_y * H }, ds.stick_r * H, edge, 48, 2.0f);
        dl->AddCircleFilled({ ds.knob_x * W, ds.knob_y * H }, ds.stick_r * H * 0.4f, IM_COL32(90, 90, 100, (int)(op * 230)), 32);
        dl->AddCircle({ ds.knob_x * W, ds.knob_y * H }, ds.stick_r * H * 0.4f, edge, 32, 2.0f);
    }
}

// The single ImGui render hook draws the touch overlay and both dev panels.
static void draw_dev_ui() {
    draw_touch_overlay();
    draw_controls_ui();
    draw_mods_ui();
}

// ---------------------------------------------------------------------------
// Graphics (SDL2 window creation — rt64 takes over from here)
// ---------------------------------------------------------------------------
ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#ifdef __ANDROID__
    // Touches must not also arrive as mouse clicks (the mouse bindings would fire).
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    // Two threads pump SDL events (window thread + game input thread); block-on-pause releases only one of them on resume.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    // SDL otherwise lists the accelerometer as a joystick, and the raw-joystick bindings turn phone tilt into stick input on every screen.
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
#else
    // Desktop testing of the touch overlay: mouse clicks arrive as finger events.
    if (rs64::touch::shared_config().enabled) {
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    }
#endif
#ifndef NDEBUG
    // Debug builds: don't steal focus from the editor when the window first shows.
    SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_SENSOR) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    // Force-feedback sticks only; missing haptic support just means no rumble on them.
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0) {
        fprintf(stderr, "[input] haptic init failed (force-feedback sticks won't rumble): %s\n", SDL_GetError());
    }
#ifdef __ANDROID__
    // Runs synchronously on SDL's Java thread as the app backgrounds, before Android destroys the surface.
    SDL_AddEventWatch([](void*, SDL_Event* ev) -> int {
        if (ev->type == SDL_APP_WILLENTERBACKGROUND) {
            g_android_foreground.store(false);
            rs64_render_suspend_surface();
        } else if (ev->type == SDL_APP_DIDENTERFOREGROUND) {
            // Start from a fresh device so sound lines up with the picture again.
            if (audio_device) {
                SDL_ClearQueuedAudio(audio_device);
            }
            g_android_audio_reopen.store(true);
            g_android_foreground.store(true);
        }
        return 0;
    }, nullptr);
#endif
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
#if defined(__APPLE__)
    window_flags |= SDL_WINDOW_METAL;
#elif !defined(_WIN32)
    window_flags |= SDL_WINDOW_VULKAN;
#endif
#ifdef __ANDROID__
    // SDLActivity hides the system bars (immersive mode) only for a fullscreen window.
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#else
    if (!hide_window) {
        std::string cfg_path;
        if (char* base = SDL_GetBasePath()) { cfg_path = base; SDL_free(base); }
        if (recomp::dbg::env_str("ROGUESQ_FULLSCREEN")) {
            rs64::video::pin_fullscreen(env_on("ROGUESQ_FULLSCREEN"));
        } else if (rs64::video::peek_fullscreen(cfg_path + "roguesq_video.json")) {
            rs64::video::set_fullscreen(true);
        }
        if (rs64::video::fullscreen()) {
            g_fullscreen_applied = true;
            window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
    }
#endif
    int window_w = 640, window_h = 480;
    if (const char* ws = recomp::dbg::env_str("ROGUESQ_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (sscanf(ws, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) {
            window_w = w;
            window_h = h;
        }
    }
    // ROGUESQ_WINDOW_POS=x,y, "left" or "right" (just left or right of the primary display's centre, 8 px apart), e.g. for a co-op pair.
    int window_x = SDL_WINDOWPOS_CENTERED, window_y = SDL_WINDOWPOS_CENTERED;
    if (const char* wp = recomp::dbg::env_str("ROGUESQ_WINDOW_POS")) {
        SDL_Rect usable{};
        int x = 0, y = 0;
        const bool left = strcmp(wp, "left") == 0;
        if ((left || strcmp(wp, "right") == 0) && SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            window_x = left ? usable.x + usable.w / 2 - window_w - 4 : usable.x + usable.w / 2 + 4;
            window_y = usable.y + (usable.h - window_h) / 2;
        } else if (sscanf(wp, "%d,%d", &x, &y) == 2) {
            window_x = x;
            window_y = y;
        }
    }
    if (env_on("ROGUESQ_UNFOCUSED")) {
        SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
    }
    SDL_Window* sdl_window = SDL_CreateWindow(
        "Star Wars: Rogue Squadron 64 Recompiled",
        window_x, window_y,
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
#elif defined(__APPLE__)
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(sdl_window, &wm);
    SDL_MetalView view = SDL_Metal_CreateView(sdl_window);
    return { wm.info.cocoa.window, SDL_Metal_GetLayer(view) };
#else
    return sdl_window;
#endif
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // Window messages only dispatch on the thread that owns the window, so events (and RT64's F1-F4 filter) must be pumped here.
    SDL_PumpEvents();
#ifdef __ANDROID__
    // Back in the foreground: move RT64 onto the new window once SDL has one (until then SDL may still report the old one).
    if (g_android_foreground.load() && rs64_render_surface_suspended()) {
        rs64_render_resume_surface();
    }
#endif

    // Build the menu config on the main thread; doing it lazily from a game-thread menu hook races menu-audio init.
    { static bool s_cfg = false;
      if (!s_cfg) { s_cfg = true; rs64_menu_config_init(); } }

    { static bool s_rumble = false;
      if (!s_rumble) { s_rumble = true; std::thread(rumble_thread_main).detach(); } }

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
    g_vi_tick = g_vi_tick + 1;
    if (rs64_vi_driven()) return;   // hardware protocol: no host-injected tokens (they double-signal size-1 queues)
    g_vi_tick = g_vi_tick - 1;
    static int s_on = -1;
    if (s_on < 0) s_on = env_on("ROGUESQ_VI_BARRIER_SIGNAL", true);
    // Tick for rs64_attrib_wait_vi (attribution loop paces to the real VI).
    g_vi_tick = g_vi_tick + 1;
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
    {"fake-controller",  "ROGUESQ_FAKE_CONTROLLER",  CliFlag::Bool,  "1", "0", "fake a connected controller (headless runs)"},
    {"auto-start",       "ROGUESQ_AUTO_START",       CliFlag::Value, "",  "",  "pulse START after <ms> (headless runs)"},
    {"hide-window",      "ROGUESQ_HIDE_WINDOW",      CliFlag::Bool,  "1", "0", "create the window hidden (background process; no display/screenshots)"},
    {"maximized",        "ROGUESQ_MAXIMIZED",        CliFlag::Bool,  "1", "0", "start with the window maximized"},
    {"unfocused",        "ROGUESQ_UNFOCUSED",        CliFlag::Bool,  "1", "0", "show the window without taking focus (test runs)"},
    {"fullscreen",       "ROGUESQ_FULLSCREEN",       CliFlag::Bool,  "1", "0", "start in borderless fullscreen (Alt+Enter toggles)"},
    {"window-size",      "ROGUESQ_WINDOW_SIZE",      CliFlag::Value, "",  "",  "initial window client size WxH (e.g. 1280x720)"},
    {"widescreen",       "ROGUESQ_WIDESCREEN",       CliFlag::Bool,  "1", "0", "expand the aspect ratio to fill the window"},
    {"draw-distance",    "ROGUESQ_DRAW_DIST",        CliFlag::Value, "",  "",  "draw distance multiplier (e.g. 1.3; terrain and fog capped at 2.5)"},
    {"force-level",      "ROGUESQ_FORCE_LEVEL",      CliFlag::Value, "",  "",  "missions you launch from the menus are level N (0-18)"},
    {"force-craft",      "ROGUESQ_FORCE_CRAFT",      CliFlag::Value, "",  "",  "missions you launch from the menus use craft N (0-8)"},
    {"boot-target",      "ROGUESQ_BOOT_TARGET",      CliFlag::Value, "",  "",  "skip the intro to a target: menu | demo:N | level:N[,craft] | abort:N | cutscene:N | lobby:host|join[,level]"},
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
#ifdef __ANDROID__
    // No console, shell environment, or exe directory on Android: log to logcat, read env from a file, run from the app's files dir.
    rs64::android::start_logcat_pump();
    rs64::android::load_env_file();
    std::filesystem::current_path(rs64::android::data_dir());
    rs64::android::install_bundled_mods();
#endif
    if (int rc = apply_cli_args(argc, argv); rc >= 0) {
        return rc;
    }
    rs64_renderdoc_init();

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
        g_report_rumble_pak.store(g_bindings.rumble.enabled);
        fflush(stderr);
    }
    // Touch controls: roguesq_touch.json (on by default on Android); ROGUESQ_TOUCH_DEBUG draws the menu tap boxes.
    {
        rs64::touch::Config tc = rs64::touch::shared_config();
        if (env_on("ROGUESQ_TOUCH_DEBUG")) {
            tc.debug_hitboxes = true;
        }
        g_touch.set_config(tc);
        g_touch.set_menu_hover([](float x, float y) {
            uint8_t* r = touch_rdram();
            if (!r || touch_context() != rs64::touch::Context::PauseMenu) {
                return -1;
            }
            float w, h;
            window_size(&w, &h);
            return rs64::touch::pause_hover(r, x, y, w, h);
        });
        g_touch.set_menu_tap([](float x, float y) {
            const rs64::touch::TapAction a = touch_menu_tap(x, y);
            if (g_touch.config().debug_hitboxes) {
                fprintf(stderr, "[touch] tap %.3f,%.3f screen=%d state=%s -> action %d\n", x, y, (int)touch_account_screen(), rs64_state_current_id(), (int)a);
            }
            return a;
        });
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
    if (FILE *nul = recomp::os::fopen(RS64_NULL_DEVICE, "w")) {
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
#ifdef __ANDROID__
    // No ROM yet: ask for one with the system file picker until a valid one is imported, or the player quits.
    std::string rom_problem;
    while (!recomp::is_rom_valid(rs_game_id)) {
        const std::string picked = rs64::android::pick_rom();
        if (picked.empty()) {
            const SDL_MessageBoxButtonData buttons[] = {
                { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose ROM" },
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
            };
            const std::string text = rom_problem + "Rogue Squadron 64 Recomp needs your Star Wars: Rogue Squadron (USA v1.0) N64 ROM.";
            const SDL_MessageBoxData box{ SDL_MESSAGEBOX_INFORMATION, nullptr, "ROM needed", text.c_str(), 2, buttons, nullptr };
            int choice = 0;
            if (SDL_ShowMessageBox(&box, &choice) != 0 || choice != 1) {
                return 0;
            }
            rom_problem.clear();
            continue;
        }
        const recomp::RomValidationError r = recomp::select_rom(picked, rs_game_id);
        std::error_code ec;
        std::filesystem::remove(picked, ec);
        fprintf(stderr, "[ROM] picked file: validation %d\n", (int)r);
        if (r == recomp::RomValidationError::Good) {
            recomp::check_all_stored_roms();
            continue;
        }
        rom_problem = (r == recomp::RomValidationError::NotARom) ? "That file is not an N64 ROM.\n\n"
                    : (r == recomp::RomValidationError::IncorrectRom || r == recomp::RomValidationError::IncorrectVersion) ? "That ROM is not Star Wars: Rogue Squadron (USA v1.0).\n\n"
                    : "That file could not be read.\n\n";
        const SDL_MessageBoxButtonData buttons[] = {
            { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose again" },
            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        };
        const SDL_MessageBoxData box{ SDL_MESSAGEBOX_WARNING, nullptr, "Wrong file", rom_problem.c_str(), 2, buttons, nullptr };
        int choice = 0;
        if (SDL_ShowMessageBox(&box, &choice) != 0 || choice != 1) {
            return 0;
        }
        rom_problem.clear();
    }
#else
    if (!recomp::is_rom_valid(rs_game_id)) {
        fprintf(stderr,
            "[ROM] Place your Rogue Squadron (USA v1.0) ROM named\n"
            "      'rogue_squadron.z64' next to the executable and restart.\n");
    }
#endif

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
            // Also a dialog: Release has no console, so a stderr-only error (e.g. a mod conflict) left a silent black window.
            .message_box = [](const char* msg) {
                fprintf(stderr, "[Error] %s\n", msg);
                fflush(stderr);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rogue Squadron 64 Recompiled", msg, nullptr);
            },
        },
        .threads_callbacks = {
            .get_game_thread_name = get_game_thread_name,
        },
        .message_queue_control = {},
    });

    return EXIT_SUCCESS;
}
