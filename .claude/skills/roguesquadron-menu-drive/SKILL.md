---
name: roguesquadron-menu-drive
description: Drive RogueSquadron64Recomp through its boot cinematic and front-end MENUS with scripted keyboard input to reach an interactive state the env-var demo shortcuts cannot — profile (pilot) creation, SELECT GAME, ENTER NAME, SELECT LEVEL, AVAILABLE CRAFT, the hangar, or actual in-mission gameplay — and then observe it with a window screenshot plus the ROGUESQ_LOG_GAMESTATE log. Use whenever a bug is reproduced only by navigating the real menus (e.g. "the hangar is black", "crash after selecting a mission", "ship select does X"), when a screenshot of a specific menu/hangar/mission screen is needed, or when the ROGUESQ_BOOT_TARGET / ROGUESQ_SKIP_DEMO attract-demo shortcuts (covered by roguesquadron-debug) don't reach the screen in question. Keyboard input is on by default in this build (no controller needed): keys are injected with Win32 SendInput scancodes to the focused game window (SDL reads scancodes). Provides tools/drive-input.ps1 (send a key sequence) and tools/repro-hangar.ps1 (one-shot launch + full boot->profile->level->craft->hangar navigation). Triggers on: "drive the menu", "get to the hangar/ship select/mission", "press start/create a profile/select a level", scripted keyboard input, SendInput, reproduce an interactive (non-attract) screen. Complements roguesquadron-debug (evidence-first triage; freeze signatures; taskkill) — use that for the analysis once this skill has you on the screen.
---

# RogueSquadron64Recomp — driving the front-end menus

Audience: AI agents in this repo. The `roguesquadron-debug` skill reaches **attract demos** via
env vars (`ROGUESQ_BOOT_TARGET`, `ROGUESQ_SKIP_DEMO`, `ROGUESQ_CINE_FASTFWD`). It cannot reach the
**interactive** front-end (create a pilot, pick a level, the hangar/ship select, start a mission),
because those need real button presses. This skill drives those presses. Read `AGENTS.md` first.

## Why keyboard works with no controller

Keyboard input is on by default (see project memory `project_keyboard_mouse_input_2026_09_15`):
a keyboard alone clears the NO-CONTROLLER gate. Default map (PC-port layout) — **Esc = Start,
Enter = A (confirm)**, W = A, Space/Backspace = B, S = Z, E = Rtrig, N = Ltrig, arrows/A/D =
stick, Alt = C-Left, X = C-Down, F = C-Right, Q = C-Up, C/V/B/Z = D-pad up/down/right/left.
Esc also releases mouse capture (harmless).

Input is injected with Win32 `SendInput` using **scancodes** (set 1), which is what SDL reads. The
game window must be foreground; the helper focuses it first. Window title:
`Star Wars: Rogue Squadron 64 Recompiled`.

## Tools

- `tools/drive-input.ps1 -Keys "esc:100:1500,enter:100:800"` — send a key sequence to the running
  window. Each token is `name:holdMs:afterMs` (hold/after optional, default 90/400).
- `tools/repro-hangar.ps1 [-Env "NAME=VAL;NAME2=VAL2"] [-BootMs 13000]` — **one-shot**: kill any
  running instance, launch (with `ROGUESQ_LOG_GAMESTATE=1 ROGUESQ_LOG_MENU_FIX=1` plus any `-Env`),
  wait for boot, then drive boot -> profile -> level -> craft -> **hangar** in a single input run.
  Writes the redirected log path into `dumps/hangar-session/latest.txt`.

## The front-end navigation map (USA v1.0)

From a fresh boot, this is the button path. Enter = A (confirm), Esc = Start.

1. **Boot cinematic** (attribution -> N64 logo -> Factor 5). Tap **Esc** a few times, spaced
   ~1.6 s, to skip to the title / **SELECT GAME**.
2. **SELECT GAME** (3 save slots): **Enter** on an empty slot -> ENTER NAME.
3. **ENTER NAME** (letter grid): **Enter** picks the highlighted letter (tap a couple), then
   **Esc** finishes the name -> ARE YOU SURE.
4. **ARE YOU SURE? YES/NO** (YES highlighted): **Enter** -> SELECT LEVEL.
5. **SELECT LEVEL** (holo table, e.g. "Ambush at Mos Eisley"): **Enter** -> AVAILABLE CRAFT.
6. **AVAILABLE CRAFT** (holo ships for the level): **Enter** selects the craft, then **one more
   Enter** confirms the mission -> the **hangar** loads.

The profile is not persisted between runs (fine) — recreate it each time. Timing drifts run to run
(the cinematic-skip phase is the variable part); prefer **one combined input run with fixed delays**
(as `repro-hangar.ps1` does) over press-check-press loops.

## Observe the result (evidence, not just a picture)

- Screenshot the window: the `windows-screenshot` MCP `capture_window_by_title` with
  `title: "Rogue Squadron 64 Recompiled"`, `gpu: true`. A tiny PNG (~5-10 KB, all black) is itself a
  signal the screen is black.
- Launch with `ROGUESQ_LOG_GAMESTATE=1` and read the redirected log
  (`Get-Content (cat dumps/hangar-session/latest.txt ...)`): `[gamestate]` prints screen/level/craft
  and the cutscene filename; `[cine-progress] iter=N delta=0` with a rising `idle=` means the
  cinematic loop has wedged (a hang, even though the window still repaints and is not "Not
  Responding"). The built-in watchdog then dumps every thread's stack — that, not a screenshot, is
  how you localize the hang (see `roguesquadron-debug`).

## Gotchas

- Kill cleanly with `taskkill /IM RogueSquadron64Recomp.exe /F` (never TaskStop). A leftover
  `Out-File` redirect host can keep an old log open — `repro-hangar.ps1` writes a fresh
  timestamped log each run to avoid mixing binaries' output.
- Send input only after the window exists and is focused; a missed early tap just lands on the
  cinematic (harmless) — the extra spaced Esc taps absorb boot-time variance.
