# Project layout

```text
src/                                    Host code; the include root, so includes are folder-qualified ("input/touch_input.h")
  ├── main/                             main.cpp (game registration, window/gfx/VI callbacks, main()), cli (command-line flags), crash (Windows crash handlers, minidumps), ui_panels (ImGui controls/mods/touch overlay), android_host, renderdoc_capture, mcp_bridge (dev-only loopback socket for tools/debug/mcp, built into Debug only unless `-DRS64_MCP=ON`)
  ├── audio/                            RSP microcode dispatch (MusyX synth runner) + SDL audio output
  ├── input/                            input_host (gamepads, joysticks, keyboard/mouse, sensors; the SDL event loop and N64 pad polling), touch_host (touch and mouse on the menus), input_bindings, rumble (motor mapping and the rumble/lightbar thread), touch_input / touch_menu / touch_config
  ├── video/                            rt64_render_context (RT64 host context; HLE send_dl, present-side fixes, boot-target driver), render_diag (env-gated traces and RDRAM dumps), video_config (settings and the ROGUESQ_* display overrides)
  ├── game/                             Code that hooks the recompiled game: hook_helpers (entry points for rogue_squadron.toml hooks), toml_hooks.c, upstream_compat (libultra shims, scheduler overrides, rs64_load_overlay), register_overlays / register_patches, game_state + nav_sequencer (state classifier, BOOT_TARGET driver), builtin_hooks, static_standins
  │   ├── menus/                        menu_config (front-end menus and mod pages), controls_menu, mods_menu, menu_font, menu_typing, pause_menu, menu_layout.h, mods_page.h
  │   ├── mod_content/                  asset_overrides (mod files appended past the ROM), mod_level_hooks (camera shots, mod levels, crawls, medals, objectives, level rules), their pure-logic headers, terrain_edit.h
  │   └── lang/                         Language mods (lang, lang_core.h)
  ├── host_api/                         Mod host API (public header include/rs64/host_api.h)
  ├── lockstep/                         Input record/replay, frame hashing and exact-frame pause points; mcp_pause (the MCP pause, step and press methods)
  └── util/                             debug_logs.h (env_* helpers), mips_call.h, rdram_words.h
mods/                                   Data-only, .nrm and native mods; mods/platforms.json picks what ships
patches/                                Base-game C patches (patches/src), built into the patches ELF on every build; needs MIPS GCC
mods/multiplayer-native/                Online co-op as a native mod, always built by the game's CMake (the exe has no multiplayer code)
  ├── src/                              mp_register.cpp + mp_host.h (host-API registrations, its only route to the game), mp_mod.cpp (rs64_mod_init)
  │   ├── session/                      ghost*.cpp (session, lobby, HUD), world_sync.cpp (objectives, damage, power-ups, triggers), tow_cable.cpp, coop_imposter.cpp
  │   ├── net/                          messages.cpp (wire format), lobby.cpp (handshake, status lines, discovery), editors.cpp (text rows), net_link.cpp (ENet6), relay_client.cpp (rs64_relay transport)
  │   └── core/                         mp_core.cpp (pure logic with unit tests: imposter flight model, puppet, lobby rules)
  ├── tests/                            mp_core_test, net_core_test, net_link_test (run by rs64_unit_tests)
  ├── mod.toml, tools/                  Manifest plus the per-player data relocation as [[patches.instruction]] (RecompModTool writes mod.json / instruction_patches.json at build time) and its generator
  └── lib/enet6/, lib/miniupnp/         Submodules for the transport, built only for this mod
relay-server/                           rs64_relay, the standalone co-op relay server (lists and pairs games, forwards packets); builds alone with only enet6, released by relay-release.yml
tests/                                  Host unit tests (rs64_unit_tests)
rogue_squadron.toml, patches.toml       N64Recomp configs (game ELF / patches ELF)
rsp/                                    RSPRecomp configs (MusyX synth, shared boot ucode)
tools/state/state_model.toml            Game-state descriptor (state_table.inl codegen + Python tools)
lib/N64ModernRuntime/                   Submodule — fork at MikeSemicolonD/N64ModernRuntime
  ├── librecomp/                        Recompiler runtime (overlay loading, get_function, SEH)
  └── ultramodern/                      libultra emulation (threads, mesgqueue, events)
lib/rt64/                               Submodule — fork at MikeSemicolonD/rt64
  └── src/gbi/rt64_gbi_f3dfactor5.cpp   Native Factor 5 GBI profile (the render core)
syms/                                   Submodule (RogueSquadron64RecompSyms): function + data symbols and the mods data variant. dump_syms rewrites them; commit inside syms/, then bump the pointer
android/, ci/                           Android Gradle project; release CI runner scripts
docs/                                   guides/ (build, mods, co-op), engine/ (debugging, env vars, host API, CI), reference/ (RE notes; its README is the on-ramp)
tools/
  ├── debug/                            Crash/hang triage, stability + leak harnesses, menu driving, validate/, lockstep/, recordings/
  ├── re/                               Reverse-engineering helpers: rz/, rename/, render/, MORT decoders, one-off disasm/decode probes
  ├── release/                          Versioning, release bundling, packaging and cross-builds (linux/, macos/, android/)
  ├── mods/, state/, style/, d3d11/     Build inputs read by CMake/CI (mod build scripts, state model, style baseline, D3D11 shader combos)
  └── web/                              Browser tools (rs64-explorer/, its data generator, save-editor/), deployed to GitHub Pages by .github/workflows/pages.yml on pushes to main that touch the site (the landing page is the root index.html)
build/                                  CMake out-of-source build dir
```

The recompiled MIPS code lives in-repo at `RecompiledFuncs/` — `funcs_*.c` plus `funcs.h`, `lookup.cpp`, and `recomp_overlays.inl`. It is **gitignored** (a derivative of the copyrighted ROM; generated locally, never committed) and regenerated from `rogue_squadron.toml` with `cmake --build build --config Debug --target regen_funcs`; the next build re-globs automatically (`CONFIGURE_DEPENDS`). Regen reads the decomp ELF at `../rogue_squadron64/build/roguesquadron.elf`, and rebuilds `N64RecompCLI` first. N64Recomp fails the regen if any patch (including the co-op relocation in `patch_files`) names a missing function or vram. These are auto-generated; hand-instrumenting them with diagnostic `fprintf` probes is routine, but load-bearing logic belongs in `rogue_squadron.toml` hooks (see [AGENTS.md](../../AGENTS.md#overriding-recompiled-functions)), not here — regeneration silently discards inline edits.

The forked submodules under `lib/` carry intentional `if(false) fprintf(...)` debug-toggle cruft and game-specific defensive guards. **Do not propose stripping these** as cleanup; they are intentional.
