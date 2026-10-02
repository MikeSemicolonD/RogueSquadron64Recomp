# Fork-modification inventory

A categorised snapshot of what this project adds on top of `lib/rt64`,
`lib/N64ModernRuntime`, and `src/`. Use it to tell integration glue from
defensive guards from debug cruft when deciding what a change is for.

Game-logic overrides live in regen-safe `rogue_squadron.toml` hooks, not in `funcs_*.c`
(see [AGENTS.md](../AGENTS.md) "Overriding recompiled functions").

## Categories

| Code | Meaning |
|---|---|
| **A** | Integration glue or necessary support — Factor 5 ucode handlers, ROM-hash check, gamepad init, overlay loading. |
| **B** | Defensive guard over a bug whose root cause is unproven. Works empirically; each is a deferred investigation. |
| **C** | Pure debug instrumentation — log lines, env-gated traces, dump probes, watchdogs. Mostly removable or env-gated. |
| **D** | Forgotten / orphaned / superseded — disabled hypotheses, dead `if(false)` blocks, whitespace tweaks. |

## `lib/rt64` (fork at MikeSemicolonD/rt64)

### Cat A — Factor 5 ucode support

- `src/gbi/rt64_gbi_f3dfactor5.cpp` + `.h` — Factor 5 GBI backend. Custom opcodes (0x80, 0x02, 0xB0, 0xB2, 0xB4, 0xB5, 0xBF, 0xFF) need handlers absent from stock F3DEX; without it the parser misreads the stream as F3DEX and crashes.
- Bounds checks in `rt64_gbi_f3d.cpp` / `_f3dex.cpp` (G_DL / G_VTX) so drift into garbage doesn't AV.
- `src/gbi/rt64_gbi.h` / `rt64_gbi.cpp` — `GBIUCode::F3DFACTOR5` enum and ucode-hash registration; CMakeLists registration.

### Cat B — Defensive guards

| Location | Guard | Unknown bug |
|---|---|---|
| `rt64_rdp.cpp` setColorImage/setDepthImage | Reject addr < 0x100000 or ≥ 0x800000; reject width > 1024 | Why the game emits CIMGs with garbage payloads at all. |
| `rt64_rdp.cpp` loadTile/loadBlock | RDRAM bounds checks | Why tile loads sometimes have wild source addresses. |
| `rt64_present_queue.cpp` | VI-follow override modes (mode 3 = pick freshest by timestamp) | Whether the Factor 5 cinematic violates RT64's VI heuristic assumptions. |
| `rt64_state.cpp` | Combiner-stack bounds check during late cinematic | Why the cinematic workload sometimes overflows the combiner stack. |
| `rt64_framebuffer_manager.cpp` | Skip zero-width / pixelSize==0 (G_IM_SIZ_4b) framebuffers | The game registers such framebuffers during the cinematic. |
| `rt64_native_target.cpp` | Null guards on D3D12 resources + 64-bit address arithmetic | Upstream feeding bad pointers; source unidentified. |
| `rt64_gbi_f3dfactor5.cpp` chunk fetch | Per-task 4096-hop limit, chunk-revisit detection, back-link check (no global interpreter cap) | A walk that escapes the chunk list. |

### Cat C / D

- Env-gated traces (`ROGUESQ_LOG_PIPELINE` and the other `ROGUESQ_LOG_*` categories) across `rt64_rdp`, `rt64_framebuffer`, `rt64_workload_queue`, `rt64_buffer_uploader`, `rt64_present_queue`.
- Per-mux color-key diagnostics in `RasterPS.hlsl` / `RasterVS.hlsl` / `VideoInterfacePS.hlsl`; env-gated thread-naming logs.
- Cat D: `if(false) fprintf(...)` dead prints in `rt64_gbi_*`; `#if 0` shader blocks.

## `lib/N64ModernRuntime` (fork at MikeSemicolonD/N64ModernRuntime)

### Cat A — Necessary runtime extension

- Thread context magic sentinel in `ultramodern.hpp` + validation in `threads.cpp` (SEH-wrapped magic-load + VirtualQuery page-state check; detects corrupt OSThread context pointers).
- SEH exception propagation on Windows threads (`recomp.cpp`); earlier catch-and-exit lost minidumps.
- libultra stubs: `osViGetCurrentField`, pak/PFS no-pak, eep, cont rumble.
- VI mode deep-copy in `events.cpp` (Factor 5 reuses the OSViMode pointer); dummy VI mode init.

### Cat B — Defensive guards

| Location | Guard | Unknown bug |
|---|---|---|
| `rsp.hpp` DMA helpers | Clamp dram_addr to RDRAM bounds; skip IMEM-bit DMAs | Why the graphics ucode issues DMAs to dram_addr ≥ 0x800000 early in boot. |
| `dp.cpp` osDpGetCounters | 64-bit VA safety on the buffer pointer | Why uint32_t truncation AVs here. |
| `overlays.cpp` | Inverted-bounds guard on overlay section iteration | When/why overlay loads fail to match. |
| `overlays.cpp` get_function(0) | Returns a stub thunk + caller log instead of assert+exit | Why MIPS functions call `get_function(0)`; tail-return $ra leak suspected. |
| `ultra_translation.cpp` osYieldThread | `std::this_thread::yield()` instead of asserting | Cooperative-yield invariant not held. |
| `mesgqueue.cpp` do_send | Bails if `mq->msg` non-canonical or msgCount==0 | Why the message-queue struct becomes corrupt during cinematic. |

### Cat C / D

- mqdiag per-queue counters (`mqdiag_dump`), `[trace]` lines (mostly `if(false)`), `ROGUESQ_LOG_*` env-gated traces, static counters.
- Cat D: whitespace tweak in `recomp.cpp`; the LLE leftovers `g_rsp_dpc_start/end`, `rsp_dpc_submit`, `RSP_DPC_*` in `librecomp/.../rsp.hpp` and `submit_rdp_range`/`submit_rdp_range_batch`/`send_rdp_range` in `events.hpp`/`renderer_context.hpp`, which nothing calls since graphics went HLE.

## `src/` and recompiled MIPS output

- `src/main/main.cpp` — Cat A: RSP microcode dispatch (MusyX synth runner for `M_AUDTASK`), Factor 5 boot ucode DMEM setup, SDL2 audio/window/gamepad, ROM hash check. Cat B: STL bounds-check abort suppression (`_CrtSetReportHook` returns 1). Cat C: F12 minidump hotkey, DbgHelp symbolication, minidump writer, SEH + SIGABRT handlers.
- `src/main/upstream_compat.cpp` — Cat C: `ROGUESQ_DATA_BP` hardware data breakpoint (DR0/DR1 + VEH) for finding a diverging write.
- `src/main/host_api.cpp`, `host_api_game.cpp`, `host_api.h` — Cat A: the mod host API (hook ids, actions, flags, text sources, input/key/quit handlers, seal/rollback) and its game-side calls; see [modding-host-api.md](modding-host-api.md).
- `src/main/builtin_hooks.cpp` — Cat A: registers the built-in handlers for the `rogue_squadron.toml` hook sites through the host API.
- `src/main/mp_register.cpp`, `mp_host.h` — Cat A: online co-op's host-API registrations, called by the built-ins or by the `multiplayer-native` mod.
- `src/main/net_core.cpp`, `net_link.cpp` — Cat A: co-op wire protocol and ENet6/UPnP transport.
- `src/main/ghost.cpp`, `ghost_lobby.cpp`, `ghost_hud.cpp` — Cat A: ghost co-op session, front-end lobby, HUD/radar/puppet hooks; see [multiplayer.md](multiplayer.md).
- `src/main/coop_imposter.cpp` — Cat A: player 2 as an imposter wingman NPC, flown from port 1 or posed from the network.
- `src/main/lockstep.cpp`, `lockstep_core.cpp` — Cat A: input record/replay, per-frame state hashing, the relocation guard, and the ported X-wing flight model.
- NPC health hooks (`rogue_squadron.toml`, `rs64_npc_health_slot_ok` in `hook_helpers.cpp`) — Cat B: entry hooks on the NPC health/damage accessors that guard the per-difficulty slot read/write when `npc+0x190` is torn down (structure-destruction freeze, invincible AT-PTs).
- `RecompiledFuncs/funcs_*.c` carries no hand edits; it is regenerated, and overrides live in `rogue_squadron.toml` hooks.

## Candidates to leave the fork

- The Factor 5 GBI backend and VI-follow logic — could live in our repo if RT64 grew a "register a custom GBI from outside" extension API.
- Thread context magic — a defense any recomp wants; plausibly upstreamable.
