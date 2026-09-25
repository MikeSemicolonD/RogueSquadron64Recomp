// Larger Object Pool: the shared NPC context pool (ships, lasers, missiles, trails, effects) grows from 192 to POOL_SIZE slots.
// When the pool is full the game evicts a live object to make room; vanilla level 1 already runs it dry.
// Replaces allocNpcContextArrays / destroyNpcContextArrays, the only code that depends on the slot count (everything else pushes/pops the free stack).

#define RECOMP_PATCH __attribute__((section(".recomp_patch")))

#define POOL_SIZE 384
#define CONTEXT_SIZE 0x3C

#define gNpcContextArray     (*(unsigned char**)0x80130BB8)
#define gNpcContextArrayPtrs (*(unsigned char***)0x80130BBC)
#define gNpcListA            (*(unsigned char**)0x80130BC0)
#define gNpcListB            (*(unsigned char**)0x80130BC4)
#define gNpcNextOpenSlot     (*(int*)0x80130BC8)

void* rs_malloc(unsigned int size, unsigned int flags);
void rs_free(void* ptr);
void func_8004028C(void);

// allocNpcContextArrays
RECOMP_PATCH void func_8003FD54(void) {
    int i;
    gNpcContextArray = rs_malloc(POOL_SIZE * CONTEXT_SIZE, 0);
    gNpcContextArrayPtrs = rs_malloc(POOL_SIZE * sizeof(unsigned char*), 0);
    for (i = 0; i < POOL_SIZE; i++) {
        gNpcContextArrayPtrs[i] = gNpcContextArray + i * CONTEXT_SIZE;
    }
    gNpcNextOpenSlot = POOL_SIZE - 1;
    gNpcListB = 0;
    gNpcListA = 0;
}

// destroyNpcContextArrays: destroyTransientNpcSlots until every slot is free, free both linked lists (next at +0x48), then the arrays.
RECOMP_PATCH void func_8003FDD8(void) {
    unsigned char* node;
    while (gNpcNextOpenSlot < POOL_SIZE - 1) {
        func_8004028C();
    }
    while ((node = gNpcListA) != 0) {
        gNpcListA = *(unsigned char**)(node + 0x48);
        rs_free(node);
    }
    while ((node = gNpcListB) != 0) {
        gNpcListB = *(unsigned char**)(node + 0x48);
        rs_free(node);
    }
    rs_free(gNpcContextArrayPtrs);
    rs_free(gNpcContextArray);
    gNpcContextArrayPtrs = 0;
    gNpcContextArray = 0;
}
