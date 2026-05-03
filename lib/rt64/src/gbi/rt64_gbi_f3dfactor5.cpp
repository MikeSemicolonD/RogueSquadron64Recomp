//
// RT64
//

#include "rt64_gbi_f3dfactor5.h"

#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "rt64_gbi_f3dex.h"
#include "rt64_gbi_f3d.h"
#include "rt64_gbi_rdp.h"

#include "hle/rt64_rsp.h"

#include <cstdio>

namespace RT64 {
    // Defined in rt64_interpreter.cpp — one-shot capture ring buffer.
    struct DLHistEntry { uint32_t w0, w1, dlAddr; uint8_t opcode; };
    extern DLHistEntry g_dlHist[];
    extern size_t g_dlHistCount;
    extern bool g_op02Captured;

    namespace GBI_F3DFACTOR5 {
        // op 0x80: Factor5 chunk header (metadata, not control flow).
        // Payload: w0=next_chunk_addr, w1=prev_chunk_addr (back-pointer).
        //
        // Confirmed via texrect_chunks.bin dump: chunks are 0x108-byte blocks
        // packed contiguously; w0 points exactly 0x108 forward. The parent DL
        // walks the chain via standard G_DL (op=0x06, ~109 invocations vs 210
        // op=0x80 markers in the dump). Header is a no-op for HLE.
        //
        // CAUTION: tried calling state->fullSync() here as a batch flush —
        // crashed with "vector subscript out of range" after 3 invocations.
        // fullSync from inside a mid-DL handler is unsafe in this codebase
        // because the workload-cursor advance leaves indexed structures in
        // a transitional state. Don't reintroduce without auditing every
        // workload-indexed path in the call chain first.
        void op80_unknown(State *state, DisplayList **dl) {
            // no-op
        }

        // TODO: identify. Payload is constant: w0=0x028001C0 w1=0x01FF0000 every call.
        // On first dispatch, dump full RDRAM + DL history so the RSP op02 handler
        // can be reverse-engineered offline against real input data.
        void op02_unknown(State *state, DisplayList **dl) {
            constexpr size_t kDLHistLen = 64;
            if (g_op02Captured) return;
            g_op02Captured = true;

            constexpr size_t kRDRAMSize = 8 * 1024 * 1024;  // 8 MB (expansion pak)
            if (FILE *fp = fopen("rdram_op02.bin", "wb")) {
                fwrite(state->RDRAM, 1, kRDRAMSize, fp);
                fclose(fp);
            }

            if (FILE *fp = fopen("dlhist_op02.txt", "w")) {
                fprintf(fp, "# DL history at first op02 dispatch\n");
                fprintf(fp, "# total cmds seen so far: %zu\n", g_dlHistCount);
                size_t start = (g_dlHistCount > kDLHistLen) ? (g_dlHistCount - kDLHistLen) : 0;
                for (size_t i = start; i < g_dlHistCount; i++) {
                    const auto &e = g_dlHist[i % kDLHistLen];
                    fprintf(fp, "%04zu  op=0x%02X  w0=0x%08X  w1=0x%08X  dlAddr=0x%08X\n",
                        i, e.opcode, e.w0, e.w1, e.dlAddr);
                }
                fclose(fp);
            }

            fprintf(stderr, "[op02] captured RDRAM + DL history (cmd #%zu)\n", g_dlHistCount);
            fflush(stderr);
        }

        // Opcode 0xB5 in Factor5 is the chunk/DL terminator — equivalent to
        // F3DEX's G_ENDDL (0xB8). Each 0x108-byte chunk ends with op=0xB5 at
        // offset 0x100; without popping the call stack here, the interpreter
        // walks linearly into the next chunk's op=0x80 header (which is also
        // a no-op) and continues forever. Confirmed via runtime DL dumps: the
        // sub-DL at 0x007239B8 contains a SINGLE op=0xB5 command, only making
        // sense as a "do nothing, return" marker.
        void op_B5_endDl(State *state, DisplayList **dl) {
            *dl = state->popReturnAddress();
        }

        // op_B4: Factor5-specific 16-byte command (cmd word + 8-byte payload).
        // Identified via drift detector: every drift-into-ASCII transition was
        // preceded by op_B4 dispatched at standard 8-byte stride. The 8 bytes
        // following the cmd were being read as the next opcode, causing the
        // interpreter to mis-parse data as commands. Consume the extra word
        // here; the dispatch loop's dl++ takes care of the cmd word itself.
        // Payload pattern observed: w0=0xB4000006 with varying w1 carrying
        // packed data (e.g., 0x14373C28). Likely a register/state setter.
        // Treat as no-op for now — just consume the right number of bytes.
        void op_B4_consume16(State *state, DisplayList **dl) {
            (*dl)++;  // skip the 8-byte payload
        }

        // Factor5 op_BF: also a 16-byte command (cmd word + 8-byte payload).
        // F3DEX standard maps op_BF = G_TRI1 (8 bytes), but in Factor5 the
        // drift detector consistently shows op_BF as the last cmd before
        // drift-into-ASCII payload. Same pattern as op_B4. Pair often appears
        // as `op_FA op_BF` (set env color + Factor5 cmd).
        void op_BF_consume16(State *state, DisplayList **dl) {
            (*dl)++;
        }

        // Strict G_DL filter for Factor5. Standard F3D G_DL has w0=0x06000000
        // (opcode + branch flag at bit 16, rest zero). Factor5 emits commands
        // that share the first byte 0x06 but pack data into w0's low 24 bits
        // (e.g., w0=0x060A07C0 — opcode 0x06 followed by 24 bits of payload).
        // Treating these as G_DLs leads runDl to dispatch w1 as a target
        // address, which can land inside vertex/color data and corrupt the
        // interpreter state. Filter: only call F3D::runDl if w0's payload
        // bits (excluding the branch flag at bit 16) are zero.
        void op_06_strict_dl(State *state, DisplayList **dl) {
            const uint32_t w0Payload = (*dl)->w0 & 0x00FEFFFF;  // mask out opcode and branch flag
            if (w0Payload == 0) {
                GBI_F3D::runDl(state, dl);
                return;
            }
            // Looks like a Factor5-specific opcode reusing byte 0x06. No-op for now.
            static int n = 0;
            if (++n <= 10 || (n % 5000) == 0) {
                fprintf(stderr, "[trace] op_06 non-standard #%d w0=0x%08X w1=0x%08X (skipped)\n",
                    n, (*dl)->w0, (*dl)->w1);
                fflush(stderr);
            }
        }

        // Factor5 emits one G_SETCIMG (0xFF) per render-pass with a bogus
        // payload (w0=0xFFF00F0F, w1=0x00000000) immediately before the
        // overlay TEXRECT batch. That zeroes RDP::colorImage.address, so the
        // subsequent TEXRECTs render onto a null target and produce no
        // visible output. Real SETCIMG calls always carry a non-zero w1.
        // Filter out the bogus form here.
        void setColorImage_filtered(State *state, DisplayList **dl) {
            const uint32_t w0 = (*dl)->w0;
            const uint32_t w1 = (*dl)->w1;
            if (w1 == 0) {
                return;
            }
            // Reject when fmt field (w0 bits 23-21) is invalid (>4). Standard
            // G_IM_FMT values: RGBA=0, YUV=1, CI=2, IA=3, I=4. fmt 5,6,7 only
            // appear when garbage (e.g. RGBA8888 pixel = 0xFFFFFFFF) is parsed
            // as a SETCIMG cmd. Without this, downstream allocators get fed
            // bogus fmt+siz+width and crash with zero-size buffer asserts in
            // D3D12MemoryAllocator.
            const uint32_t fmt = (w0 >> 21) & 0x7;
            if (fmt > 4) {
                static int n = 0;
                if (++n <= 10 || (n % 5000) == 0) {
                    fprintf(stderr, "[trace] setColorImage skip-bogus #%d w0=0x%08X w1=0x%08X (fmt=%u>4)\n",
                        n, w0, w1, fmt);
                    fflush(stderr);
                }
                return;
            }
            GBI_F3D::setColorImage(state, dl);
        }


        void setup(GBI *gbi) {
            GBI_F3DEX::setup(gbi);

            gbi->map[0x80] = &op80_unknown;
            gbi->map[0x02] = &op02_unknown;
            gbi->map[0x06] = &op_06_strict_dl;

            // Factor5 emits commands that share first byte with F3DEX opcodes
            // but encode entirely different payloads. Inheriting the F3DEX
            // handler causes crashes when garbage payload is interpreted as
            // F3DEX state. Map known-collision opcodes to no-op:
            //   0xB0 = F3DEX G_BRANCH_Z. Factor5 emits packed data here; the
            //          inherited handler extracts a vertex index from w0 bits
            //          1-11, hits std::array bounds check on the 32-entry
            //          vertex cache. Confirmed via crash-dump ring buffer.
            gbi->map[0xB0] = &op80_unknown;
            gbi->map[0xB4] = &op_B4_consume16;
            gbi->map[0xBF] = &op_BF_consume16;
            // EXPERIMENT: was &op_B5_endDl. Runtime DL dumps show many sub-DLs
            // start with op_B5 followed by FD/F5/F3 setup ending in op_B8
            // (standard F3D G_ENDDL). Treating B5 as endDl skips the setup
            // and leaves following TEXRECTs rendering with stale TMEM —
            // probable cause of "P shows as E" / missing-glyph symptom.
            // Try B5 as no-op; trust B8 (inherited from F3D) as real endDl.
            // Chunk runaways are caught by the RDRAM-bounds exit in the
            // interpreter loop.
            gbi->map[0xB5] = &op80_unknown;  // no-op
            gbi->map[0xFF] = &setColorImage_filtered;

            // Factor5 emits TEXRECTs in LLE format (16 bytes: TEXRECT + one
            // RDPHALF follow-up packing uls/ult/dsdx/dtdy together). The default
            // F3DEX HLE texrect handler reads 24 bytes (TEXRECT + RDPHALF_1 +
            // RDPHALF_2), which consumes the *next* TEXRECT as garbage follow-up
            // data. Force the LLE variants so per-glyph texture coords decode
            // correctly — without this, all text TEXRECTs render as solid white
            // blocks because dsdx/dtdy are read from the wrong word.
            gbi->map[0xE4] = &GBI_RDP::texrectLLE;
            gbi->map[0xE5] = &GBI_RDP::texrectFlipLLE;

            // REVERTED 2026-05-03: experimental tri1/tri2 mappings on 0x22/
            // 0x26/0x2A/0x2E and 0x05 were producing polygon fragments where
            // 2D content was expected (e.g. N64 logo). The opcodes' constant
            // `op|0x003400` payload doesn't decode as F3DEX vertex indices —
            // running tri1/tri2 on them just hallucinates geometry from
            // garbage data. Keep these unmapped until we have a 3D scene
            // confirmed loaded with vertex-load opcodes (op_04, etc.) firing,
            // and can correlate the actual triangle-emit opcode by ratio.
        }
    }
};
