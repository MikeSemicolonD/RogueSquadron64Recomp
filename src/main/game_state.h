// Canonical game-state model: host-side classify + force.
// Table data is generated from state_model.toml into state_table.inl.
#pragma once
#include <cstdint>

enum { RS_OP_EQ = 0, RS_OP_NE = 1, RS_OP_RANGE = 2, RS_OP_FLAG = 3 };
enum RsForceSlot { RS_SLOT_NONE = -1, RS_SLOT_BOOT_LEVEL = 0, RS_SLOT_MENU_SCREEN = 1, RS_SLOT_DEMO = 2 };

struct RsPredicate {
    uint32_t addr;      // KSEG0 (0x80xxxxxx); read as a big-endian word starting here
    uint32_t mask;
    uint8_t  op;
    uint32_t value;
    uint32_t value_hi;
};

struct RsState {
    const char* id;
    const char* name;
    int parent;         // index into g_state_table, or -1
    int depth;
    int pred_off;
    int pred_cnt;
    int force_slot;     // RsForceSlot
    int fp_off;
    int fp_cnt;
    int rf_off;         // reachable_from pool offset (used by force reachability)
    int rf_cnt;
};

extern "C" {
    void rs64_state_poll(const uint8_t* rdram);   // reclassify; updates the published state
    int  rs64_state_current(void);                // index into g_state_table, or -1 (unknown)
    const char* rs64_state_current_id(void);      // id string, or "unknown"
    int  rs64_state_in_cinematic(void);           // 1 while a cutscene timeline plays (boot intro, in-mission cutscenes) or the state is "cinematic"

    int rs64_force_state(const char* id);         // stage a force; 0 ok, -1 unknown/no-slot
    int rs64_force_consume(int slot, int32_t* out, int out_cap);  // slot hooks call this
}
