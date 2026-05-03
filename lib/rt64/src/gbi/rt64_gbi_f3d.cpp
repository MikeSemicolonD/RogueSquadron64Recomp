//
// RT64
//

#include "rt64_gbi_f3d.h"

#include <cassert>
#include <cstdio>
#include <unordered_map>

#include "../include/rt64_extended_gbi.h"

#include "rt64_f3d.h"
#include "rt64_gbi_extended.h"
#include "rt64_gbi_rdp.h"

namespace RT64 {
    namespace GBI_F3D {
        void matrix(State *state, DisplayList **dl) {
            state->rsp->matrix((*dl)->w1, (*dl)->p0(16, 8));
        }

        void popMatrix(State *state, DisplayList **dl) {
            if ((*dl)->w1 == 0) {
                state->rsp->popMatrix(1);
            }
        }
        
        void moveMem(State *state, DisplayList **dl) {
            switch ((*dl)->p0(16, 8)) {
            case F3D_G_MV_VIEWPORT:
                state->rsp->setViewport((*dl)->w1);
                break;
            case F3D_G_MV_MATRIX_1:
                state->rsp->forceMatrix((*dl)->w1);
                *dl = *dl + 3;
                break;
            case F3D_G_MV_L0:
                state->rsp->setLight(0, (*dl)->w1);
                break;
            case F3D_G_MV_L1:
                state->rsp->setLight(1, (*dl)->w1);
                break;
            case F3D_G_MV_L2:
                state->rsp->setLight(2, (*dl)->w1);
                break;
            case F3D_G_MV_L3:
                state->rsp->setLight(3, (*dl)->w1);
                break;
            case F3D_G_MV_L4:
                state->rsp->setLight(4, (*dl)->w1);
                break;
            case F3D_G_MV_L5:
                state->rsp->setLight(5, (*dl)->w1);
                break;
            case F3D_G_MV_L6:
                state->rsp->setLight(6, (*dl)->w1);
                break;
            case F3D_G_MV_L7:
                state->rsp->setLight(7, (*dl)->w1);
                break;
            case F3D_G_MV_LOOKATX:
                state->rsp->setLookAt(0, (*dl)->w1);
                break;
            case F3D_G_MV_LOOKATY:
                state->rsp->setLookAt(1, (*dl)->w1);
                break;
            default: {
                // Factor5 (and likely other custom ucodes) emit G_MOVEMEM with
                // indices outside the F3D set. Asserting kills the game; instead
                // log the index once-per-value and continue. The relevant state
                // (matrix/light/etc.) just doesn't update — visually that means
                // some geometry uses stale state, which is preferable to a hard
                // abort. Useful for tracking which indices Factor5 actually uses
                // so a future RE pass can map them.
                static std::unordered_map<uint8_t, int> seen;
                uint8_t idx = (*dl)->p0(16, 8);
                int &n = seen[idx];
                if (++n <= 3) {
                    fprintf(stderr, "[gbi_f3d] moveMem unknown idx=0x%02X (n=%d) w0=0x%08X w1=0x%08X\n",
                        idx, n, (*dl)->w0, (*dl)->w1);
                    fflush(stderr);
                }
                break;
            }
            }
        }
        
        void vertex(State *state, DisplayList **dl) {
            state->rsp->setVertex((*dl)->w1, (*dl)->p0(20, 4) + 1, (*dl)->p0(16, 4));
        }

        void runDl(State *state, DisplayList **dl) {
            { static int n = 0;
              const uint32_t targetAddr = state->rsp->fromSegmentedMasked((*dl)->w1);
              const uint8_t branch = (uint8_t)((*dl)->p0(16, 1));
              if (++n <= 20 || (n % 5000) == 0) {
                  fprintf(stderr, "[trace] G_DL #%d -> 0x%08X branch=%u\n",
                      n, targetAddr, (unsigned)branch);
                  fflush(stderr);
              }
            }
            const uint32_t rdramAddress = state->rsp->fromSegmentedMasked((*dl)->w1);
            // Guard: invalid target (NULL or outside RDRAM). Reached when an
            // unmapped Factor5 opcode mis-advances dl into MIPS code memory,
            // and the interpreter parses raw instruction bytes as opcodes;
            // an instruction-encoded "op_06" (any MIPS lwc1/sw with bits[31:24]
            // == 0x06) lands here with garbage w1. Without this check we walk
            // into NULL and trigger a vector subscript out-of-range crash deep
            // in RT64. Treat as no-op (consume the cmd, don't follow).
            if (rdramAddress == 0 || rdramAddress >= 0x00800000) {
                static int n = 0;
                if (++n <= 10) {
                    fprintf(stderr, "[trace] G_DL skip-invalid #%d target=0x%08X (raw w1=0x%08X)\n",
                        n, rdramAddress, (*dl)->w1);
                    fflush(stderr);
                }
                return;
            }
            if ((*dl)->p0(16, 1) == 0) {
                state->pushReturnAddress(*dl);
            }
            *dl = reinterpret_cast<DisplayList *>(state->fromRDRAM(rdramAddress)) - 1;
        }

        void endDl(State *state, DisplayList **dl) {
            { static int n = 0;
              if (++n <= 20 || (n % 5000) == 0) {
                  fprintf(stderr, "[trace] G_ENDDL #%d\n", n);
                  fflush(stderr);
              }
            }
            *dl = state->popReturnAddress();
        }

        void sprite2DBase(State *state, DisplayList **dl) {
            // TODO
        }

        void tri1(State *state, DisplayList **dl) {
            static int n = 0;
            if (++n <= 4) {
                fprintf(stderr, "[gbi_f3d] tri1 #%d w0=0x%08X w1=0x%08X\n", n, (*dl)->w0, (*dl)->w1);
                fflush(stderr);
            }
            state->rsp->drawIndexedTri((*dl)->p1(16, 8) / 10, (*dl)->p1(8, 8) / 10, (*dl)->p1(0, 8) / 10);
        }

        void quad(State *state, DisplayList **dl) {
            static int n = 0;
            if (++n <= 4) {
                fprintf(stderr, "[gbi_f3d] quad #%d w0=0x%08X w1=0x%08X\n", n, (*dl)->w0, (*dl)->w1);
                fflush(stderr);
            }
            const uint8_t v0 = (*dl)->p1(24, 8) / 10;
            const uint8_t v1 = (*dl)->p1(16, 8) / 10;
            const uint8_t v2 = (*dl)->p1(8, 8) / 10;
            const uint8_t v3 = (*dl)->p1(0, 8) / 10;
            state->rsp->drawIndexedTri(v0, v1, v2);
            state->rsp->drawIndexedTri(v0, v2, v3);
        }

        void cullDl(State *state, DisplayList **dl) {
            // TODO
        }

        void moveWord(State *state, DisplayList **dl) {
            uint8_t type = (*dl)->p0(0, 8);
            switch (type) {
            case G_MW_MATRIX:
                assert(false);
                // TODO
                break;
            case G_MW_NUMLIGHT:
                state->rsp->setLightCount((((*dl)->w1 - 0x80000000) >> 5) - 1);
                break;
            case G_MW_CLIP:
                // TODO
                break;
            case G_MW_SEGMENT:
                state->rsp->setSegment((*dl)->p0(10, 4), (*dl)->w1);
                break;
            case G_MW_FOG:
                state->rsp->setFog((int16_t)((*dl)->p1(16, 16)), (int16_t)((*dl)->p1(0, 16)));
                break;
            case G_MW_LIGHTCOL:
                switch ((*dl)->p0(8, 16)) {
                case G_MWO_aLIGHT_1:
                    state->rsp->setLightColor(0, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_2:
                    state->rsp->setLightColor(1, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_3:
                    state->rsp->setLightColor(2, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_4:
                    state->rsp->setLightColor(3, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_5:
                    state->rsp->setLightColor(4, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_6:
                    state->rsp->setLightColor(5, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_7:
                    state->rsp->setLightColor(6, (*dl)->w1);
                    break;
                case F3D_G_MWO_aLIGHT_8:
                    state->rsp->setLightColor(7, (*dl)->w1);
                    break;
                }

                break;
            case F3D_G_MW_POINTS: 
                {
                    const uint32_t value = (*dl)->p0(8, 16);
                    state->rsp->modifyVertex(value / 40, value % 40, (*dl)->w1);
                }
                break;
            case G_MW_PERSPNORM:
                // TODO
                break;
            default:
                break;
            }
        }

        void texture(State *state, DisplayList **dl) {
            uint8_t tile = (*dl)->p0(8, 3);
            uint8_t level = (*dl)->p0(11, 3);
            uint8_t on = (*dl)->p0(0, 8);
            uint16_t sc = (*dl)->p1(16, 16);
            uint16_t tc = (*dl)->p1(0, 16);
            state->rsp->setTexture(tile, level, on, sc, tc);
        }

        void setOtherModeH(State *state, DisplayList **dl) {
            state->rsp->setOtherModeH((*dl)->p0(0, 8), (*dl)->p0(8, 8), (*dl)->w1);
        }

        void setOtherModeL(State *state, DisplayList **dl) {
            state->rsp->setOtherModeL((*dl)->p0(0, 8), (*dl)->p0(8, 8), (*dl)->w1);
        }

        void setGeometryMode(State *state, DisplayList **dl) {
            state->rsp->setGeometryMode((*dl)->w1);
        }

        void clearGeometryMode(State *state, DisplayList **dl) {
            state->rsp->clearGeometryMode((*dl)->w1);
        }

        void rdpHalf1(State *state, DisplayList **dl) {
            state->microcode.half1 = (*dl)->w1;
        }

        void rdpHalf2(State *state, DisplayList **dl) {
            state->microcode.half2 = (*dl)->w1;
        }

        void setColorImage(State *state, DisplayList **dl) {
            const uint8_t fmt = (*dl)->p0(21, 3);
            const uint8_t siz = (*dl)->p0(19, 2);
            const uint16_t width = (*dl)->p0(0, 12) + 1;
            const uint32_t address = (*dl)->w1;
            state->rsp->setColorImage(fmt, siz, width, address);
        }

        void setDepthImage(State *state, DisplayList **dl) {
            const uint32_t address = (*dl)->w1;
            state->rsp->setDepthImage(address);
        }

        void setTextureImage(State *state, DisplayList **dl) {
            const uint8_t fmt = (*dl)->p0(21, 3);
            const uint8_t siz = (*dl)->p0(19, 2);
            const uint16_t width = (*dl)->p0(0, 12) + 1;
            const uint32_t address = (*dl)->w1;
            state->rsp->setTextureImage(fmt, siz, width, address);
        }

        void reset(State *state) {
            state->rsp->setLookAtVectors(hlslpp::float3(0.0f, 1.0f, 0.0f), hlslpp::float3(1.0f, 0.0f, 0.0f));
            state->rsp->setFog(0x0100, 0x0000);
        }

        void setup(GBI *gbi) {
            gbi->constants = {
                { F3DENUM::G_MTX_MODELVIEW, 0x00 },
                { F3DENUM::G_MTX_PROJECTION, 0x01 },
                { F3DENUM::G_MTX_MUL, 0x00 },
                { F3DENUM::G_MTX_LOAD, 0x02 },
                { F3DENUM::G_MTX_NOPUSH, 0x00 },
                { F3DENUM::G_MTX_PUSH, 0x04 },
                { F3DENUM::G_TEXTURE_ENABLE, 0x00000002 },
                { F3DENUM::G_SHADING_SMOOTH, 0x00000200 },
                { F3DENUM::G_CULL_FRONT, 0x00001000 },
                { F3DENUM::G_CULL_BACK, 0x00002000 },
                { F3DENUM::G_CULL_BOTH, 0x00003000 }
            };
            
            gbi->map[F3D_G_SPNOOP] = &GBI_EXTENDED::noOpHook;
            gbi->map[F3D_G_MTX] = &matrix;
            gbi->map[F3D_G_MOVEMEM] = &moveMem;
            gbi->map[F3D_G_VTX] = &vertex;
            gbi->map[F3D_G_DL] = &runDl;
            gbi->map[F3D_G_ENDDL] = &endDl;
            gbi->map[F3D_G_SPRITE2D_BASE] = &sprite2DBase;
            gbi->map[F3D_G_TRI1] = &tri1;
            gbi->map[F3D_G_QUAD] = &quad;
            gbi->map[F3D_G_CULLDL] = &cullDl;
            gbi->map[F3D_G_POPMTX] = &popMatrix;
            gbi->map[F3D_G_MOVEWORD] = &moveWord;
            gbi->map[F3D_G_TEXTURE] = &texture;
            gbi->map[F3D_G_SETOTHERMODE_H] = &setOtherModeH;
            gbi->map[F3D_G_SETOTHERMODE_L] = &setOtherModeL;
            gbi->map[F3D_G_SETGEOMETRYMODE] = &setGeometryMode;
            gbi->map[F3D_G_CLEARGEOMETRYMODE] = &clearGeometryMode;
            gbi->map[F3D_G_RDPHALF_1] = &rdpHalf1;
            gbi->map[F3D_G_RDPHALF_2] = &rdpHalf2;
            gbi->map[G_SETCIMG] = &setColorImage;
            gbi->map[G_SETZIMG] = &setDepthImage;
            gbi->map[G_SETTIMG] = &setTextureImage;
            gbi->map[G_RDPNOOP] = &GBI_RDP::noOp;

            gbi->resetFromTask = &reset;
        }
    }
};