"""MCP server that launches Rogue Squadron 64 Recompiled and drives its RT64 debug bridge (docs/engine/rt64-mcp.md).

Run as a stdio MCP server (Claude Code .mcp.json), or `python rs64_mcp.py --self-test` against a Debug build.
"""

import http.client
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tmem_decode  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
RENDERDOC_DLL = r"E:\Projects\renderdoc\x64\Release\renderdoc.dll"
WAIT_LOG_FAIL = r"SEH caught|null-call|Assertion failed|Failed to find function"
WAIT_LOG_MAX_S = 120


class GameError(Exception):
    pass


_symbols = None


def symbol_table():
    """name -> vram from syms/ (data and function symbols)."""
    global _symbols
    if _symbols is None:
        import tomllib
        _symbols = {}
        for name in ("rogue_squadron.datasyms.toml", "rogue_squadron.syms.toml"):
            with open(os.path.join(REPO, "syms", name), "rb") as f:
                for section in tomllib.load(f).get("section", []):
                    for sym in section.get("symbols", []) + section.get("functions", []):
                        _symbols.setdefault(sym["name"], sym["vram"])
    return _symbols


def write_minidump(pid, path):
    """MiniDumpWithFullMemory of pid, as tools/debug/dump-game.ps1 writes (game threads and RDRAM included)."""
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.windll.kernel32
    dbghelp = ctypes.windll.dbghelp
    kernel32.OpenProcess.restype = wintypes.HANDLE
    dbghelp.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    process = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not process:
        raise GameError(f"could not open pid {pid}")
    try:
        with open(path, "wb") as f:
            import msvcrt
            if not dbghelp.MiniDumpWriteDump(process, pid, msvcrt.get_osfhandle(f.fileno()), 0x2, None, None, None):
                raise GameError(f"MiniDumpWriteDump failed ({ctypes.GetLastError()})")
    finally:
        kernel32.CloseHandle(process)


def capture_window(pid):
    """RGBA bytes, width, height of the largest visible top-level window of pid (PrintWindow full content, BitBlt if that is black)."""
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            rect = wintypes.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(rect))
            found.append((rect.right * rect.bottom, hwnd, rect.right, rect.bottom))
        return True

    user32.EnumWindows(enum, 0)
    if not found or max(found)[0] == 0:
        raise GameError(f"no visible window for pid {pid}")
    _, hwnd, w, h = max(found)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG), ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                    ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD), ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", wintypes.LONG),
                    ("biYPelsPerMeter", wintypes.LONG), ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]

    HDC, HGDI = wintypes.HDC, wintypes.HGDIOBJ
    user32.GetDC.restype = HDC
    user32.GetDC.argtypes = [wintypes.HWND]
    user32.ReleaseDC.argtypes = [wintypes.HWND, HDC]
    user32.PrintWindow.argtypes = [wintypes.HWND, HDC, wintypes.UINT]
    gdi32.CreateCompatibleDC.restype = HDC
    gdi32.CreateCompatibleDC.argtypes = [HDC]
    gdi32.CreateDIBSection.restype = wintypes.HBITMAP
    gdi32.CreateDIBSection.argtypes = [HDC, ctypes.c_void_p, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
    gdi32.SelectObject.restype = HGDI
    gdi32.SelectObject.argtypes = [HDC, HGDI]
    gdi32.DeleteObject.argtypes = [HGDI]
    gdi32.DeleteDC.argtypes = [HDC]
    gdi32.BitBlt.argtypes = [HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, HDC, ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    window_dc = user32.GetDC(hwnd)
    mem_dc = gdi32.CreateCompatibleDC(window_dc)
    info = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    bits = ctypes.c_void_p()
    bitmap = gdi32.CreateDIBSection(mem_dc, ctypes.byref(info), 0, ctypes.byref(bits), None, 0)
    old = gdi32.SelectObject(mem_dc, bitmap)
    try:
        user32.PrintWindow(hwnd, mem_dc, 3)
        raw = ctypes.string_at(bits, w * h * 4)
        if not any(raw[i] or raw[i + 1] or raw[i + 2] for i in range(0, len(raw), 4 * 97)):
            gdi32.BitBlt(mem_dc, 0, 0, w, h, window_dc, 0, 0, 0x00CC0020)
            raw = ctypes.string_at(bits, w * h * 4)
    finally:
        gdi32.SelectObject(mem_dc, old)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(mem_dc)
        user32.ReleaseDC(hwnd, window_dc)

    rgba = bytearray(raw)
    rgba[0::4], rgba[2::4] = raw[2::4], raw[0::4]
    rgba[3::4] = b"\xff" * (w * h)
    return bytes(rgba), w, h


class Game:
    def __init__(self):
        self.proc = None
        self.sock = None
        self.port = 0
        self.session_dir = None
        self.log_path = None
        self.next_id = 1
        self.pending = {}
        self.lock = threading.Lock()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def status(self):
        return {
            "running": self.alive(),
            "pid": self.proc.pid if self.proc else 0,
            "port": self.port,
            "exit_code": None if (self.proc is None or self.alive()) else self.proc.returncode,
            "log_path": self.log_path,
            "session_dir": self.session_dir,
        }

    def log_tail(self, lines=40):
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return ""

    def launch(self, config="Debug", env=None, args=None, timeout_s=60, record=None):
        if self.alive():
            raise GameError(f"game already running (pid {self.proc.pid}); close it first")

        exe_dir = os.path.join(REPO, "build", config)
        exe = os.path.join(exe_dir, "RogueSquadron64Recomp.exe")
        if not os.path.isfile(exe):
            raise GameError(f"no build at {exe}")

        self.session_dir = os.path.join(REPO, "dumps", "mcp", time.strftime("%Y%m%d_%H%M%S"))
        os.makedirs(self.session_dir, exist_ok=True)
        self.log_path = os.path.join(self.session_dir, "game.log")

        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]

        full_env = dict(os.environ)
        full_env["ROGUESQ_HLE_DEV_MODE"] = "1"
        if os.path.isfile(RENDERDOC_DLL):
            full_env["ROGUESQ_RENDERDOC"] = RENDERDOC_DLL
            full_env["ROGUESQ_RENDERDOC_OUT"] = os.path.join(REPO, "dumps", "RenderDoc", "mcp-" + os.path.basename(self.session_dir), "cap")
        if record:
            full_env["ROGUESQ_LS_RECORD"] = os.path.abspath(record)
        full_env.update({k: str(v) for k, v in (env or {}).items()})

        cmd = [exe, "--mcp-port", str(self.port), "--mute", "--unfocused"] + list(args or [])
        log = open(self.log_path, "wb")
        self.proc = subprocess.Popen(cmd, cwd=exe_dir, env=full_env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        log.close()

        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if not self.alive():
                tail = self.log_tail()
                if "unknown option '--mcp-port'" in tail:
                    raise GameError(f"the {config} build has no MCP bridge; configure with -DRS64_MCP=ON (or use Debug) and rebuild")
                raise GameError(f"game exited (code {self.proc.returncode}) during launch:\n{tail}")
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                break
            except OSError:
                time.sleep(0.25)
        else:
            self.close()
            raise GameError(f"bridge did not answer within {timeout_s}s:\n{self.log_tail()}")

        self.sock.settimeout(None)
        threading.Thread(target=self._reader, args=(self.sock,), daemon=True).start()
        self.call("ping", {}, timeout_s=10)
        return self.status()

    def _reader(self, sock):
        f = sock.makefile("r", encoding="utf-8")
        try:
            for line in f:
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                with self.lock:
                    slot = self.pending.get(msg.get("id"))
                if slot is not None:
                    slot["reply"] = msg
                    slot["event"].set()
        except OSError:
            pass

    def call(self, method, params=None, timeout_s=30):
        if self.sock is None:
            raise GameError("game not launched")

        with self.lock:
            rid = self.next_id
            self.next_id += 1
            slot = {"event": threading.Event(), "reply": None}
            self.pending[rid] = slot

        try:
            line = json.dumps({"id": rid, "method": method, "params": params or {}}) + "\n"
            try:
                self.sock.sendall(line.encode("utf-8"))
            except OSError:
                pass

            deadline = time.time() + timeout_s
            while not slot["event"].wait(0.05):
                if not self.alive():
                    code = self.proc.returncode if self.proc else None
                    raise GameError(f"game exited (code {code}):\n{self.log_tail()}")
                if time.time() > deadline:
                    raise GameError(f"{method}: no reply within {timeout_s}s")
        finally:
            with self.lock:
                self.pending.pop(rid, None)

        reply = slot["reply"]
        if not reply.get("ok"):
            raise GameError(f"{method}: {reply.get('error')}")
        return reply.get("result")

    def dump_texture(self, fb, proj, call, tile=0):
        """Dump one draw tile's TMEM and decode it: returns the .tmem, .json and .png paths."""
        info = self.call("get_draw", {"fb": fb, "proj": proj, "call": call})
        tiles = info["tile_list"]
        if not 0 <= tile < len(tiles):
            raise GameError(f"tile must be in [0, {len(tiles)})")
        name = tiles[tile].get("tmem_hash") or tiles[tile].get("tile_copy_id", "tile")
        path = os.path.join(self.session_dir, "tex", f"{name}.tmem")
        r = self.call("dump_tmem", {"fb": fb, "proj": proj, "call": call, "tile": tile, "path": path})
        png = tmem_decode.decode_file(r["path"])
        return {"tmem_path": r["path"], "json_path": r["json_path"], "png_path": png, "meta": r["meta"]}

    def screenshot(self, path=None):
        """PNG of the game window's client area, found by this server's own PID; returns path, size and distinct colour count."""
        if not self.alive():
            raise GameError("game not running")
        rgba, w, h = capture_window(self.proc.pid)
        if path is None:
            dl = self.call("frame_info")["clocks"].get("dl", 0)
            path = os.path.join(self.session_dir, "shots", f"{dl}_{int(time.time() * 1000) % 100000}.png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmem_decode.write_png(path, rgba, w, h)
        colours = len(set(rgba[i:i + 4] for i in range(0, min(len(rgba), 4 * 65536), 4)))
        return {"path": path, "width": w, "height": h, "colours": colours}

    def read_symbol(self, name, size=4):
        """Read a data or function symbol from syms/ by name."""
        addr = symbol_table().get(name)
        if addr is None:
            raise GameError(f"no symbol named {name}")
        return {"name": name, "addr": addr, **self.call("rdram_read", {"addr": addr, "size": size})}

    def walk_dl(self, addr="task"):
        """Dump RDRAM and walk a display list offline with tools/debug/validate/f5_dl_walk.py (default: the current gfx task)."""
        dl = self.call("frame_info")["clocks"]["dl"]
        dump = os.path.join(self.session_dir, f"rdram_dl{dl}.bin")
        self.call("rdram_dump", {"path": dump}, 60)
        out = os.path.join(self.session_dir, f"dl_walk_{dl}_{addr}.txt")
        walker = os.path.join(REPO, "tools", "debug", "validate", "f5_dl_walk.py")
        r = subprocess.run([sys.executable, walker, dump, addr], capture_output=True, text=True, timeout=300)
        with open(out, "w", encoding="utf-8") as f:
            f.write(r.stdout + r.stderr)
        return {"walk_path": out, "dump_path": dump, "exit_code": r.returncode, "tail": (r.stdout + r.stderr)[-3000:]}

    def wait_log(self, pattern, timeout_s=30, from_start=False, fail_pattern=WAIT_LOG_FAIL):
        """Wait for a regex to appear in the game's log (stdout+stderr): new lines only unless from_start. A fail_pattern line ends the wait
        with an error at once; timeout_s is capped at WAIT_LOG_MAX_S."""
        regex = re.compile(pattern)
        fail = re.compile(fail_pattern) if fail_pattern else None
        timeout_s = min(timeout_s, WAIT_LOG_MAX_S)
        offset = 0 if from_start else os.path.getsize(self.log_path)
        deadline = time.time() + timeout_s
        while True:
            with open(self.log_path, "rb") as f:
                f.seek(offset)
                chunk = f.read()
            # Only whole lines; a partial last line is read again next time.
            done = chunk.rfind(b"\n") + 1
            offset += done
            for line in chunk[:done].decode("utf-8", errors="replace").splitlines():
                if regex.search(line):
                    return {"line": line, "running": self.alive()}
                if fail and fail.search(line):
                    raise GameError(f"fail_pattern matched before {pattern!r}: {line}")
            if time.time() > deadline:
                raise GameError(f"no log line matched {pattern!r} within {timeout_s}s")
            if not self.alive():
                raise GameError(f"game exited (code {self.proc.returncode}) before {pattern!r}:\n{self.log_tail()}")
            time.sleep(0.25)

    def hang_dump(self):
        """Full-memory minidump of the running game, then tools/debug/reconstruct-freeze.py (game threads, queues) and host-stacks.py (symbolized host stacks; Debug only)."""
        if not self.alive():
            raise GameError("game not running")
        path = os.path.join(self.session_dir, f"hang_{time.strftime('%H%M%S')}.dmp")
        write_minidump(self.proc.pid, path)
        report = {"dump_path": path}
        for name, script in (("freeze", "reconstruct-freeze.py"), ("host_stacks", "host-stacks.py")):
            r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "debug", script), path], capture_output=True, text=True, timeout=600, cwd=REPO)
            text_path = os.path.join(self.session_dir, f"{os.path.splitext(os.path.basename(path))[0]}_{name}.txt")
            with open(text_path, "w", encoding="utf-8") as f:
                f.write(r.stdout + r.stderr)
            report[name] = {"path": text_path, "tail": (r.stdout + r.stderr)[-4000:]}
        return report

    def kill_external(self):
        subprocess.run(["taskkill", "/F", "/PID", str(self.proc.pid)], capture_output=True)

    def disconnect(self):
        """Hang up on the bridge (shutdown first: the reader's makefile keeps a plain close() from ending the connection)."""
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
            self.sock = None

    def close(self):
        self.disconnect()
        if self.alive():
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass
        return self.status()


def self_test():
    game = Game()
    try:
        r = game.launch()
        assert r["running"] and r["pid"] > 0, r
        assert game.call("rt64_status", {})["paused"] is False
        game.kill_external()
        t0 = time.time()
        try:
            game.call("rt64_status", {})
            raise AssertionError("call after kill succeeded")
        except GameError as e:
            assert "game exited" in str(e), e
        assert time.time() - t0 < 1.0, time.time() - t0
    finally:
        game.close()

    game = Game()
    try:
        game.launch(env={"ROGUESQ_BOOT_TARGET": "level:0"})
        dl0 = game.call("frame_info")["clocks"]["dl"]
        expect_error(lambda: game.call("list_draws"), "not paused")
        t0 = time.time()
        expect_error(lambda: game.call("pause_at", {"clock": "nonexistent", "frame": 1, "timeout_s": 5}, timeout_s=15), "timeout")
        assert 4.5 < time.time() - t0 < 8, time.time() - t0
        assert game.call("frame_info")["clocks"]["dl"] > dl0

        frozen = game.call("pause_at", {"clock": "mission", "frame": 300}, timeout_s=180)["picture"]
        assert frozen["clock"] == "mission" and frozen["frame"] == 300, frozen
        info = game.call("frame_info")
        assert info["held"] and info["rt64_paused"], info
        expect_error(lambda: game.call("pause_at", {"clock": "mission", "frame": 400}), "already paused")
        time.sleep(1)
        assert game.call("frame_info")["clocks"]["dl"] == frozen["dl_seq"]

        assert len(game.call("rdram_read", {"addr": 0x80130B70, "size": 4})["hex"]) == 8
        expect_error(lambda: game.call("rdram_read", {"addr": 0x7FFFFFFF, "size": 1}), "0x80000000")
        expect_error(lambda: game.call("rdram_read", {"addr": 0x81000000, "size": 1}), "outside valid RAM")
        game.call("rdram_read", {"addr": 0x80C00000, "size": 4})
        expect_error(lambda: game.call("rdram_read", {"addr": 0x80000000, "size": 65537}), "size")
        expect_error(lambda: game.call("rdram_write", {"addr": 0x80130B70, "hex": "abc"}), "even-length")
        original = game.call("rdram_read", {"addr": 0x80130B70, "size": 4})["hex"]
        game.call("rdram_write", {"addr": 0x80130B70, "hex": "12345678"})
        assert game.call("rdram_read", {"addr": 0x80130B70, "size": 4})["hex"] == "12345678"
        game.call("rdram_write", {"addr": 0x80130B70, "hex": original})
        assert game.call("rdram_read", {"addr": 0x80130B70, "size": 4})["hex"] == original

        summary = game.call("frame_summary")
        assert summary["draws"] > 0, summary
        fb, proj = next((f["index"], p["index"]) for f in summary["fb_pairs"] for p in f["projections"] if p["draws"] > 0)
        count = summary["fb_pairs"][fb]["projections"][proj]["draws"]
        listed = game.call("list_draws", {"fb": fb, "proj": proj})
        assert listed["total"] == count and len(listed["rows"]) == min(count, 500), listed["total"]
        for bad in (count, -1, 1.5):
            expect_error(lambda: game.call("get_draw", {"fb": fb, "proj": proj, "call": bad}), f"[0, {count})")
        draw = game.call("get_draw", {"fb": fb, "proj": proj, "call": 0, "vertices": True})
        assert "tile_list" in draw and "combiner" in draw and "vertices" in draw, draw.keys()
        assert len(draw["rdp"]["prim_color"]) == 4, draw["rdp"]
        shaders = game.call("get_draw", {"fb": fb, "proj": proj, "call": 0, "shader_dir": os.path.join(game.session_dir, "shaders")})["shader_files"]
        assert os.path.getsize(shaders["pixel"]) > 0 and os.path.getsize(shaders["vertex"]) > 0, shaders
        assert isinstance(game.call("frame_warnings")["warnings"], list)
        assert isinstance(game.call("sprite_commands")["commands"], list)
        assert {"fog", "lights", "look_at", "modify"} <= set(game.call("rsp_state"))
        vi = game.call("vi_info")
        assert vi["width"] > 0 and vi["fb_size"][0] > 0, vi

        user = game.call("get_config")["user"]
        flipped = game.call("set_config", {"user": {"fxaa": not user["fxaa"]}})["user"]
        assert flipped["fxaa"] != user["fxaa"]
        assert game.call("set_config", {"user": {"fxaa": user["fxaa"]}})["user"]["fxaa"] == user["fxaa"]
        expect_error(lambda: game.call("set_config", {"user": {"notASetting": 1}}), "unknown setting")
        features = game.call("rt_features")
        toggled = game.call("rt_features", {"set": {"lights": not features["lights"]}})
        assert toggled["lights"] != features["lights"], (features, toggled)
        game.call("rt_features", {"set": {"lights": features["lights"]}})
        expect_error(lambda: game.call("rt_features", {"set": {"bogus": True}}), "unknown feature")
        toggles = game.call("render_toggles")
        assert game.call("render_toggles", {"texture_replacement": not toggles["texture_replacement"]})["texture_replacement"] != toggles["texture_replacement"]
        game.call("render_toggles", {"texture_replacement": toggles["texture_replacement"]})

        dump_path = os.path.join(game.session_dir, "rdram.bin")
        assert game.call("rdram_dump", {"path": dump_path})["bytes"] == 0x800000
        with open(dump_path, "rb") as f:
            f.seek(0x130B70)
            assert f.read(4).hex() == game.call("rdram_read", {"addr": 0x80130B70, "size": 4})["hex"]

        center = [vi["fb_size"][0] / 2, vi["fb_size"][1] / 2]
        picked = game.call("pick", {"x": center[0], "y": center[1]})["draws"]
        assert picked and {"fb", "proj", "call"} <= set(picked[0]), picked
        expect_error(lambda: game.call("pick", {"x": "a"}), "x and y")
        assert game.read_symbol("gCurrentLevel")["hex"] == game.call("rdram_read", {"addr": 0x80130B70, "size": 4})["hex"]
        expect_error(lambda: game.read_symbol("noSuchSymbol"), "no symbol")
        walked = game.walk_dl()
        assert walked["exit_code"] == 0 and os.path.getsize(walked["walk_path"]) > 0, walked["tail"]
        assert "dl_addr" in game.call("frame_summary")["fb_pairs"][0]
        assert "client connected" in game.wait_log(r"\[mcp\] client connected", 5, from_start=True)["line"]
        expect_error(lambda: game.wait_log("never-printed-xyz", 1), "no log line")
        expect_error(lambda: game.wait_log("never-printed-xyz", 5, from_start=True, fail_pattern=r"\[mcp\] client connected"), "fail_pattern")
        expect_error(lambda: game.call("pause_when", {"addr": 0x80128EAE, "size": 3}), "size must be")
        expect_error(lambda: game.call("pause_when", {"addr": 0x80128EAE, "op": "bogus"}), "op must be")
        expect_error(lambda: game.call("pause_when", {"addr": 0x80128EAE}), "already paused")

        expect_error(lambda: game.call("press", {"buttons": ["bogus"]}), "unknown button")
        game.call("press", {"buttons": ["a"], "stick": [0, 1], "frames": 2})
        assert game.call("frame_info")["input"]["buttons"] == ["a"]
        held_from = game.call("step", {"frames": 3})["picture"]
        assert game.call("frame_info")["input"] is None, "press did not expire after its frames"
        game.call("press", {"buttons": ["start"]})
        game.call("release")
        assert game.call("frame_info")["input"] is None
        frozen = held_from

        rows = game.call("list_draws", {"limit": 5000})["rows"]
        textured = next(r for r in rows if r["tiles"] > 0 and r["tris"] > 4)
        dumped = game.dump_texture(textured["fb"], textured["proj"], textured["call"])
        meta = dumped["meta"]
        with open(dumped["tmem_path"], "rb") as f:
            rgba, w, h = tmem_decode.decode(f.read(), meta)
        assert (w, h) == (meta["width"], meta["height"]) and w * h > 0, meta
        assert len(set(rgba[i:i + 4] for i in range(0, len(rgba), 4))) > 1, "uniform texture"
        assert os.path.getsize(dumped["png_path"]) > 0
        expect_error(lambda: game.call("dump_tmem", {"fb": textured["fb"], "proj": textured["proj"], "call": textured["call"], "tile": 99, "path": "x"}), "tile must be")

        shot_a = game.screenshot()
        time.sleep(2)
        shot_b = game.screenshot()
        assert shot_a["width"] > 0 and shot_a["height"] > 0 and shot_a["colours"] > 1, shot_a
        assert open(shot_a["path"], "rb").read() == open(shot_b["path"], "rb").read(), "frozen screenshots differ"
        target_draw = game.call("pick", {"x": center[0], "y": center[1]})["draws"][0]
        game.call("highlight", {k: target_draw[k] for k in ("fb", "proj", "call")})
        time.sleep(0.5)
        assert open(game.screenshot()["path"], "rb").read() != open(shot_a["path"], "rb").read(), "highlight did not tint the picture"
        game.call("highlight", {"clear": True})
        time.sleep(0.5)
        assert open(game.screenshot()["path"], "rb").read() == open(shot_a["path"], "rb").read(), "clearing the highlight did not restore the picture"
        fb_view = game.call("view_framebuffer", {"index": 0})
        assert fb_view["index"] == 0, fb_view
        time.sleep(0.5)
        assert open(game.screenshot()["path"], "rb").read() != open(shot_a["path"], "rb").read(), "framebuffer view did not change the picture"
        game.call("view_framebuffer", {"index": -1})
        expect_error(lambda: game.call("view_framebuffer", {"index": 999}), "index must be")
        cam = game.call("free_cam", {"enabled": True})
        assert cam["enabled"] and all(abs(v) < 1e7 for v in cam["position"]), cam
        moved = game.call("free_cam", {"move": [0, 0, 1000]})
        delta = [b - a for a, b in zip(cam["position"], moved["position"])]
        assert 990 < sum(d * d for d in delta) ** 0.5 < 1010, delta
        assert sum(d * f for d, f in zip(delta, cam["forward"])) > 0, (delta, cam["forward"])
        time.sleep(0.5)
        assert open(game.screenshot()["path"], "rb").read() != open(shot_a["path"], "rb").read(), "free cam move did not change the picture"
        target = [p + f * 500 for p, f in zip(moved["position"], moved["forward"])]
        placed = game.call("free_cam", {"position": cam["position"], "target": target})
        assert all(abs(a - b) <= 1e-3 * max(1.0, abs(a)) for a, b in zip(placed["position"], cam["position"])), placed
        aim = [t - p for t, p in zip(target, cam["position"])]
        aim_len = sum(a * a for a in aim) ** 0.5
        assert sum(a / aim_len * f for a, f in zip(aim, placed["forward"])) > 0.999, (aim, placed["forward"])
        assert not game.call("free_cam", {"enabled": False})["enabled"]
        time.sleep(0.5)
        assert open(game.screenshot()["path"], "rb").read() == open(shot_a["path"], "rb").read(), "disabling free cam did not restore the picture"

        before = game.call("lights_list")["presets"]
        bright = {"enabled": True, "level": -2, "lights": [{"name": "l", "enabled": True, "description": {"position": [0, 0, 0], "attenuationRadius": 1e7, "diffuseColor": [8, 8, 8]}}]}
        game.call("lights_set", {"name": "mcp_selftest", "preset": bright})
        time.sleep(0.5)
        assert open(game.screenshot()["path"], "rb").read() == open(shot_a["path"], "rb").read(), "a light edit changed the frozen frame"
        assert game.call("lights_list")["presets"]["mcp_selftest"]["lights"][0]["name"] == "l"
        expect_error(lambda: game.call("lights_set", {"name": "bad", "preset": {"lights": 5}}), "malformed preset")
        expect_error(lambda: game.call("lights_set", {"name": "bad", "preset": {}}), "lights array")
        assert "bad" not in game.call("lights_list")["presets"]
        saved = game.call("lights_save", {"path": os.path.join(game.session_dir, "lights.json")})["path"]
        with open(saved, encoding="utf-8") as f:
            assert any(p["name"] == "mcp_selftest" for p in json.load(f)["presets"])
        assert game.call("lights_remove", {"name": "mcp_selftest"})["removed"]
        assert not game.call("lights_remove", {"name": "mcp_selftest"})["removed"]
        assert game.call("lights_list")["presets"] == before

        game.call("view_framebuffer", {"index": 0})
        game.call("view_draw", {"index": 3})
        game.call("free_cam", {"enabled": True})
        game.call("texture_dumping", {"dir": os.path.join(game.session_dir, "texdump")})
        game.call("resume")
        time.sleep(0.5)
        status = game.call("rt64_status")
        assert (status["framebuffer_index"], status["draw_index"], status["free_cam"], status["texture_dumping"]) == (-1, -1, False, False), status
        time.sleep(10)
        times = game.call("frame_times", {"samples": True})
        assert all(series["count"] > 0 for name, series in times.items() if name != "matching"), {k: v["count"] for k, v in times.items()}
        assert all(len(series.get("samples", [])) == series["count"] for series in times.values())
        assert 30.3 < times["present"]["avg"] < 36.3, times["present"]
        watched = game.call("pause_when", {"addr": 0x80128EAE, "size": 1, "op": "changes"}, timeout_s=30)["picture"]
        assert watched["watch"]["value"] != watched["watch"]["was"], watched
        game.call("resume")
        frozen = game.call("pause_at", {"clock": "dl", "frame": game.call("frame_info")["clocks"]["dl"] + 5})["picture"]

        rdc = game.call("renderdoc_capture", {"frames": 1}, timeout_s=40)["path"]
        assert rdc.endswith(".rdc") and os.path.isfile(rdc), rdc

        stepped = game.call("step", {"frames": 1})["picture"]
        assert stepped["dl_seq"] == frozen["dl_seq"] + 1, (frozen, stepped)
        stepped = game.call("step", {"frames": 2, "clock": "mission"})["picture"]
        assert stepped["clocks"]["mission"] == frozen["clocks"]["mission"] + 3, (frozen, stepped)

        game.call("resume")
        time.sleep(1)
        info = game.call("frame_info")
        assert not info["held"] and not info["rt64_paused"] and info["clocks"]["dl"] > stepped["dl_seq"], info

        threading.Timer(2.0, game.kill_external).start()
        t0 = time.time()
        expect_error(lambda: game.call("pause_at", {"clock": "nonexistent", "frame": 1, "timeout_s": 60}, timeout_s=90), "game exited")
        assert time.time() - t0 < 3.0, time.time() - t0
    finally:
        game.close()
    game = Game()
    try:
        game.launch(env={"ROGUESQ_BOOT_TARGET": "level:0"})
        game.disconnect()
        time.sleep(0.5)
        # What a browser fetch() puts on the wire: HTTP headers, then a JSON line in the body. The bridge must hang up before the body runs.
        browser = http.client.HTTPConnection("127.0.0.1", game.port, timeout=5)
        data, closed = b"", False
        try:
            browser.request("POST", "/", body='{"id":5,"method":"ping","params":{}}\n', headers={"Content-Type": "text/plain"})
            while True:
                chunk = browser.sock.recv(4096)
                if not chunk:
                    closed = True
                    break
                data += chunk
        except ConnectionError:
            closed = True
        except socket.timeout:
            pass
        browser.close()
        assert closed and b"pong" not in data and b"busy" not in data, ("an HTTP request reached the bridge", data)

        reconnect(game)
        target = game.call("frame_info")["clocks"]["dl"] + 60
        threading.Thread(target=lambda: swallow(lambda: game.call("pause_at", {"clock": "dl", "frame": target}, timeout_s=20)), daemon=True).start()
        time.sleep(0.3)
        game.disconnect()
        time.sleep(5)
        reconnect(game)
        info = game.call("frame_info")
        assert not info["held"] and not info["rt64_paused"], ("a pause pending at disconnect stuck", info)
        dl = info["clocks"]["dl"]
        time.sleep(1)
        assert game.call("frame_info")["clocks"]["dl"] > dl
    finally:
        game.close()
    print("self-test OK")


def reconnect(game):
    game.disconnect()
    game.sock = socket.create_connection(("127.0.0.1", game.port), timeout=5)
    game.sock.settimeout(None)
    threading.Thread(target=game._reader, args=(game.sock,), daemon=True).start()


def swallow(fn):
    try:
        fn()
    except GameError:
        pass


def expect_error(fn, text):
    try:
        fn()
    except GameError as e:
        assert text in str(e), e
        return
    raise AssertionError(f"expected an error containing {text!r}")


def serve():
    from mcp.server.fastmcp import FastMCP

    mcp = FastMCP("rs64")
    game = Game()

    @mcp.tool()
    def launch(config: str = "Debug", env: dict | None = None, args: list[str] | None = None, timeout_s: float = 60, record: str | None = None) -> dict:
        """Launch the game (cwd = build/<config>, RenderDoc injected, muted, dev mode) and connect to its MCP bridge. env adds ROGUESQ_* variables, e.g. {"ROGUESQ_BOOT_TARGET": "level:0"}. record=<path.rec> records the next mission's input (including press) as a replayable recording (ROGUESQ_LS_RECORD; needs a level boot target)."""
        return game.launch(config, env, args, timeout_s, record)

    @mcp.tool()
    def close() -> dict:
        """Kill the launched game and return its final status."""
        return game.close()

    @mcp.tool()
    def status() -> dict:
        """Game process status: running, pid, port, exit_code, log_path (stdout+stderr) and session_dir (outputs)."""
        return game.status()

    @mcp.tool()
    def call(method: str, params: dict | None = None, timeout_s: float = 30) -> dict:
        """Send any bridge method directly (for methods without a dedicated tool)."""
        return game.call(method, params, timeout_s)

    @mcp.tool()
    def pause_at(clock: str, frame: int, timeout_s: float = 120) -> dict:
        """Hold the game so the picture is exactly `frame` of `clock`, then freeze RT64 on it. Clocks: a game state id (mission, menu.main, ...; frames since entered), cutscene, crawl, or dl (display-list sequence since boot)."""
        return game.call("pause_at", {"clock": clock, "frame": frame, "timeout_s": timeout_s}, timeout_s + 15)

    @mcp.tool()
    def pause_when(addr: int, size: int = 4, op: str = "changes", value: int | None = None, timeout_s: float = 120) -> dict:
        """Freeze when an RDRAM value (KSEG0 addr, 1/2/4 bytes) changes, or is eq/ne/gt/lt `value`; checked once a frame. The picture includes the value seen. For a game state change use pause_at(state, 0)."""
        params = {"addr": addr, "size": size, "op": op, "timeout_s": timeout_s}
        if value is not None:
            params["value"] = value
        return game.call("pause_when", params, timeout_s + 15)

    @mcp.tool()
    def wait_log(pattern: str, timeout_s: float = 30, from_start: bool = False, fail_pattern: str = WAIT_LOG_FAIL) -> dict:
        """Wait until a regex appears in the game's log; new lines only unless from_start. Ends early with an error when the game exits or
        a fail_pattern line appears (default: crash markers; pass a line that proves the awaited one will not come, or "" for none).
        timeout_s is capped at 120: drive the game toward the event (press, step) and wait again rather than waiting long."""
        return game.wait_log(pattern, timeout_s, from_start, fail_pattern)

    @mcp.tool()
    def step(frames: int = 1, clock: str = "dl", timeout_s: float = 30) -> dict:
        """From a frozen frame, run `frames` more frames of `clock` and freeze again. Light edits show after a step."""
        return game.call("step", {"frames": frames, "clock": clock, "timeout_s": timeout_s}, timeout_s + 15)

    @mcp.tool()
    def resume() -> dict:
        """Release a pause and let the game run."""
        return game.call("resume")

    @mcp.tool()
    def frame_info() -> dict:
        """Game state, every clock's latest frame (dl = display lists since boot), held/rt64_paused, and the frozen picture's clocks."""
        return game.call("frame_info")

    @mcp.tool()
    def rdram_read(addr: int, size: int = 4) -> dict:
        """Read game memory as hex (big-endian, as the N64 sees it). addr is a KSEG0 address, e.g. 0x80130B70; size <= 65536."""
        return game.call("rdram_read", {"addr": addr, "size": size})

    @mcp.tool()
    def rdram_write(addr: int, hex: str) -> dict:
        """Write hex bytes to game memory at a KSEG0 address. Prefer writing while paused."""
        return game.call("rdram_write", {"addr": addr, "hex": hex})

    @mcp.tool()
    def press(buttons: list[str] | None = None, stick: list[float] | None = None, frames: int = 0) -> dict:
        """Hold controller-1 buttons (start a b z l r dup ddown dleft dright cup cdown cleft cright; up/down/left/right = full stick) and/or stick [x, y] for `frames` display lists (0 = until release). While paused, follow with step to play exactly those frames."""
        params = {"buttons": buttons or [], "frames": frames}
        if stick is not None:
            params["stick"] = stick
        return game.call("press", params)

    @mcp.tool()
    def release() -> dict:
        """Let go of the input held by press."""
        return game.call("release")

    @mcp.tool()
    def pick(x: float, y: float) -> dict:
        """Frozen frame only: the draw calls covering framebuffer pixel (x, y) in native coordinates (vi_info fb_size, e.g. 320x240), front-most first: the F1 right-click picker."""
        return game.call("pick", {"x": x, "y": y})

    @mcp.tool()
    def highlight(fb: int | None = None, proj: int | None = None, call: int | None = None, color: int | None = None, clear: bool = False) -> dict:
        """Frozen frame only: tint one draw call on screen (RGBA color, default magenta), or clear=true to remove every tint. Follow with screenshot."""
        if clear:
            return game.call("highlight", {"clear": True})
        params = {"fb": fb, "proj": proj, "call": call}
        if color is not None:
            params["color"] = color
        return game.call("highlight", params)

    @mcp.tool()
    def read_symbol(name: str, size: int = 4) -> dict:
        """Read game memory at a symbol from syms/ (e.g. gCurrentLevel) as hex."""
        return game.read_symbol(name, size)

    @mcp.tool()
    def walk_dl(addr: str = "task") -> dict:
        """Dump RDRAM and walk a Factor 5 display list offline (tools/debug/validate/f5_dl_walk.py): the current gfx task, or a hex address such as frame_summary's dl_addr. Returns the walk file and its tail."""
        return game.walk_dl(addr)

    @mcp.tool()
    def hang_dump() -> dict:
        """Full-memory minidump of the running game plus reconstruct-freeze.py (blocked game threads and queues) and host-stacks.py (symbolized host stacks, Debug builds). Several GB on disk; delete the dump when done."""
        return game.hang_dump()

    @mcp.tool()
    def frame_warnings() -> dict:
        """Frozen frame only: RT64's command warnings (message plus the load, tile or draw call it refers to)."""
        return game.call("frame_warnings")

    @mcp.tool()
    def sprite_commands() -> dict:
        """Frozen frame only: the frame's S2D sprite commands (draw call range and replacement hash)."""
        return game.call("sprite_commands")

    @mcp.tool()
    def rsp_state() -> dict:
        """Frozen frame only: RSP fog (mul/offset), lights, LookAt and vertex modifies, as the F1 Debugger lists them."""
        return game.call("rsp_state")

    @mcp.tool()
    def vi_info() -> dict:
        """The last screen VI registers plus framebuffer address/size, visibility and pixel aspect."""
        return game.call("vi_info")

    @mcp.tool()
    def get_config() -> dict:
        """RT64's Configuration tab: user settings (resolution, antialiasing, filtering, aspect, refresh rate, ...) and enhancement settings."""
        return game.call("get_config")

    @mcp.tool()
    def set_config(user: dict | None = None, enhancement: dict | None = None, save: bool = False) -> dict:
        """Change Configuration-tab settings live (keys as get_config returns them). Not written to RT64's config file unless save=true."""
        params = {"save": save}
        if user is not None:
            params["user"] = user
        if enhancement is not None:
            params["enhancement"] = enhancement
        return game.call("set_config", params)

    @mcp.tool()
    def rt_features(set: dict | None = None) -> dict:
        """Read or toggle the RS64 ray-tracing passes: lights, shadows, soft_shadows, fog_shafts, ao, gi, reflections. A feature pinned by its env var can't change (listed in pinned_by_env)."""
        return game.call("rt_features", {"set": set} if set else {})

    @mcp.tool()
    def rt_debug_view(step: int | None = None, name: str | None = None) -> dict:
        """The ray-tracing debug view (as F9 / Shift+F9 in game): step=+1/-1 (or +-n) cycles through them, name picks one (e.g.
        "shadow mask", "light reach", "off"); returns the view in use and every name. Shows after the next step when paused;
        lights and shadows render only with ROGUESQ_RT_LIGHTS=1 / ROGUESQ_RT_SHADOWS=1."""
        params = {}
        if step is not None:
            params["step"] = step
        if name is not None:
            params["name"] = name
        return game.call("rt_debug_view", params)

    @mcp.tool()
    def render_toggles(texture_replacement: bool | None = None, view_rdram: bool | None = None) -> dict:
        """Read or set texture-pack replacement (F4) and ViewRDRAM (F3)."""
        params = {}
        if texture_replacement is not None:
            params["texture_replacement"] = texture_replacement
        if view_rdram is not None:
            params["view_rdram"] = view_rdram
        return game.call("render_toggles", params)

    @mcp.tool()
    def rdram_dump(path: str | None = None) -> dict:
        """Write the game's 8 MB RDRAM (big-endian, the ROGUESQ_DUMP_RDRAM_AT_VI format for rdram_golden_diff.py / state_diff) to path, default under the session dir. Best while paused."""
        target = path or os.path.join(game.session_dir, f"rdram_dl{game.call('frame_info')['clocks']['dl']}.bin")
        return game.call("rdram_dump", {"path": target}, 60)

    @mcp.tool()
    def frame_summary() -> dict:
        """Frozen frame only: draw/triangle counts and every framebuffer pair (color/depth address, width, projections with draw counts)."""
        return game.call("frame_summary")

    @mcp.tool()
    def list_draws(fb: int | None = None, proj: int | None = None, offset: int = 0, limit: int = 500) -> dict:
        """Frozen frame only: one compact row per draw call (tris, TMEM hashes, combiner, other mode, geometry mode, world matrix), optionally for one fb pair / projection."""
        params = {"offset": offset, "limit": limit}
        if fb is not None:
            params["fb"] = fb
        if proj is not None:
            params["proj"] = proj
        return game.call("list_draws", params)

    @mcp.tool()
    def get_draw(fb: int, proj: int, call: int, vertices: bool = False, shaders: bool = False) -> dict:
        """Frozen frame only: everything the F1 Debugger shows for one draw call (loads, tiles, combiner, other modes, RDP colors, world matrices; vertices on request). shaders=true writes its pixel/vertex HLSL under the session dir."""
        params = {"fb": fb, "proj": proj, "call": call, "vertices": vertices}
        if shaders:
            params["shader_dir"] = os.path.join(game.session_dir, "shaders")
        return game.call("get_draw", params)

    @mcp.tool()
    def dump_texture(fb: int, proj: int, call: int, tile: int = 0) -> dict:
        """Frozen frame only: save a draw tile's TMEM (.tmem + .json) and its decoded .png under the session dir; returns the paths and tile parameters."""
        return game.dump_texture(fb, proj, call, tile)

    @mcp.tool()
    def texture_dumping(dir: str | None = None) -> dict:
        """Start RT64's texture dumper into dir (every texture load, .tmem + RDRAM + json), or stop it with no dir."""
        return game.call("texture_dumping", {"dir": dir} if dir else {})

    @mcp.tool()
    def screenshot(path: str | None = None) -> dict:
        """PNG of the game window (client area, captured by the game's PID; default path under the session dir). Returns path, size and distinct colour count."""
        return game.screenshot(path)

    @mcp.tool()
    def view_framebuffer(index: int = -1, depth: bool = False) -> dict:
        """Frozen frame only: show framebuffer pair `index`'s color (or depth) image on screen in place of the frame; -1 restores the normal picture. Follow with screenshot."""
        return game.call("view_framebuffer", {"index": index, "depth": depth})

    @mcp.tool()
    def view_draw(index: int = -1) -> dict:
        """Frozen frame only: draw the frame only up to global draw call `index`; -1 draws everything."""
        return game.call("view_draw", {"index": index})

    @mcp.tool()
    def free_cam(enabled: bool = True, position: list[float] | None = None, target: list[float] | None = None,
                 move: list[float] | None = None, rotate: list[float] | None = None) -> dict:
        """Frozen frame only: RT64's free camera. First enable starts at the game camera; position+target place it, move is [right, up, forward] in world units, rotate is [yaw, pitch] radians. enabled=false restores the game camera."""
        params = {"enabled": enabled}
        for key, value in (("position", position), ("target", target), ("move", move), ("rotate", rotate)):
            if value is not None:
                params[key] = value
        return game.call("free_cam", params)

    @mcp.tool()
    def get_camera() -> dict:
        """The free camera: enabled, position, forward, near/far planes, fov, view matrix."""
        return game.call("get_camera")

    @mcp.tool()
    def lights_list() -> dict:
        """RT light presets (the F1 Lights tab) and the current level. Lights render only with ROGUESQ_RT_LIGHTS=1."""
        return game.call("lights_list")

    @mcp.tool()
    def lights_set(name: str, preset: dict) -> dict:
        """Add or replace a light preset (roguesq_lights.json schema: {"enabled", "level" (-2 any, -1 outside missions), "lights": [{"name", "enabled", "description": {"position", "diffuseColor", "attenuationRadius", ...}}]}). Positions are map space in a level (512 per cell, y down). Shows after the next step."""
        return game.call("lights_set", {"name": name, "preset": preset})

    @mcp.tool()
    def lights_remove(name: str) -> dict:
        """Remove a light preset; shows after the next step."""
        return game.call("lights_remove", {"name": name})

    @mcp.tool()
    def lights_save(path: str) -> dict:
        """Write every light preset to a file in the roguesq_lights.json format."""
        return game.call("lights_save", {"path": path})

    @mcp.tool()
    def frame_times(samples: bool = False) -> dict:
        """RT64's Render-tab frame-time series (ms, last 120): min/avg/max/p99/count for present, renderer_cpu/gpu, matching, workload, dl_api/cpu, update_screen_api/cpu, vi_changed; raw samples on request. Meaningful while running, not paused."""
        return game.call("frame_times", {"samples": samples})

    @mcp.tool()
    def renderdoc_capture(frames: int = 1, timeout_s: float = 20) -> dict:
        """Capture the next presents with RenderDoc and return the .rdc path (open it with the renderdoc MCP)."""
        return game.call("renderdoc_capture", {"frames": frames, "timeout_s": timeout_s}, timeout_s + 15)

    try:
        mcp.run()
    finally:
        game.close()


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        self_test()
    else:
        serve()
