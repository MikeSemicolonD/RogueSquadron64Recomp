//
// RT64
//

#include "rt64_gbi_f3dfactor5.h"

#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"

#include "rt64_gbi_f3dex.h"

namespace RT64 {
    namespace GBI_F3DFACTOR5 {
        // TODO: identify. Payload: w0=0x80AAAAAA w1=0.
        // Tried as raw-RDRAM sub-DL call — infinite recursion, hangs after ~200 DLs.
        // So 0x80 is NOT a G_DL-style pointer. Likely a register/state set where AAAAAA
        // is an ID or a value, not an address. Kept as a no-op until disassembled.
        void op80_unknown(State *state, DisplayList **dl) {
            // no-op
        }

        // TODO: identify. Payload is constant: w0=0x028001C0 w1=0x01FF0000 every call.
        // Looks like a fixed configuration command (maybe clipping range or segment setup).
        void op02_unknown(State *state, DisplayList **dl) {
            // no-op
        }

        void setup(GBI *gbi) {
            GBI_F3DEX::setup(gbi);

            gbi->map[0x80] = &op80_unknown;
            gbi->map[0x02] = &op02_unknown;
        }
    }
};
