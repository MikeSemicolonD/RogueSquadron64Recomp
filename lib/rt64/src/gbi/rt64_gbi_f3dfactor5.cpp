//
// RT64
//

#include "rt64_gbi_f3dfactor5.h"

#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "rt64_gbi_f3dex.h"

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

        // Opcode 0xB5 is G_QUAD in F3DEX, but Factor5 emits it with payload
        // (w0=0xB5000000, w1=0) — w1 is the index data in F3DEX's encoding,
        // so all-zero w1 means "draw a quad with vertices 0,0,0,0" against
        // an empty vertex cache. That can't be right. Factor5 reuses 0xB5 for
        // something else (probably sync/noop). No-op until disassembled.
        void op_B5_noop(State *state, DisplayList **dl) {
            // no-op
        }

        void setup(GBI *gbi) {
            GBI_F3DEX::setup(gbi);

            gbi->map[0x80] = &op80_unknown;
            gbi->map[0x02] = &op02_unknown;
            gbi->map[0xB5] = &op_B5_noop;
        }
    }
};
