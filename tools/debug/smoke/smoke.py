"""Visual + stat smoke test: every target in manifest.toml captured on exact frames, then compared with a baseline run.

  python tools/debug/smoke/smoke.py run [--tag current] [--sections levels,changes,menus,pause,cutscenes,perf] [--filter level00] [--build Release] [--speed 16] [--platforms windows]
  python tools/debug/smoke/smoke.py compare [--baseline baseline] [--tag current] [--platforms windows]
  python tools/debug/smoke/smoke.py clean

Runs the Release exe from a staged copy in build/Smoke with the pinned profile in tools/debug/smoke/profile, never the user's own settings or save;
the profile and save are reset before every launch. Output goes to dumps/smoke/<tag>/<platform>/<target>/<clock>-<frame:06>[_<change>].png plus a .json with
the game state, draw counts, frame times and (in missions) player, camera, wingmen and NPC positions. A target that fails to run leaves error.txt.
Make a baseline with `run --tag baseline`. Frame times in the other sections run at --speed; the perf section measures each repo video preset at real speed.
"""
import argparse
import html
import json
import os
import re
import shutil
import struct
import sys
import time
import tomllib
import traceback

from PIL import Image, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "mcp"))

from rs64_mcp import REPO, Game, symbol_table  # noqa: E402

BUILDS = os.path.join(REPO, "build")
STAGE = os.path.join(REPO, "build", "Smoke")
PROFILE = os.path.join(HERE, "profile")
OUT = os.path.join(REPO, "dumps", "smoke")
# invincibility: an unflown craft would otherwise crash (the Trench Run walls) and stop the chain on a game over.
MODS = ["display-settings", "controls-menu", "quit-prompt", "larger_object_pool.nrm", "invincibility.nrm"]
MOD_IDS = ["display-settings", "controls-menu", "quit-prompt", "larger_object_pool", "invincibility"]
SECTIONS = ["levels", "changes", "menus", "pause", "cutscenes", "perf"]
# The launch config each platform runs (rs64_mcp Game.launch); None = no launcher yet.
PLATFORMS = {"windows": "Smoke", "linux": None, "android": None}
config = PLATFORMS["windows"]

PLAYER0 = 0x80137DB8
NPC_SLOTS = 0x80130BB0
# distanceFromCameraToPoint reads the camera's world position at *0x80138D1C + 0x4C.
CAMERA_PTR = 0x80138D1C
# An NPC context's first word is its update function.
WINGMAN_UPDATE = symbol_table()["npcWingmanUpdate"]
PIXEL_DELTA = 64
CONTROL = "control"
FAULTS = ("Assertion", "Access violation", "SEH caught", "null-call", "[hle-seh]")
MENU_DONE = r"\[nav\] menu: path done"
NAV_STUCK = r"\[nav\] stuck"


def stage(build):

    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)

    os.makedirs(STAGE)

    src_dir = os.path.join(BUILDS, build)

    for name in os.listdir(src_dir):
        if name.endswith((".dll", ".z64")) or name in ("RogueSquadron64Recomp.exe", "RogueSquadron64Recomp.pdb"):
            shutil.copy2(os.path.join(src_dir, name), STAGE)

    for mod in MODS:
        src = os.path.join(src_dir, "mods", mod)
        dst = os.path.join(STAGE, "mods", mod)

        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)

    with open(os.path.join(STAGE, "mods.json"), "w") as f:
        json.dump({"enabled_mods": MOD_IDS, "mod_order": MOD_IDS}, f, indent=4)


def reset_profile():
    """Every launch starts from the pinned settings and no save: a menu target that flips a setting, or a level that makes a pilot, can't leak into the next."""

    for name in os.listdir(STAGE):
        if name.startswith("roguesq_") and name.endswith(".json"):
            os.remove(os.path.join(STAGE, name))

    for name in ("saves", "mod_config"):
        shutil.rmtree(os.path.join(STAGE, name), ignore_errors=True)

    os.makedirs(os.path.join(STAGE, "saves"))

    for name in os.listdir(PROFILE):
        shutil.copy2(os.path.join(PROFILE, name), STAGE)


def read(game, addr, size):

    return bytes.fromhex(game.call("rdram_read", {"addr": addr, "size": size})["hex"])


def ptr_ok(p):

    return 0x80000000 <= p < 0x80800000


def stats(game):

    p = read(game, PLAYER0, 0x2A0)
    inner = 4
    out = {
        "player": {
            "active": p[0],
            "pos": list(struct.unpack(">3f", p[inner + 4:inner + 16])),
            "health": list(struct.unpack(">2f", p[inner + 0xC0:inner + 0xC8])),
            "vehicle": p[0x290],
        },
        "camera": None,
        "wingmen": [],
        "npcs": [],
    }

    cam = struct.unpack(">I", read(game, CAMERA_PTR, 4))[0]

    if ptr_ok(cam):
        out["camera"] = list(struct.unpack(">3f", read(game, cam + 0x4C, 12)))

    table = struct.unpack(">I", read(game, NPC_SLOTS, 4))[0]

    if not ptr_ok(table):
        return out

    slots = read(game, table, 0x800 * 8)
    seen = set()

    # Several slots can share one NPC context; each context is listed once.
    for i in range(0x800):
        ctx = struct.unpack(">I", slots[i * 8:i * 8 + 4])[0]

        if not ptr_ok(ctx) or ctx in seen:
            continue

        seen.add(ctx)

        c = read(game, ctx, 0x3C)
        pos_ptr = struct.unpack(">I", c[8:12])[0]
        pos = list(struct.unpack(">3f", read(game, pos_ptr, 12))) if ptr_ok(pos_ptr) else None
        npc = {"slot": i, "ctx": f"{ctx:08X}", "update": c[0:4].hex(), "kind": c[0x14:0x1A].hex(), "pos": pos}
        out["npcs"].append(npc)

        if struct.unpack(">I", c[0:4])[0] == WINGMAN_UPDATE:
            out["wingmen"].append(npc)

    return out


def shot(game, out_dir, name, mission):

    base = os.path.join(out_dir, name)
    game.screenshot(base + ".png")
    summary = game.call("frame_summary")
    times = game.call("frame_times")
    record = {
        "state": game.call("frame_info").get("state"),
        "draws": summary["draws"],
        "triangles": summary["triangles"],
        "frame_times": {k: times[k] for k in ("present", "renderer_cpu", "renderer_gpu") if k in times},
    }

    if mission:
        record.update(stats(game))

    with open(base + ".json", "w") as f:
        json.dump(record, f, indent=1)


def session(out, env, body, speed, record=None, settings=None):
    """One launch: body(game) moves game.target_dir along as it shoots. A failure, or faults in the log, is written into the target it was on; returns False on failure."""

    reset_profile()
    game = Game()
    game.target_dir = out

    # A 16:9 window, so WIDESCREEN has room to widen the picture.
    full_env = {"ROGUESQ_SPEED": str(speed), "ROGUESQ_FAKE_CONTROLLER": "1", "ROGUESQ_WINDOW_SIZE": "1280x720", "ROGUESQ_MENU_FIXED": "1", "ROGUESQ_SUBTITLES": "1"}
    full_env.update(env)
    error = None

    # The game can't create the recording in a missing folder, and then runs without the fixed step.
    os.makedirs(out, exist_ok=True)

    try:
        game.launch(config, full_env, record=record, settings=settings)
        body(game)
    except Exception:
        error = traceback.format_exc()
    finally:
        game.close()

    os.makedirs(game.target_dir, exist_ok=True)

    with open(game.log_path, "rb") as f:
        lines = f.read().decode("utf-8", "replace").splitlines()

    # A fault line and the stack frames ("  [ n] ...") printed under it.
    bad = []
    in_stack = False

    for line in lines:
        if any(k in line for k in FAULTS):
            in_stack = True
        elif not (in_stack and line.startswith("  [")):
            in_stack = False
            continue

        bad.append(line)

    if bad:
        with open(os.path.join(game.target_dir, "faults.txt"), "a") as f:
            f.write("\n".join(bad) + "\n")

    if error:
        with open(os.path.join(game.target_dir, "error.txt"), "a") as f:
            f.write(error + "\n" + game.log_tail(60) + "\n")

    for leftover in (record, record and record + ".hash"):
        if leftover and os.path.exists(leftover):
            os.remove(leftover)

    shutil.rmtree(game.session_dir, ignore_errors=True)
    print(f"{'ERROR at' if error else 'done, last'} {os.path.relpath(game.target_dir, OUT)}", flush=True)

    return error is None


def enter(game, target_dir):

    game.target_dir = target_dir
    os.makedirs(target_dir, exist_ok=True)


def hold(game, clock, shots, mission, after_restart=False):
    """Hold on each (frame, suffix, action) of `clock` in turn; action(game), if any, runs on the held frame before stepping on. Shots are named mission-<frame> on the ls clock."""

    prev = None
    name = "mission" if clock == "ls" else clock

    for frame, suffix, action in shots:
        if action:
            action(game)

        if prev is None:
            game.call("pause_at", {"clock": clock, "frame": frame, "timeout_s": 240, "next": after_restart}, 255)
        else:
            game.call("step", {"frames": frame - prev, "clock": clock, "timeout_s": 120}, 135)

        shot(game, game.target_dir, f"{name}-{frame:06d}{suffix}", mission)
        prev = frame

    game.call("resume")


# Lockstep frames a pause page takes to settle after a press.
PAUSE_SETTLE = 40


def tap(game, buttons, wait):
    """Press for 2 frames from a held frame, then hold again `wait` lockstep frames later."""

    game.call("press", {"buttons": buttons, "frames": 2})
    game.call("step", {"frames": wait, "clock": "ls", "timeout_s": 120}, 135)


def pause_settings_cycle(game, at, pages, skip):
    """Held at lockstep frame `at`, pause and open each pause page path (as in pause_chain), then step every setting row through all its values with A,
    one shot per value (pause-<page>-<key>-NN), until it is back where it started; B and START return to the game. Rows in `skip` are left alone."""

    game.call("pause_at", {"clock": "ls", "frame": at, "timeout_s": 240}, 255)

    # START can land on a fly-in or skip-intro prompt instead of pausing, and a page can take an extra press to close, so go by the page the game reports.
    for _ in range(8):
        if game.call("pause_menu", {})["page"] == 0:
            break

        tap(game, ["start"], PAUSE_SETTLE)
    else:
        raise RuntimeError("the pause menu did not open")

    for page in pages:
        path = [int(s) if s.isdigit() else s for s in str(page).split("/")]

        for step in path:
            before = game.call("pause_menu", {})["page"]

            for _ in range(4):
                game.call("pause_menu", {"page": step})
                tap(game, ["a"], PAUSE_SETTLE)

                if game.call("pause_menu", {})["page"] != before:
                    break
            else:
                raise RuntimeError(f"A did not open {step} on the way to {page}")

        rows = game.call("pause_menu", {})["rows"]

        for k, row in enumerate(rows):
            key = row["key"]

            if not key or key in skip:
                continue

            start = game.call("setting", {}).get(key, {}).get("value")
            game.call("pause_menu", {"entry": k})

            for n in range(24):
                tap(game, ["a"], PAUSE_SETTLE)
                shot(game, game.target_dir, f"pause-{str(page).replace('/', '-')}-{key}-{n:02d}", False)

                if game.call("setting", {}).get(key, {}).get("value") == start:
                    break

        for _ in range(len(path) + 4):
            if game.call("pause_menu", {})["page"] == 0:
                break

            tap(game, ["b"], PAUSE_SETTLE)
        else:
            raise RuntimeError(f"B did not return to the pause root from {page}")

    tap(game, ["start"], PAUSE_SETTLE)
    game.call("resume")


def pause_chain(out, level, at, pages, frames, speed):
    """Every pause page in one fixed-step mission: held on lockstep frame `at`, START opens the root, and each page path ("1/display": a page id or mod page
    name per level) is opened from the root on held frames (MCP pause_menu sets the cursor, then A) and shot `frames` lockstep frames later; B per level goes back
    to the root. The ls clock runs on while paused, so every shot is frame-exact."""

    settle = PAUSE_SETTLE
    press = tap

    def body(game):

        game.call("pause_at", {"clock": "ls", "frame": at, "timeout_s": 240}, 255)
        press(game, ["start"], settle)

        for page in pages:
            path = [int(s) if s.isdigit() else s for s in str(page).split("/") if s != "0"]

            enter(game, os.path.join(out, f"pause_{level}_{str(page).replace('/', '-')}"))

            for i, step in enumerate(path):
                game.call("pause_menu", {"page": step})
                press(game, ["a"], frames[0] if i == len(path) - 1 else settle)

            if not path:
                game.call("step", {"frames": frames[0], "clock": "ls", "timeout_s": 120}, 135)

            shot(game, game.target_dir, f"pause-{frames[0]:06d}", False)

            for prev, frame in zip(frames, frames[1:]):
                game.call("step", {"frames": frame - prev, "clock": "ls", "timeout_s": 120}, 135)
                shot(game, game.target_dir, f"pause-{frame:06d}", False)

            for _ in path:
                press(game, ["b"], settle)

    session(out, {"ROGUESQ_BOOT_TARGET": f"level:{level}"}, body, speed, os.path.join(out, "pause.rec"))


def wait_nav(game, pattern, from_start=False):

    line = game.wait_log(f"{pattern}|{NAV_STUCK}", timeout_s=120, from_start=from_start)["line"]

    if "stuck" in line:
        raise RuntimeError(f"nav sequence stuck: {line}")


def step_setting(game, key, value):
    """Step a setting until it reads `value` (a slider is set directly)."""

    for _ in range(16):
        if game.call("setting", {})[key]["value"] == value:
            return

        try:
            game.call("setting", {"key": key})
        except Exception as e:
            if "slider" not in str(e):
                raise

            game.call("setting", {"key": key, "value": value})

    raise RuntimeError(f"{key} did not return to {value}")


def change_chain(specs, at, settle=30):
    """Shots for several setting changes in one mission: change i is applied at at + 60i, shot `settle` frames later, changed back and shot again.
    The control run (specs = [CONTROL] * n) shoots the same frames without changing anything, for the visual leak check."""

    shots = []
    original = {}

    for i, spec in enumerate(specs):
        keys = [] if spec == CONTROL else spec.split("+")
        t = at + i * 2 * settle

        def apply(game, keys=keys):

            values = game.call("setting", {})

            for key in keys:
                original[key] = values[key]["value"]
                game.call("setting", {"key": key})

        def restore(game, keys=keys):

            for key in reversed(keys):
                step_setting(game, key, original[key])

        shots.append((t + settle, f"_{spec}", apply))
        shots.append((t + 2 * settle, f"_{spec}_restored", restore))

    return shots


def level_chain(out, ids, frames, speed, pause_pages=(), pause_skip=()):
    """Every level in one launch: each mission is held for its shots, its pause settings pages cycled (pause_settings_cycle), then aborted to SELECT LEVEL
    and the next one started (ROGUESQ_NAV_CHAIN). A level that fails is skipped and the chain relaunched at the one after it."""

    remaining = list(ids)

    while remaining:
        done = []

        def body(game, levels=list(remaining)):

            for i, level in enumerate(levels):
                enter(game, os.path.join(out, f"level{level:02d}"))
                hold(game, "ls", [(f, "", None) for f in frames], True, after_restart=i > 0)

                if pause_pages:
                    pause_settings_cycle(game, frames[-1] + 30, pause_pages, pause_skip)

                done.append(level)

                if i + 1 < len(levels):
                    game.call("nav", {"next_level": levels[i + 1]})

        env = {"ROGUESQ_BOOT_TARGET": f"abort:{remaining[0]}", "ROGUESQ_NAV_CHAIN": "1", "ROGUESQ_LS_EACH_MISSION": "1"}
        session(out, env, body, speed, os.path.join(out, "levels.rec"))
        remaining = remaining[len(done) + 1:]


def hold_nav(game, frames, after):
    """Shoot frames of the "nav" clock (game frames since the path's final A press); after = the clock's restart count before that press, None for the first."""

    params = {"clock": "nav", "frame": frames[0], "timeout_s": 120}

    if after is not None:
        params["after"] = after

    game.call("pause_at", params, 135)
    shot(game, game.target_dir, f"nav-{frames[0]:06d}", False)

    for prev, frame in zip(frames, frames[1:]):
        game.call("step", {"frames": frame - prev, "clock": "nav", "timeout_s": 120}, 135)
        shot(game, game.target_dir, f"nav-{frame:06d}", False)

    game.call("resume")


def menu_chain(out, paths, frames, speed, cheats=""):
    """Every menu path in one launch: back to the main menu between paths, and any setting a path pressed put back first.
    Shots count from the final A press, and ROGUESQ_MENU_FIXED pins the backdrop and the animation timing, so menu shots repeat exactly. A failed path is skipped and the chain relaunched."""

    remaining = list(paths)

    while remaining:
        done = []

        def body(game, todo=list(remaining)):

            snapshot = game.call("setting", {})

            for i, path in enumerate(todo):
                enter(game, os.path.join(out, "menu_" + path.lower().replace("/", "__").replace(" ", "_").replace("*", "")))

                if i == 0:
                    hold_nav(game, frames, None)
                else:
                    current = game.call("setting", {})

                    for key, v in snapshot.items():
                        if current[key]["value"] != v["value"]:
                            step_setting(game, key, v["value"])

                    after = game.call("frame_info")["restarts"].get("nav", 0)
                    game.call("nav", {"menu": path})
                    hold_nav(game, frames, after)

                done.append(path)

        session(out, {"ROGUESQ_BOOT_TARGET": f"menu:{remaining[0]}", "ROGUESQ_CHEATS": cheats}, body, speed)
        remaining = remaining[len(done) + 1:]


def cutscene_chain(out, cheats, viewer_path, frames, speed):
    """Every cutscene in one launch: the DIRECTOR unlock (ROGUESQ_CHEATS) makes AT THE MOVIES play them all in a row; each is shot and named by its place in that order.
    Ends when no further cutscene starts or the viewer gives way to the attract demos. Frames past a cutscene's end are not shot (the hold would land in the next one)."""

    def body(game):

        # The viewer takes over before the nav sees a menu change, so "path done" never comes: wait for its parent page, then hold from the next cutscene start.
        enter(game, os.path.join(out, "cutscene_viewer"))
        parent = viewer_path.split("/")[-2]
        wait_nav(game, rf"\[nav\] menu: selected {re.escape(parent)}$", from_start=True)
        base = game.call("frame_info")["restarts"].get("cutscene", 0)

        while True:
            end = None

            for k, frame in enumerate(frames):
                if end is not None and frame >= end:
                    break

                info = game.call("frame_info")
                restart = k == 0 and "cutscene" in info["clocks"]

                if k == 0 and info["state"] == "menu.main":
                    print("viewer done: menu.main", flush=True)

                    return

                try:
                    held = game.call("pause_at", {"clock": "cutscene", "frame": frame, "timeout_s": 60, "next": restart}, 75)
                except Exception as e:
                    if "timeout" in str(e) and k == 0:
                        print("viewer done: no cutscene started within 60 s", flush=True)

                        return

                    raise

                # After the last movie the viewer returns to the main menu, which later idles into the attract demos (DEMO_ACTIVE, bit 5 of gGameSettings+0x10).
                state = game.call("frame_info")["state"]

                if state == "menu.main" or struct.unpack(">I", read(game, 0x80130B50, 4))[0] & 0x20:
                    print(f"viewer done: {state}", flush=True)
                    game.call("resume")

                    return

                # gCurrentCutsceneFile (0x800B1904) +0x44 is the end frame; the game stops 0xA short of it.
                if end is None:
                    end = struct.unpack(">I", read(game, struct.unpack(">I", read(game, 0x800B1904, 4))[0] + 0x44, 4))[0] - 0xA

                seq = held["picture"]["restarts"].get("cutscene", 0) - base
                enter(game, os.path.join(out, f"cutscene_seq_{seq:02d}"))
                shot(game, game.target_dir, f"cutscene-{frame:06d}", False)
                game.call("resume")

    # A menu boot target fast-forwards every cinematic (for the boot intro), which would skip through the viewer too.
    env = {"ROGUESQ_BOOT_TARGET": f"menu:{viewer_path}", "ROGUESQ_CHEATS": cheats, "ROGUESQ_CINE_FASTFWD": "0"}
    session(out, env, body, speed)


def run(args):

    with open(os.path.join(HERE, "manifest.toml"), "rb") as f:
        m = tomllib.load(f)

    global config

    config = PLATFORMS[args.platform]
    stage(args.build)
    out = os.path.join(OUT, args.tag, args.platform)
    sections = args.sections.split(",")
    wanted = (lambda name: any(f in name for f in args.filter.split(",")))

    def one_level(level, shots, pause_pages=()):

        def body(game):

            enter(game, os.path.join(out, f"level{level:02d}"))
            hold(game, "ls", shots, True)

            if pause_pages:
                pause_settings_cycle(game, shots[-1][0] + 30, pause_pages, m["levels"].get("pause_skip", ()))

        session(out, {"ROGUESQ_BOOT_TARGET": f"level:{level}"}, body, args.speed, os.path.join(out, "level.rec"))

    # Level 20 (CHICKEN) skips the front end, so it can't be reached from SELECT LEVEL and gets a launch of its own.
    if "levels" in sections:
        ids = [i for i in m["levels"]["ids"] if wanted(f"level{i:02d}")]
        level_chain(out, [i for i in ids if i != 20], m["levels"]["frames"], args.speed, m["levels"].get("pause_pages", ()), m["levels"].get("pause_skip", ()))

        if 20 in ids:
            one_level(20, [(f, "", None) for f in m["levels"]["frames"]], m["levels"].get("pause_pages", ()))

    # Per level: the control run, then all visual changes, then all sim changes, each one launch.
    if "changes" in sections:
        c = m["changes"]
        n = max(len(c["visual"]), len(c["sim"]))

        for level in [i for i in c["levels"] if wanted(f"level{i:02d}")]:
            for specs in ([CONTROL] * n, c["visual"], c["sim"]):
                one_level(level, change_chain(specs, c["at"]))

    if "menus" in sections:
        paths = [p for p in m["menus"]["paths"] if wanted("menu_" + p.lower())]

        if paths:
            menu_chain(out, paths, m["menus"]["frames"], args.speed, m["menus"].get("cheats", ""))

    if "pause" in sections:
        p = m["pause"]
        pages = [page for page in p["pages"] if wanted(f"pause_{p['level']}_{page}")]

        if pages:
            pause_chain(out, p["level"], p["at"], pages, p["frames"], args.speed)

    # Without the passcode, one launch per cutscene frame: the cutscene clock runs on wall time, so a step after a hold lands late.
    if "cutscenes" in sections:
        cs = m["cutscenes"]

        if cs.get("cheats"):
            cutscene_chain(out, cs["cheats"], cs["viewer_path"], cs["frames"], args.speed)
        else:
            for scene in cs["ids"]:
                name = f"cutscene_{scene.replace(',', '_')}"

                for frame in cs["frames"] if wanted(name) else []:
                    def cutscene_body(game, name=name, frame=frame):

                        enter(game, os.path.join(out, name))
                        hold(game, "cutscene", [(frame, "", None)], False)

                    session(out, {"ROGUESQ_BOOT_TARGET": f"cutscene:{scene}"}, cutscene_body, args.speed)

    if "perf" in sections:
        p = m["perf"]

        for level in p["levels"]:
            for preset in p["presets"]:
                if wanted(f"perf_level{level:02d}_{preset}"):
                    perf_run(out, level, preset, p["at"], p["seconds"])


def perf_run(out, level, preset, at, seconds):
    """One real-speed launch with a repo video preset merged over the pinned profile: held on mission frame `at` to line the runs up, then run `seconds` and RT64's last
    120 frame times read into perf_level<id>_<preset>/perf.json."""

    with open(os.path.join(REPO, "video-presets", preset, "roguesq_video.json"), encoding="utf-8") as f:
        settings = json.load(f)

    def body(game):

        enter(game, os.path.join(out, f"perf_level{level:02d}_{preset}"))
        game.call("pause_at", {"clock": "mission", "frame": at, "timeout_s": 240}, 255)
        shot(game, game.target_dir, f"mission-{at:06d}", True)
        game.call("resume")

        # A wall-clock wait is right here: the frame times are of the real-speed run, not of any one frame.
        time.sleep(seconds)

        times = game.call("frame_times")

        with open(os.path.join(game.target_dir, "perf.json"), "w") as f:
            json.dump({k: times[k] for k in ("present", "renderer_cpu", "renderer_gpu") if k in times}, f, indent=1)

    session(out, {"ROGUESQ_BOOT_TARGET": f"level:{level}", "ROGUESQ_FIXED_STEP": "0"}, body, 1, settings=settings)


def image_diff(a, b, diff_path=None):

    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")

    if ia.size != ib.size:
        return 100.0

    d = ImageChops.difference(ia, ib)
    bands = d.split()
    m = ImageChops.lighter(ImageChops.lighter(bands[0], bands[1]), bands[2]).point(lambda v: 255 if v > PIXEL_DELTA else 0)
    over = m.histogram()[255]

    if diff_path and over:
        os.makedirs(os.path.dirname(diff_path), exist_ok=True)
        Image.composite(Image.new("RGB", ia.size, (255, 0, 255)), ImageChops.multiply(ib, Image.new("RGB", ib.size, (90, 90, 90))), m).save(diff_path)

    return 100.0 * over / (ia.size[0] * ia.size[1])


# Per target kind: the most pixels (percent) that may change by more than PIXEL_DELTA (None = not compared), and the sidecar fields that must match.
# Missions and pause pages (fixed step, ls clock) and menus (ROGUESQ_MENU_FIXED, nav clock) match exactly. Cutscenes match except animated water and
# ground textures and subtitles, which follow the voice stream (measured up to 15%), so their limit only catches a wrong camera, scene or model.
RULES = {
    "level": (0.05, ["state", "player", "camera", "wingmen", "npcs"]),
    "cutscene": (20, ["state"]),
    "menu": (0.05, ["state"]),
    "pause": (0.05, ["state"]),
    # Real-speed runs: the picture follows the wall clock, only perf.json is compared (PERF_SLOWER).
    "perf": (None, ["state"]),
}
STAT_FIELDS = ["player", "camera", "wingmen", "npcs"]
# A preset whose average frame time grew by more than this fraction fails.
PERF_SLOWER = 0.15


def load_json(path):

    with open(path) as f:
        return json.load(f)


def visual_leaks(run_dir, visual):
    """A visual-only change must leave the game exactly where the unchanged control run is at the same frames."""

    problems = {}

    for root, _, files in os.walk(run_dir):
        for name in files:
            spec = next((v for v in visual if name.endswith((f"_{v}.json", f"_{v}_restored.json"))), None)

            if spec is None:
                continue

            frame = name.split("_", 1)[0]
            control = os.path.join(root, f"{frame}_{CONTROL}{'_restored' if name.endswith('_restored.json') else ''}.json")

            if not os.path.exists(control):
                continue

            a = load_json(control)
            b = load_json(os.path.join(root, name))
            moved = [k for k in STAT_FIELDS if a.get(k) != b.get(k)]

            if moved:
                problems[os.path.relpath(os.path.join(root, name[:-5] + ".png"), run_dir)] = f"{spec} moved the game ({', '.join(moved)})"

    return problems


def compare(args):

    with open(os.path.join(HERE, "manifest.toml"), "rb") as f:
        visual = tomllib.load(f)["changes"]["visual"]

    base = os.path.join(OUT, args.baseline, args.platform)
    cur = os.path.join(OUT, args.tag, args.platform)
    leaks = visual_leaks(cur, visual)
    rows = []

    for root, dirs, files in os.walk(base):
        dirs[:] = [d for d in dirs if not d.startswith("_")]

        for name in sorted(files):
            if not name.endswith(".png"):
                continue

            rel = os.path.relpath(os.path.join(root, name), base)
            other = os.path.join(cur, rel)
            kind = next(k for k in RULES if rel.startswith(k))
            limit, fields = RULES[kind]
            problems = []
            pct = None

            if not os.path.exists(other):
                problems.append("missing")
            else:
                pct = image_diff(os.path.join(base, rel), other, os.path.join(cur, "_diff", rel))

                if limit is not None and pct > limit:
                    problems.append(f"pixels {pct:.3f}% > {limit}%")

                ja = load_json(os.path.join(base, rel[:-4] + ".json"))
                jb = load_json(other[:-4] + ".json")
                problems += [f"{k} differs" for k in fields if ja.get(k) != jb.get(k)]

            if rel in leaks:
                problems.append(leaks[rel])

            rows.append((rel, pct, problems))

    for root, _, files in os.walk(cur):
        for name in ("error.txt", "faults.txt"):
            if name in files:
                rows.append((os.path.relpath(os.path.join(root, name), cur), None, [name[:-4]]))

    for root, _, files in os.walk(base):
        if "perf.json" not in files:
            continue

        rel = os.path.relpath(os.path.join(root, "perf.json"), base)
        before = load_json(os.path.join(base, rel)).get("present", {}).get("avg")
        after = load_json(os.path.join(cur, rel)).get("present", {}).get("avg") if os.path.exists(os.path.join(cur, rel)) else None

        if not before or not after:
            rows.append((rel, None, ["missing"]))
            continue

        slower = after > before * (1 + PERF_SLOWER)
        rows.append((f"{rel} ({1000 / before:.0f} -> {1000 / after:.0f} fps)", None, [f"frame time +{after / before - 1:.0%}"] if slower else []))

    failed = 0

    for rel, pct, problems in rows:
        failed += bool(problems)
        print(f"{'FAIL' if problems else 'ok  '} {rel:60} {'; '.join(problems)}")

    report(cur, base, rows)
    print(f"{args.platform}: {failed} of {len(rows)} failed; report: {os.path.join(cur, 'report.html')}")

    return failed


def report(cur, base, rows):

    def ft(path):

        if not os.path.exists(path):
            return ""

        t = load_json(path).get("frame_times", {}).get("present")

        return f"{1000.0 / t['avg']:.0f} fps (p99 {t['p99']:.1f} ms)" if t and t.get("avg") else ""

    rel_base = os.path.relpath(base, cur).replace("\\", "/")
    lines = [
        "<!doctype html><meta charset=utf-8><title>Smoke report</title>",
        "<style>body{font:14px sans-serif;background:#111;color:#ddd}img{width:32%;margin:2px}.f{color:#f66}.o{color:#6c6}</style>",
        f"<h1>Smoke report</h1><p>{sum(bool(r[2]) for r in rows)} of {len(rows)} failed. Baseline, current, changed pixels (magenta). FPS is meaningful only at --speed 1.</p>",
    ]

    for rel, pct, problems in sorted(rows, key=lambda r: (not r[2], r[0])):
        url = rel.replace("\\", "/")
        status = f"<span class=f>FAIL: {html.escape('; '.join(problems))}</span>" if problems else "<span class=o>ok</span>"
        lines.append(f"<h3>{html.escape(url)} {status}</h3>")

        if url.endswith(".png"):
            fps = ft(os.path.join(cur, rel[:-4] + ".json"))
            lines.append(f"<p>{f'{pct:.3f}% pixels changed' if pct is not None else ''} {fps}</p>")
            diff = f"<img src='_diff/{url}'>" if os.path.exists(os.path.join(cur, "_diff", rel)) else ""
            lines.append(f"<img src='{rel_base}/{url}'><img src='{url}'>{diff}")

    with open(os.path.join(cur, "report.html"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():

    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--tag", default="current")
    r.add_argument("--sections", default=",".join(SECTIONS), help="any of " + ",".join(SECTIONS))
    r.add_argument("--filter", default="", help="only targets whose folder name contains one of these (comma list), e.g. level00 or level16,level17")
    r.add_argument("--build", default="Release", help="build folder to stage the exe from (Debug gives symbolized fault stacks, ~10x slower)")
    r.add_argument("--speed", type=int, default=16)
    c = sub.add_parser("compare")
    c.add_argument("--baseline", default="baseline")
    c.add_argument("--tag", default="current")

    for p in (r, c):
        p.add_argument("--platforms", default="windows", help="comma list of " + ",".join(PLATFORMS))

    sub.add_parser("clean", help="delete dumps/smoke")
    args = ap.parse_args()

    if args.cmd == "clean":
        shutil.rmtree(OUT, ignore_errors=True)

        return

    platforms = args.platforms.split(",")

    for name in platforms:
        if name not in PLATFORMS:
            sys.exit(f"unknown platform {name}; known: {', '.join(PLATFORMS)}")

        if PLATFORMS[name] is None:
            sys.exit(f"{name} has no launcher yet")

    failed = 0

    for name in platforms:
        args.platform = name

        if args.cmd == "run":
            run(args)
        else:
            failed += compare(args)

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
