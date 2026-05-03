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

        // Factor5 emits one G_SETCIMG (0xFF) per render-pass with a bogus
        // payload (w0=0xFFF00F0F, w1=0x00000000) immediately before the
        // overlay TEXRECT batch. That zeroes RDP::colorImage.address, so the
        // subsequent TEXRECTs render onto a null target and produce no
        // visible output. Real SETCIMG calls always carry a non-zero w1.
        // Filter out the bogus form here.
        void setColorImage_filtered(State *state, DisplayList **dl) {
            if ((*dl)->w1 == 0) {
                return;
            }
            GBI_F3D::setColorImage(state, dl);
        }


        void setup(GBI *gbi) {
            GBI_F3DEX::setup(gbi);

            gbi->map[0x80] = &op80_unknown;
            gbi->map[0x02] = &op02_unknown;
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

            // EXPERIMENTAL: Factor5 emits a family of unknown opcodes with the
            // pattern op|0x003400 in both w0 and w1, where the opcode byte
            // itself appears to encode an operand (bits [6:2]). Frequency
            // analysis from the broken-loop opFreq dump suggested:
            //   op 0x2A, 0x2E (~12K each) — most frequent, plausibly tri2/quad
            //   op 0x26 (~5K)             — paired tri-style command
            //   op 0x12, 0x36, 0x3A (~2K each) — plausibly G_VTX variants
            // Try routing them to F3DEX's tri1/tri2 handlers as a probe — if
            // any geometry appears that wasn't there before, we have a hint.
            // Worst case: they remain effectively no-ops (degenerate triangles
            // since vertex indices decode to small/zero values).
            gbi->map[0x2A] = &GBI_F3DEX::tri1;
            gbi->map[0x2E] = &GBI_F3DEX::tri1;
            gbi->map[0x26] = &GBI_F3DEX::tri2;
            gbi->map[0x22] = &GBI_F3DEX::tri2;
        }
    }
};
