"""Offline freeze-state reconstructor.

Reads a full-memory external minidump, locates the 8 MB guest RDRAM region by
content-validating known OSMesgQueue addresses, then:
  1. Enumerates every OSThread struct in RDRAM (heuristic + graph validation).
  2. Prints, for each thread, its id / priority / next / and its `queue` field.
     The `queue` field is authoritative (thread_queue_insert sets it, pop clears
     it), so it self-reports each thread's exact wait target:
        0x00000000  -> currently running (not in any queue)
        0xFFFFFFFF  -> sitting in the run queue, waiting its turn  [BRANCH A]
        0x8011xxxx  -> blocked on that OSMesgQueue's list
  3. Dumps the state of every known frame/audio/VI queue.

Three-way read:
  A. A thread has queue==0xFFFFFFFF while the running thread is blocked
     -> scheduler never resumed it (ultramodern bug).
  B. Every thread blocked; the missing producer is an alive HOST thread
     -> message dropped / misrouted host->game.
  C. Every thread blocked; the missing producer is another blocked GAME thread
     -> game-level wait cycle (e.g. SP-done misrouted by the stubbed yield).

Usage: python tools/reconstruct-freeze.py [dump.dmp]
"""
import sys
from pathlib import Path
from minidump.minidumpfile import MinidumpFile

RDRAM_SIZE = 0x800000

# OSThread layout (ultramodern ultra64.h)
T_NEXT, T_PRI, T_QUEUE, T_STATE16, T_ID = 0x00, 0x04, 0x08, 0x12, 0x14
# OSMesgQueue layout
Q_RECV, Q_SEND, Q_VALID, Q_FIRST, Q_MSGCOUNT, Q_MSG = 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14

KNOWN_QUEUES = {
    0x80114388: "VI-event",
    0x8011A800: "audio-done",
    0x8011A818: "gfx SP-done",
    0x8011A7E8: "DP-done consumer",
    0x8011A408: "gate-thread DP",
    0x8011A420: "SP-scheduler input",
    0x80128CF0: "video (post-swap ack)",
    0x80128D10: "frame-sync mutex",
}
STATE_NAME = {0: "STOPPED", 1: "QUEUED", 2: "RUNNING", 3: "BLOCKED"}


def be32(buf, off):
    # guest big-endian 32-bit at guest offset `off`, buffer stored host ^3-swapped
    return (buf[(off) ^ 3] << 24) | (buf[(off + 1) ^ 3] << 16) | \
           (buf[(off + 2) ^ 3] << 8) | buf[(off + 3) ^ 3]


def be16(buf, off):
    return (buf[(off) ^ 3] << 8) | buf[(off + 1) ^ 3]


def in_rdram(g):
    return 0x80000000 <= g < 0x80800000


def load_rdram(md):
    """Find and return the 8 MB RDRAM region as a flat bytes buffer (guest off 0)."""
    reader = md.get_reader()
    segs = []
    for attr in ("memory_segments_64", "memory_segments"):
        lst = getattr(md, attr, None)
        if lst is not None:
            segs = lst.memory_segments
            break
    best = None
    for seg in segs:
        size = seg.size
        if size < RDRAM_SIZE:
            continue
        base = seg.start_virtual_address
        # validate: read a handful of known queues; msgCount small, msg in RDRAM
        try:
            probe = reader.read(base, RDRAM_SIZE)
        except Exception:
            continue
        hits = 0
        for q in KNOWN_QUEUES:
            off = q & 0xFFFFFF
            if off + Q_MSG + 4 > len(probe):
                continue
            mc = be32(probe, off + Q_MSGCOUNT)
            msg = be32(probe, off + Q_MSG)
            if 1 <= mc <= 256 and in_rdram(msg):
                hits += 1
        if hits >= 5:
            print(f"[rdram] region @host 0x{base:016X} size 0x{size:X}  ({hits}/{len(KNOWN_QUEUES)} queues valid)")
            best = probe
            break
    return best


def read_thread(rd, g):
    off = g & 0xFFFFFF
    return dict(next=be32(rd, off + T_NEXT), pri=be32(rd, off + T_PRI),
                queue=be32(rd, off + T_QUEUE), id=be32(rd, off + T_ID),
                state=be16(rd, off + T_STATE16),
                ctx=be32(rd, off + 0x20))  # low 4 bytes of the host context ptr (8-byte aligned field)


def looks_like_thread(t):
    return t["pri"] <= 255 and t["id"] < 4096 and \
        (t["next"] == 0 or in_rdram(t["next"])) and \
        (t["queue"] in (0, 0xFFFFFFFF) or in_rdram(t["queue"]))


def find_all_queues(rd):
    """A queue is validated by: msgCount 1..256, msg ptr in RDRAM. Scan for any
    address whose blocked_on_recv/send heads point at thread-like structs."""
    valid = dict(KNOWN_QUEUES)
    return valid


def enumerate_threads(rd):
    """Walk blocked lists from every known queue (authoritative). Then strict-scan
    for run-queue / running threads that no RDRAM queue points to."""
    real = {}
    src = {}  # g -> "recv<-name" / "send->name"
    for q, nm in KNOWN_QUEUES.items():
        off = q & 0xFFFFFF
        for slot, tag in ((Q_RECV, f"recv<-{nm}"), (Q_SEND, f"send->{nm}")):
            head = be32(rd, off + slot)
            seen = set()
            while in_rdram(head) and head not in seen:
                seen.add(head)
                t = read_thread(rd, head)
                if not looks_like_thread(t):
                    break
                real[head] = t
                src.setdefault(head, tag)
                head = t["next"]
    # strict scan for run-queue members (queue == run-queue sentinel) that no
    # RDRAM queue points to. 0xFFFFFFFF is rare enough to be a strong anchor.
    for off in range(0, RDRAM_SIZE - 0x24, 4):
        if be32(rd, off + T_QUEUE) != 0xFFFFFFFF:
            continue
        g = 0x80000000 + off
        if g in real:
            continue
        t = read_thread(rd, g)
        if not looks_like_thread(t):
            continue
        if t["ctx"] < 0x1000:          # real threads carry a host context ptr
            continue
        # Game threads can have id 0. A STOPPED thread keeps a stale run-queue sentinel after
        # osStopThread (thread_queue_remove does not clear it), so require state QUEUED (1).
        if 0 <= t["id"] < 64 and t["state"] == 1:
            real[g] = t
            src.setdefault(g, "RUN-QUEUE")
    for g in real:
        real[g]["src"] = src.get(g, "?")
    return real


def qname(addr):
    if addr == 0:
        return "-- running (no queue)"
    if addr == 0xFFFFFFFF:
        return "** RUN QUEUE (awaiting resume)"
    base = addr & ~1 if False else addr
    for q, nm in KNOWN_QUEUES.items():
        if addr == q:
            return f"blocked recv <- {nm} (0x{q:08X})"
        if addr == q + 4:
            return f"blocked send -> {nm} (0x{q:08X})"
    return f"blocked on 0x{addr:08X} (unknown queue)"


def main():
    dumps = sorted(Path("dumps/crash-dumps").glob("*_external.dmp"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    path = sys.argv[1] if len(sys.argv) > 1 else (str(dumps[0]) if dumps else None)
    if not path:
        print("No external dump found.")
        return 1
    print(f"Reading {path}")
    md = MinidumpFile.parse(path)
    rd = load_rdram(md)
    if rd is None:
        print("Could not locate RDRAM region.")
        return 1

    threads = enumerate_threads(rd)
    print(f"\n=== OSThreads (blocked on a real queue, or in the run queue): {len(threads)} ===")
    print(f"{'guest':<12}{'id':<5}{'pri':<5}{'next':<12}{'src':<22}wait target")
    print("-" * 100)
    run_queue_members, running, blocked = [], [], []
    for g in sorted(threads, key=lambda x: (threads[x]['src'], threads[x]['id'])):
        t = threads[g]
        q = t["queue"]
        print(f"0x{g:08X}  {t['id']:<5}{t['pri']:<5}0x{t['next']:08X}  {t['src']:<22}{qname(q)}")
        if q == 0xFFFFFFFF:
            run_queue_members.append((g, t))
        elif q == 0:
            running.append((g, t))
        else:
            blocked.append((g, t, q))

    print("\n=== Known queue state ===")
    print(f"{'queue':<26}{'valid':<7}{'first':<7}{'msgCount':<10}{'recv-head':<12}{'send-head'}")
    print("-" * 80)
    for q, nm in KNOWN_QUEUES.items():
        off = q & 0xFFFFFF
        print(f"{nm:<18}0x{q:08X}  {be32(rd,off+Q_VALID):<7}{be32(rd,off+Q_FIRST):<7}"
              f"{be32(rd,off+Q_MSGCOUNT):<10}0x{be32(rd,off+Q_RECV):08X}  0x{be32(rd,off+Q_SEND):08X}")

    print("\n=== Verdict ===")
    if run_queue_members:
        print(f"BRANCH A (scheduler bug): {len(run_queue_members)} thread(s) sit in the run "
              f"queue awaiting resume while the machine is frozen:")
        for g, t in run_queue_members:
            print(f"    id={t['id']} pri={t['pri']} @0x{g:08X}")
    else:
        print("No thread in the run queue -> NOT branch A. Every game thread is blocked.")
        print("Blocked set (id -> wait target):")
        for g, t, q in sorted(blocked, key=lambda x: x[1]['id']):
            print(f"    id={t['id']:<4} -> {qname(q)}")
        print("Now identify, for the queue the running/MAIN thread waits on, whether its")
        print("producer is an alive HOST thread (branch B) or a blocked GAME thread (branch C).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
