#include "audio/audio.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "os_compat.h"
#include "ultramodern/ultra64.h"
#include "util/debug_logs.h"
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#endif

using recomp::dbg::env_on;
using recomp::dbg::env_int;

static inline uint32_t rdram_be32(const uint8_t* rdram, uint32_t off)
{
    return (uint32_t(rdram[(off + 0) ^ 3]) << 24) | (uint32_t(rdram[(off + 1) ^ 3]) << 16) | (uint32_t(rdram[(off + 2) ^ 3]) << 8) | uint32_t(rdram[(off + 3) ^ 3]);
}
static inline void rdram_wr32(uint8_t* rdram, uint32_t off, uint32_t v) {

    rdram[(off + 0) ^ 3] = (uint8_t)(v >> 24);
    rdram[(off + 1) ^ 3] = (uint8_t)(v >> 16);
    rdram[(off + 2) ^ 3] = (uint8_t)(v >> 8);
    rdram[(off + 3) ^ 3] = (uint8_t)v;
}

static unsigned g_rs64_audio_underruns = 0;

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

namespace rs64::audio {
static RspExitReason musyx_stub(uint8_t*, uint32_t)
{
    // Count audio-task submissions: proves the CPU MusyX sequencer is alive and
    // dispatching M_AUDTASK (only the RSP synth is stubbed) vs audio never driven.
    static int s_n = 0;
    ++s_n;

    if (s_n <= 8 || (s_n & 255) == 0)
    {
        fprintf(stderr, "[musyx-stub] M_AUDTASK #%d (sequencer is submitting audio tasks)\n", s_n);
        fflush(stderr);
    }

    return RspExitReason::Broke;
}

// Mimics SP_BOOT (zero DMEM, DMA ucode_data to DMEM[0]) then runs the RSPRecomp'd MusyX synth; the PCM reaches SDL via osAiSetNextBuffer and queue_samples.
static RspExitReason musyx_audio_runner(uint8_t* rdram, uint32_t ucode_addr)
{
    // ROGUESQ_LOG_AUDIO_OUT=1: one-time scan of RDRAM for the .samp waveform signature
    // (first 8 bytes of samp_SND.bin: FC 97 FF 63 01 58 00 81). Tells us if the sample
    // bank is loaded in RAM and at what base (= .samp_base for voice sample ptrs).
    static int s_log_samp_scan = -1, s_samp_found = 0, s_samp_scan_n = 0;

    if (s_log_samp_scan < 0) s_log_samp_scan = env_on("ROGUESQ_LOG_AUDIO_OUT");

    if (s_log_samp_scan && !s_samp_found && ((++s_samp_scan_n) <= 1 || (s_samp_scan_n % 256) == 0))
    {
        static const uint8_t sig[8] = { 0xFC,0x97,0xFF,0x63,0x01,0x58,0x00,0x81 };
        int hitsS = 0, hitsR = 0;
        uint32_t firstS = 0, firstR = 0;

        for (uint32_t o = 0; o + 8 <= 0x800000u; ++o) {
            bool ms = true, mr = true;

            for (int k = 0; k < 8; ++k) {
                if (rdram[(o + k) ^ 3] != sig[k]) ms = false;
                if (rdram[o + k] != sig[k]) mr = false;
                if (!ms && !mr) break;
            }

            if (ms) {
                if (!hitsS) firstS = o;

                ++hitsS;
            }

            if (mr) {
                if (!hitsR) firstR = o;

                ++hitsR;
            }

            if (hitsS >= 2 && hitsR >= 2) break;
        }

        fprintf(stderr, "[samp-scan #%d] swizzled hits=%d@0x%08X  raw hits=%d@0x%08X (%s)\n",
            s_samp_scan_n, hitsS, hitsS ? (0x80000000u | firstS) : 0u, hitsR, hitsR ? (0x80000000u | firstR) : 0u,
            (hitsS || hitsR) ? "<-LOADED" : "not in RAM");
        fflush(stderr);

        if (hitsS || hitsR) s_samp_found = 1;
    }

    std::memset(dmem, 0, 0xFC0);
    uint32_t ud  = g_audio_ucode_data_addr & 0x00FFFFFFu;
    uint32_t uds = g_audio_ucode_data_size;

    if (ud && uds && uds <= 0xFC0) {
        dma_rdram_to_dmem(rdram, /*dmem*/0, /*dram*/ud, /*rd_len*/uds - 1);
    }

    // Write the OSTask to DMEM[0xFC0..0x1000] big-endian (i^3 swizzle). The synth
    // reads data_ptr @+0x30, data_size @+0x34, output_buff @+0x28 from r1=0xFC0.
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

    static int s_n = 0;
    ++s_n;

    // Compare the LIVE synth ucode (RAM at OSTask.ucode) vs what the recomp decoded (ROM 0x9ABE0).
    // IMEM 0x3E8 (jal target of the header DMA) is nop+COP2 in ROM; if live RAM shows mtc0/DMA here
    // the recomp decoded the WRONG source (toml text_offset stale) — that's the synth-silence root.
    if (recomp::os::getenv("ROGUESQ_LOG_UCODESRC") && s_n <= 2)
    {
        uint32_t uc = (uint32_t)g_audio_task.t.ucode & 0x00FFFFFFu;
        auto dump = [&](uint32_t off){ char b[80]; int o=0;
            for(int k=0;k<16;k++) o+=snprintf(b+o,sizeof(b)-o,"%s%02X",(k%4==0)?" ":"",rdram[(uc+off+k)^3]);

            fprintf(stderr,"[ucodesrc] live RAM ucode(0x%06X)+0x%X:%s\n", uc, off, b); };
        dump(0x3E8);  // ROM = 00000000 4B271094 C9231800 4B070850 (nop+vector)
        dump(0x3B0);  // ROM = 40850800 03E00008 40871000 400A3000 (mtc0 DMA)
        fflush(stderr);
    }

    // Probe the command list the synth will process — does it actually contain voice commands?
    static int s_log_cmdlist = -1;

    if (s_log_cmdlist < 0) s_log_cmdlist = env_on("ROGUESQ_LOG_AUDIO_OUT");

    if (s_log_cmdlist && (s_n <= 8 || (s_n % 128) == 0))
    {
        uint32_t dp = (uint32_t)g_audio_task.t.data_ptr & 0x00FFFFFFu;
        uint32_t ds = (uint32_t)g_audio_task.t.data_size;
        char hx[260];
        int o = 0;
        uint32_t ca=0x149700;
        unsigned vcount=((unsigned)rdram[ca^3]<<8)|rdram[(ca+1)^3];
        uint32_t cb=0x149702;
        unsigned bcnt=rdram[cb^3];

        fprintf(stderr,"[voicetable #%d] synthVoiceCount@0x80149700=%u  byteCnt@0x80149702=%u\n", s_n, vcount, bcnt);

        // Descriptor-buffer ptrs at 0x80149708 (-0x68F8, double-buffered by activeCount). musyxMixActiveVoices
        // writes the DSP voice descriptors here. Compare to the command's word[2] to detect a buffer desync.
        auto rdw=[&](uint32_t a){return rdram_be32(rdram, a);};

        for (int b=0;b<2;b++)
        {
            uint32_t bp=rdw(0x149708+b*4);
            uint32_t bpp=bp&0x00FFFFFFu;
            int nz=0,nz100=0;
            char h0[80],h1[80];
            int o0=0,o1=0;

            if (bpp && bpp+0x300<=0x800000u)
            {
                for (uint32_t k=0;k<0x300;k++) if(rdram[(bpp+k)^3]) ++nz;
                for (uint32_t k=0x100;k<0x130;k++) if(rdram[(bpp+k)^3]) ++nz100;
                for (int k=0;k<0x18;k++) o0+=snprintf(h0+o0,sizeof(h0)-o0,"%s%02X",(k%4==0)?" ":"",rdram[(bpp+k)^3]);
                for (int k=0;k<0x18;k++) o1+=snprintf(h1+o1,sizeof(h1)-o1,"%s%02X",(k%4==0)?" ":"",rdram[(bpp+0x100+k)^3]);
            }

            fprintf(stderr,"[descbuf #%d] [%d]=0x%08X nz=%d @+0x0:%s | nz@+0x100=%d @+0x100:%s\n", s_n, b, bp, nz, h0, nz100, h1);
        }

        // outBuf array @0x80149758 (-0x68A8), outBuf voice count @0x80149760 (-0x68A0, halfword),
        // and the per-descBuf active-flag (+0x22) count. word2 should == one of the outBufs.
        uint32_t ob0=rdw(0x149758), ob1=rdw(0x14975C);
        unsigned obc=((unsigned)rdram[0x149760^3]<<8)|rdram[0x149761^3];
        int af0=0,af1=0;

        for (int b=0;b<2;b++)
        {
            uint32_t bp=rdw(0x149708+b*4)&0x00FFFFFFu;
            int*af=b?&af1:&af0;

            if (bp&&bp+20*0x88<=0x800000u) for(int s=0;s<20;s++) if(rdram[(bp+s*0x88+0x22)^3]) ++(*af);
        }

        fprintf(stderr,"[outbuf #%d] outBuf[0]=0x%08X [1]=0x%08X voiceCount=%u | activeFlag(+0x22) descBuf0=%d descBuf1=%d\n",
          s_n, ob0, ob1, obc, af0, af1);

        // control-struct globals: word3 of the header should = -0x6898 (0x149768)
        fprintf(stderr,"[ctrl #%d] -0x6898=0x%08X -0x6894=0x%08X -0x6890=0x%08X -0x689C=0x%08X\n",
          s_n, rdw(0x149768), rdw(0x14976C), rdw(0x149770), rdw(0x149764));

        // also dump the command's referenced buffer (word[2]) for comparison
        uint32_t w2=(dp&&dp<0x800000u)?rdw(dp+8):0;
        uint32_t w2p=w2&0x00FFFFFFu;
        char hb[160];
        int ho=0;

        if (w2p&&w2p+0x30<=0x800000u) for(int k=0;k<0x30;k++) ho+=snprintf(hb+ho,sizeof(hb)-ho,"%s%02X",(k%4==0)?" ":"",rdram[(w2p+k)^3]);

        fprintf(stderr,"[cmdw2 #%d] word[2]=0x%08X:%s\n", s_n, w2, hb);

        // is the CPU-written voice data at data_ptr+0x100 (where synth would read if word2==data_ptr)?
        char hd[160];
        int hk=0;
        int nzdp=0;

        if (dp&&dp+0x300<=0x800000u)
        {
            for (uint32_t k=0x100;k<0x2A0;k++) if(rdram[(dp+k)^3]) ++nzdp;
            for (int k=0;k<0x30;k++) hk+=snprintf(hd+hk,sizeof(hd)-hk,"%s%02X",(k%4==0)?" ":"",rdram[(dp+0x100+k)^3]);
        }

        fprintf(stderr,"[dpdata #%d] data_ptr=0x%08X +0x100 nz(0x1A0)=%d:%s\n", s_n, (uint32_t)g_audio_task.t.data_ptr, nzdp, hd);
        o += snprintf(hx + o, sizeof(hx) - o, "[cmdlist #%d] data_ptr=0x%08X size=0x%X:", s_n, (uint32_t)g_audio_task.t.data_ptr, ds);

        if (dp && dp < 0x800000u) for (int k = 0; k < 16; k++) o += snprintf(hx + o, sizeof(hx) - o, "%s%02X", (k % 4 == 0) ? " " : "", rdram[(dp + k) ^ 3]);

        // also dump the voice block the command points to (data_ptr+0x08) — is it ever populated?
        uint32_t vp = dp && dp < 0x800000u ? rdram_be32(rdram, dp+8) : 0;
        uint32_t vpp = vp & 0x00FFFFFFu;
        int vnz = 0, vnz2 = 0;

        if (vpp && vpp < 0x800000u)
        {
            for (uint32_t k = 0; k < 0x100; k++) if (rdram[(vpp + k) ^ 3]) ++vnz;            // base..+0x100

            uint32_t vp2 = vpp + 0x100;                                                       // where the synth actually reads (r5+0x100)

            if (vp2 + 0x198 <= 0x800000u) for (uint32_t k = 0; k < 0x198; k++) if (rdram[(vp2 + k) ^ 3]) ++vnz2;

            o += snprintf(hx + o, sizeof(hx) - o, " | voiceHdr@0x%08X nz=%d  voiceData@+0x100 nz=%d:", vp, vnz, vnz2);

            for (int k = 0; k < 24; k++) o += snprintf(hx + o, sizeof(hx) - o, "%s%02X", (k % 4 == 0) ? " " : "", rdram[(vp2 + k) ^ 3]);
        }

        fprintf(stderr, "%s\n", hx);
        fflush(stderr);
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
    static int s_tgt = -2;

    if (s_tgt == -2) {
        const char* e = recomp::os::getenv("ROGUESQ_DUMP_SYNTH_FRAME");
        s_tgt = e ? atoi(e) : -1;
    }

    if (s_tgt >= 0 && s_n == s_tgt)
    {
        const char* base = recomp::os::getenv("ROGUESQ_DUMP_SYNTH_PATH");

        if (!base || !base[0]) base = "dumps/synth_frame";

        char pb[512];
        snprintf(pb, sizeof(pb), "%s.bin", base);
        FILE* f = recomp::os::fopen(pb, "wb");

        if (f) {
            std::vector<uint8_t> img(0x800000);

            for (uint32_t i = 0; i < 0x800000u; ++i) img[i] = rdram[i ^ 3];

            fwrite(img.data(), 1, img.size(), f);
            fclose(f);
        }

        char pt[512];
        snprintf(pt, sizeof(pt), "%s.task", base);
        FILE* g = recomp::os::fopen(pt, "wb");

        if (g)
        {
            uint8_t tb[0x40];
            std::memset(tb, 0, sizeof(tb));
            auto put = [&](int off, uint32_t v){ tb[off]=v>>24; tb[off+1]=v>>16; tb[off+2]=v>>8; tb[off+3]=v; };
            put(0x00,(uint32_t)t->t.type);        put(0x04,(uint32_t)t->t.flags);
            put(0x10,(uint32_t)t->t.ucode);       put(0x14,(uint32_t)t->t.ucode_size);
            put(0x18,(uint32_t)t->t.ucode_data);  put(0x1C,(uint32_t)t->t.ucode_data_size);
            put(0x28,(uint32_t)t->t.output_buff); put(0x2C,(uint32_t)t->t.output_buff_size);
            put(0x30,(uint32_t)t->t.data_ptr);    put(0x34,(uint32_t)t->t.data_size);
            fwrite(tb, 1, sizeof(tb), g);
            fclose(g);
        }

        fprintf(stderr, "[synth-frame] dumped task#%d data_ptr=0x%08X size=0x%X -> %s(.bin/.task)\n",
                s_n, (uint32_t)g_audio_task.t.data_ptr, (uint32_t)g_audio_task.t.data_size, base);
        fflush(stderr);
    }

    RspExitReason r = musyx_audio(rdram, ucode_addr);
    auto t2 = std::chrono::steady_clock::now();

    // Did the synth produce ANY internal signal? Scan its DMEM work area (accumulation/voice
    // buffers live in DMEM 0x600..0xFC0). All-zero => synth bailed early (no active voice). This
    // isolates "ucode not processing voices" from "output not reaching RDRAM/AI".
    static int s_dm = -1;

    if (s_dm < 0) s_dm = env_on("ROGUESQ_LOG_DMEM");

    if (s_dm && (s_n <= 8 || (s_n % 64) == 0))
    {
        uint32_t nz = 0;
        int mx = 0;

        for (uint32_t i = 0x600; i < 0xFC0; ++i) {
            uint8_t b = dmem[i];

            if (b) {
                ++nz;

                if (b > mx) mx = b;
            }
        }

        // also scan the voice block DMEM region 0xCB0..0xE48 specifically (where word2+0x100 was DMA'd)
        uint32_t nzv = 0;

        for (uint32_t i = 0xCB0; i < 0xE48; ++i) if (dmem[i]) ++nzv;

        fprintf(stderr, "[dmem #%d] work(0x600-0xFC0) nz=%u peak=%d | voiceblk(0xCB0-0xE48) nz=%u  exit=%d\n",
            s_n, nz, mx, nzv, (int)r);
        fflush(stderr);
    }

    // Probe the synth's OWN output buffer right after synthesis: does it write ANY non-zero PCM?
    // Scans raw bytes so it's swizzle-independent (a non-zero check is order-independent). This
    // isolates "synth produces silence" from "output buffer never reaches SDL" (queue_samples).
    static int s_log_synth_out = -1;

    if (s_log_synth_out < 0) s_log_synth_out = env_on("ROGUESQ_LOG_AUDIO_OUT");

    if (s_log_synth_out && (s_n <= 8 || (s_n % 128) == 0))
    {
        uint32_t ob  = (uint32_t)g_audio_task.t.output_buff & 0x00FFFFFFu;
        uint32_t obs = (uint32_t)g_audio_task.t.output_buff_size;
        uint8_t mx = 0;
        uint32_t nz = 0;

        if (ob && obs && (uint64_t)ob + obs <= 0x800000u) {
            for (uint32_t i = 0; i < obs; ++i) {
                uint8_t b = rdram[ob + i];

                if (b) {
                    ++nz;

                    if (b > mx) mx = b;
                }
            }
        }

        fprintf(stderr, "[synth-out #%d] outBuf=0x%08X size=0x%X peakByte=%d nonzero=%u %s\n",
            s_n, (uint32_t)g_audio_task.t.output_buff, obs, (int)mx, nz, nz ? "<-SYNTH HAS SIGNAL" : "(synth silent)");
        fflush(stderr);

        // MusyX gets its work from the command list at data_ptr (NOT output_buff). RDRAM is stored
        // byte-swapped, so read words with the ^3 swizzle to get true N64 values. word[2] is a
        // pointer to the real audio data/output buffer; follow it and scan for non-zero PCM.
        auto rd_w = [&](uint32_t off) -> uint32_t {
            return rdram_be32(rdram, off);
        };

        uint32_t dp = (uint32_t)g_audio_task.t.data_ptr & 0x00FFFFFFu;
        uint32_t dsz = (uint32_t)g_audio_task.t.data_size;

        if (dp && dp + 0x40 <= 0x800000u)
        {
            char line[256];
            int o = 0;
            o += snprintf(line + o, sizeof(line) - o, "[synth-cmd #%d] data_ptr=0x%08X size=0x%X words:",
                s_n, (uint32_t)g_audio_task.t.data_ptr, dsz);

            for (int w = 0; w < 8; ++w) o += snprintf(line + o, sizeof(line) - o, " %08X", rd_w(dp + w * 4));

            fprintf(stderr, "%s\n", line);
            fflush(stderr);

            // Follow word[2] as a pointer and scan its target for non-zero audio data.
            uint32_t ptr = rd_w(dp + 8) & 0x00FFFFFFu;

            if (ptr && ptr + 0x100 <= 0x800000u)
            {
                uint8_t ptr_peak = 0;
                uint32_t ptr_nz = 0;

                for (uint32_t i = 0; i < 0x100; ++i) {
                    uint8_t b = rdram[ptr + i];

                    if (b) {
                        ++ptr_nz;

                        if (b > ptr_peak) ptr_peak = b;
                    }
                }

                fprintf(stderr, "[synth-ptr #%d] target=0x%08X peakByte=%d nonzero=%u/256 %s\n",
                    s_n, rd_w(dp + 8), (int)ptr_peak, ptr_nz, ptr_nz ? "<-DATA PRESENT" : "(empty)");
                fflush(stderr);
            }
        }
    }

    double aud_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    static double s_sum = 0, s_max = 0;
    static int s_cnt = 0;
    s_sum += aud_ms;
    ++s_cnt;

    if (aud_ms > s_max) s_max = aud_ms;

    // Gap between synth ticks (retrace-thread starvation shows as gaps far above one VI).
    static auto s_last = t1;
    static double s_gap_max = 0;
    static int s_gap_over = 0;
    const double gap = std::chrono::duration<double, std::milli>(t1 - s_last).count();
    s_last = t1;

    if (gap > s_gap_max) s_gap_max = gap;
    if (gap > 40.0) ++s_gap_over;

    if (recomp::dbg::log_heartbeat() && (s_n <= 6 || (s_n % 64) == 0))
    {
        fprintf(stderr, "[musyx-run #%d] audio=%.1fms avg=%.1fms max=%.1fms (n=%d) tick-gap max=%.0fms over40=%d underruns=%u\n",
            s_n, aud_ms, s_sum / s_cnt, s_max, s_cnt, s_gap_max, s_gap_over, g_rs64_audio_underruns);
        fflush(stderr);
        s_gap_max = 0;
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

    switch (task->t.type)
    {
    case M_GFXTASK:
        // Graphics tasks go to send_dl in events.cpp; this is never reached for them.
        return &unknown_task_stub;
    case M_AUDTASK: {
        // Capture the MusyX audio ucode's OSTask fields once — these give the
        // text/data RDRAM addresses + sizes that RSPRecomp needs (see
        // project_audio_state). ucode = IMEM text source, ucode_data = DMEM data.
        static int s_logged = 0;

        if (s_logged < 4)
        {
            ++s_logged;

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
        g_audtask_n = g_audtask_n + 1;

        // Runs the RSPRecomp'd MusyX synth (now functional after the text_address=0x1080
        // fix). DEFAULT ON; opt out with ROGUESQ_NO_AUDIO_UCODE=1 to fall back to musyx_stub.
        static int s_au = -1;

        if (s_au < 0) s_au = env_on("ROGUESQ_NO_AUDIO_UCODE") ? 0 : 1;
        if (s_au) return &musyx_audio_runner;

        return &musyx_stub;
    }
    default:
        // Don't crash — log once per distinct type and stub it out.
        static thread_local uint32_t last_unknown = 0;

        if (task->t.type != last_unknown)
        {
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

void set_frequency(uint32_t freq)
{
    // Don't churn the device when the rate hasn't changed — each close/reopen
    // flushes the SDL queue and clicks. The game re-sets the same AI rate
    // repeatedly (observed 22050 set 3x), so this removes those pops.
    if (audio_device && freq == audio_sample_rate)
    {
        return;
    }

    open_audio_device(freq);
}

static void open_audio_device(uint32_t freq)
{

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

    if (!audio_device)
    {
        fprintf(stderr, "[Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());

        return;
    }

    SDL_PauseAudioDevice(audio_device, 0);

    fprintf(stderr, "[Audio] device OPENED freq=%u (output path live)\n", freq);
    fflush(stderr);
}

void queue_samples(int16_t* samples, size_t num_samples)
{
    // ultramodern's queue_samples contract passes the int16 SAMPLE count (= AI
    // byte_count / 2), NOT a byte count. This function's body is written in bytes,
    // so convert once here. Previously the sample count was used directly as bytes,
    // so only HALF of every buffer reached SDL (the game submits 0x300=768-byte
    // buffers; SDL was getting 384) — the dominant cause of the underrun/crackle.
    const size_t num_bytes = num_samples * sizeof(int16_t);

#ifdef __ANDROID__

    if (!g_android_foreground.load())
    {
        return;
    }

    static uint32_t s_probe_until = 0;

    if (g_android_audio_reopen.exchange(false))
    {
        open_audio_device(audio_sample_rate);
        s_probe_until = SDL_GetTicks() + 10000;

        fprintf(stderr, "[Audio] reopened device after returning to the foreground\n");
    }

    // Measure what is still queued in SDL for a while after returning (a multi-second value would mean the backlog is on our side).
    if (audio_device && !SDL_TICKS_PASSED(SDL_GetTicks(), s_probe_until)) {
        static uint32_t s_last = 0;

        if (SDL_GetTicks() - s_last >= 1000)
        {
            s_last = SDL_GetTicks();

            fprintf(stderr, "[Audio] after resume: SDL queued %.1f ms\n", SDL_GetQueuedAudioSize(audio_device) / 4.0 * 1000.0 / audio_sample_rate);
        }
    }

#endif

    if (audio_device)
    {
        // Underrun gauge: the device queue was already dry when this buffer arrived (audible gap).
        static int warm = 0;

        if (++warm > 64 && SDL_GetQueuedAudioSize(audio_device) == 0) ++g_rs64_audio_underruns;

        // Diagnostic (ROGUESQ_LOG_AUDIO_OUT=1): confirm PCM flows + silence vs content.
        static int s_lo = -1;

        if (s_lo < 0) s_lo = env_on("ROGUESQ_LOG_AUDIO_OUT");

        static int s_q = 0;
        ++s_q;

        if (s_lo && (s_q <= 8 || (s_q & 255) == 0))
        {
            size_t n = num_bytes / 2;
            int16_t mx = 0;

            for (size_t i = 0; i < n; ++i) { int16_t v = samples[i]; if (v < 0) v = -v; if (v > mx) mx = v; }

            fprintf(stderr, "[queue_samples #%d] %zu bytes, peak_amp=%d %s queued=%u bytes (%.1f ms)\n", s_q, num_bytes, (int)mx, mx ? "<-AUDIBLE" : "(silence)", (unsigned)SDL_GetQueuedAudioSize(audio_device), audio_sample_rate ? SDL_GetQueuedAudioSize(audio_device) / 4.0 * 1000.0 / audio_sample_rate : 0.0);
            fflush(stderr);
        }

        // Production-rate gauge: frames/sec actually produced vs the device rate. <100%
        // means the synth underproduces and any buffer eventually bleeds dry (the cut-out).
        // M_AUDTASK/s shows whether it's a cadence deficit (few tasks) or per-task size.
        if (s_lo)
        {
            static uint64_t s_tf = 0;
            static bool s_init = false;
            static std::chrono::steady_clock::time_point s_t0;
            auto nowt = std::chrono::steady_clock::now();

            if (!s_init) { s_init = true; s_t0 = nowt; }

            s_tf += num_bytes / 4;

            // Step-0 cadence probe (ROGUESQ_LOG_AUDIO_OUT): is the 89% a SUSTAINED slow cadence or JITTER
            // with dropouts? Bucket the inter-call gap of queue_samples (= buffer-production cadence) and
            // track per-call buffer frames. A tight ~1/60s cluster = sustained-slow; a bimodal spread with
            // a >25ms tail = jitter/starvation. Also count calls/window to get effective production Hz.
            static std::chrono::steady_clock::time_point s_last{}; static bool s_lhave = false;
            static uint32_t s_gh[7] = {0}; static uint32_t s_calls = 0; static uint32_t s_fmin = ~0u, s_fmax = 0; static uint64_t s_fsum = 0;

            if (s_lhave)
            {
                double gms = std::chrono::duration<double, std::milli>(nowt - s_last).count();
                int b = gms < 5 ? 0 : gms < 10 ? 1 : gms < 15 ? 2 : gms < 20 ? 3 : gms < 25 ? 4 : gms < 40 ? 5 : 6;
                s_gh[b]++;
            }

            s_last = nowt;
            s_lhave = true;
            uint32_t f = num_bytes / 4;
            s_calls++;
            s_fsum += f;

            if (f < s_fmin) s_fmin = f;
            if (f > s_fmax) s_fmax = f;

            double el = std::chrono::duration<double>(nowt - s_t0).count();

            if (el >= 2.0)
            {
                fprintf(stderr, "[audio-rate] %.0f frames/s (target %u, %.0f%%) | %.1f M_AUDTASK/s | calls=%.1f/s bufFrames min/avg/max=%u/%llu/%u\n",
                    s_tf / el, audio_sample_rate, 100.0 * (s_tf / el) / (audio_sample_rate ? audio_sample_rate : 1), g_audtask_n / el,
                    s_calls / el, s_fmin==~0u?0:s_fmin, (unsigned long long)(s_calls?s_fsum/s_calls:0), s_fmax);
                fprintf(stderr, "[audio-gap] queue_samples inter-call ms buckets <5|5-10|10-15|15-20|20-25|25-40|>40 = %u %u %u %u %u %u %u\n",
                    s_gh[0], s_gh[1], s_gh[2], s_gh[3], s_gh[4], s_gh[5], s_gh[6]);
                fflush(stderr);
                s_t0 = nowt;
                s_tf = 0;
                g_audtask_n = 0;

                for (int i=0;i<7;i++) s_gh[i]=0; s_calls=0; s_fmin=~0u; s_fmax=0; s_fsum=0;
            }
        }

        // Optional PCM->WAV capture for offline music encoding (ROGUESQ_DUMP_PCM=path or 1).
        // Header sizes are patched each buffer so the file stays valid if the run is killed.
        static FILE* s_wav = nullptr;
        static int s_wav_init = -1;
        static uint32_t s_wav_bytes = 0;

        if (s_wav_init < 0) {
            const char* e = recomp::os::getenv("ROGUESQ_DUMP_PCM");
            s_wav_init = (e && e[0] && e[0] != '0') ? 1 : 0;

            if (s_wav_init)
            {
                const char* path = (e[0] == '1' && e[1] == '\0') ? "dumps/wav/capture.wav" : e;
                s_wav = recomp::os::fopen(path, "wb");

                fprintf(stderr, "[pcm-dump] init path=%s fopen=%p\n", path, (void*)s_wav);
                fflush(stderr);

                if (s_wav)
                {
                    uint16_t ch = 2, bps = 16;
                    uint32_t sr = audio_sample_rate;
                    uint32_t br = sr * ch * bps / 8;
                    uint16_t ba = ch * bps / 8;
                    uint32_t fmtlen = 16;
                    uint16_t fmt = 1;
                    uint8_t hdr[44] = {0};
                    memcpy(hdr, "RIFF", 4); memcpy(hdr + 8, "WAVE", 4); memcpy(hdr + 12, "fmt ", 4);
                    memcpy(hdr + 16, &fmtlen, 4); memcpy(hdr + 20, &fmt, 2); memcpy(hdr + 22, &ch, 2);
                    memcpy(hdr + 24, &sr, 4); memcpy(hdr + 28, &br, 4); memcpy(hdr + 32, &ba, 2); memcpy(hdr + 34, &bps, 2);
                    memcpy(hdr + 36, "data", 4);
                    fwrite(hdr, 1, 44, s_wav);
                    fflush(s_wav);
                    fprintf(stderr, "[pcm-dump] writing %s @ %uHz stereo s16\n", path, sr);
                    fflush(stderr);
                }
            }
        }

        if (s_wav) {
            fwrite(samples, 1, num_bytes, s_wav);
            s_wav_bytes += (uint32_t)num_bytes;
            uint32_t riff = 36 + s_wav_bytes;
            fseek(s_wav, 4, SEEK_SET);
            fwrite(&riff, 4, 1, s_wav);
            fseek(s_wav, 40, SEEK_SET);
            fwrite(&s_wav_bytes, 4, 1, s_wav);
            fseek(s_wav, 0, SEEK_END);
            fflush(s_wav);
        }

        // Speaker-safety master gain: the MusyX mix clips hard (peaks slam the int16 rail)
        // during busy cinematic SFX. Scale down before SDL so it doesn't blast/distort.
        // Tunable via ROGUESQ_AUDIO_GAIN (0.0-1.0); default 0.5. Applied after the raw WAV dump.
        static float s_gain = -1.0f;

        if (s_gain < 0.0f) {
            const char* e = recomp::os::getenv("ROGUESQ_AUDIO_GAIN");
            s_gain = (e && e[0]) ? (float)atof(e) : 0.35f;

            if (s_gain < 0.0f || s_gain > 1.0f) s_gain = 0.35f;
        }

        if (s_gain < 0.999f)
        {
            size_t n = num_bytes / 2;

            for (size_t i = 0; i < n; ++i) samples[i] = (int16_t)((float)samples[i] * s_gain);
        }

        SDL_QueueAudio(audio_device, samples, (Uint32)num_bytes);
    }
}

size_t get_frames_remaining()
{

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

#ifdef __ANDROID__

void set_foreground(bool on) {

    if (on)
    {
        // Start from a fresh device so sound lines up with the picture again.
        if (audio_device) {
            SDL_ClearQueuedAudio(audio_device);
        }

        g_android_audio_reopen.store(true);
    }

    g_android_foreground.store(on);
}

bool foreground() {
    return g_android_foreground.load();
}

#endif
}
