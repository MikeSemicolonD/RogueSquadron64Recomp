/* Symbol bindings for patches. Two kinds of entries:
 *
 *  1. Game-side functions/data the patches need to reference. These resolve
 *     to addresses in the original ROM. Add as we need them.
 *
 *  2. Runtime stubs at fake absolute addresses (0x8FXXXXXX). N64Recomp
 *     recognises these and emits direct calls into librecomp/host code.
 */

__start = 0x80000000;

/* --- Runtime stubs (host code callable from patches) -------------------- */
/* (none yet — populate when we need them) */

/* --- Game functions called from patches --------------------------------- */
/* heap_guards.c calls func_8002221C (alloc helper). Address from
 * syms/rogue_squadron.syms.toml. */
func_8002221C = 0x8002221C;

/* --- Game data symbols touched by patches ------------------------------- */
/* heap_guards.c reads/writes these globals. Addresses are static RAM slots
 * in the original game's BSS.
 *
 *  heap_free_head      (Block*)   the head of the heap's free list
 *  heap_other_listhead (Block**)  pointer to the active-queue head slot
 *  heap_tail_loc       (Block**)  pointer to current tail-node slot
 *  heap_bump           (void*)    bump-allocator pointer (popped + 0x100)
 */
heap_free_head      = 0x801163B0;
heap_other_listhead = 0x801163FC;
heap_tail_loc       = 0x8011A7DC;
heap_bump           = 0x801163D4;
