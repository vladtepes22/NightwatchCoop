#!/usr/bin/env python3
"""Machine-wide limits shared by every agent: running game clients and parallel builds.

Each pool is one JSON file next to E:\\games\\bbport\\agents\\game_queue.json (BB_GAME_QUEUE_FILE):
the "game" pool is game_queue.json, others are <pool>_queue.json (e.g. build_queue.json). Each is
edited under an exclusive lock file next to it:

  {"max_clients": 3, "max_solo": 4,   # jobs up to max_solo slots run only alone
   "holders": [{"id", "pid", "count", "root", "since"}],   # runs that hold slots
   "queue":   [{"id", "pid", "count", "root", "since"}]}   # waiting, first come first served

A job needing `count` slots waits until it is first in the queue and the holders' slots plus
`count` fit in max_clients. Entries whose process is gone are dropped, so a killed job never
blocks the queue. max_clients is read from the file each time (edit it to change the limit;
defaults: game 2 (BB_GAME_QUEUE_MAX), build 2 (BB_BUILD_QUEUE_MAX)).

  python tools/mp/game_queue.py status [pool]      # who runs, who waits (pool: game, build)
  python tools/mp/game_queue.py clear-dead [pool]  # drop entries of dead processes now
"""
import contextlib, json, os, sys, time
from pathlib import Path

QUEUE_FILE = Path(os.environ.get("BB_GAME_QUEUE_FILE", r"E:\games\bbport\agents\game_queue.json"))
POOL_DEFAULTS = {"game": int(os.environ.get("BB_GAME_QUEUE_MAX", "3")),
                 "build": int(os.environ.get("BB_BUILD_QUEUE_MAX", "3"))}


def pool_file(pool: str) -> Path:
    return QUEUE_FILE if pool == "game" else QUEUE_FILE.with_name(f"{pool}_queue.json")


def _alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k.GetExitCodeProcess(h, ctypes.byref(code))
        k.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


@contextlib.contextmanager
def _locked(pool: str = "game"):
    qf = pool_file(pool)
    qf.parent.mkdir(parents=True, exist_ok=True)
    lock = qf.with_suffix(".lock")
    t0 = time.time()
    while True:
        try:
            fd = os.open(str(lock), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            break
        except (FileExistsError, PermissionError):  # Windows: the lock is being deleted right now
            # A lock older than 30 s belongs to a process that died mid-edit.
            try:
                if time.time() - lock.stat().st_mtime > 30:
                    lock.unlink()
                    continue
            except OSError:
                pass
            if time.time() - t0 > 120:
                raise SystemExit(f"{pool} queue: cannot lock {lock}")
            time.sleep(0.1)
    try:
        try:
            state = json.loads(qf.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            state = {}
        state.setdefault("max_clients", POOL_DEFAULTS.get(pool, 2))
        # A job bigger than max_clients (e.g. a 4-player test) may still run, but only alone:
        # max_solo is the largest such job (game pool: 4; 4 games at once starve the CPU otherwise).
        state.setdefault("max_solo", 4 if pool == "game" else state["max_clients"])
        state.setdefault("holders", [])
        state.setdefault("queue", [])
        yield state
        tmp = qf.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
        os.replace(tmp, qf)
    finally:
        try:
            lock.unlink()
        except OSError:
            pass


def _prune(state):
    state["holders"] = [e for e in state["holders"] if _alive(e["pid"])]
    state["queue"] = [e for e in state["queue"] if _alive(e["pid"])]


@contextlib.contextmanager
def game_slots(count: int, root: str, pool: str = "game", label: str = ""):
    """Hold `count` slots of `pool` for the duration of the block (waits its turn); yields the
    slot numbers held."""
    me = {"id": f"{os.getpid()}-{time.time():.0f}", "pid": os.getpid(), "count": count, "root": str(root), "label": label,
          "t": time.time(),
          "since": time.strftime("%H:%M:%S")}
    with _locked(pool) as st:
        _prune(st)
        if count > max(st["max_clients"], st.get("max_solo", 0)):
            raise SystemExit(f"{pool} queue: this job needs {count} slots but the machine-wide limit is "
                             f"{st['max_clients']} ({pool_file(pool)}: max_clients)")
        st["queue"].append(me)
    last_note = 0.0
    try:
        while True:
            with _locked(pool) as st:
                _prune(st)
                used = sum(e["count"] for e in st["holders"])
                # Priority roots (one path fragment per line in priority.txt next to the queue file)
                # go ahead of everyone else, first come first served among themselves.
                try:
                    prio = [l.strip().lower() for l in (QUEUE_FILE.parent / "priority.txt").read_text().splitlines()
                            if l.strip()]
                except OSError:
                    prio = []
                is_prio = lambda e: any(f in e["root"].replace("/", "\\").lower() for f in prio)
                order = [e for e in st["queue"] if is_prio(e)] + [e for e in st["queue"] if not is_prio(e)]
                first = order and order[0]["id"] == me["id"]
                # Backfill: when the head of the queue doesn't fit in the free slots, the earliest
                # waiter that does fit may go (runs are short, so the head isn't starved for long).
                def fits(e):  # within max_clients, or a big job alone (up to max_solo)
                    return used + e["count"] <= st["max_clients"] or (used == 0 and e["count"] <= st.get("max_solo", 0))
                if not first and order:
                    free = st["max_clients"] - used
                    head_fits = fits(order[0])
                    if not head_fits:
                        # Never backfill past a waiting big job: it needs the machine to itself.
                        # ...nor past a head that has waited over 60 s (3-4 client tests starved
                        # behind a stream of 2-client backfills).
                        waited = time.time() - order[0].get("t", time.time())
                        big = order[0]["count"] > st["max_clients"] or waited > 60
                        fitting = None if big else next((e for e in order if e["count"] <= free), None)
                        first = fitting is not None and fitting["id"] == me["id"]
                # Gate (gate.txt next to the queue file, e.g. "only the join speed-up may test until
                # its fix is merged"): while it exists, only priority roots get game slots.
                gate = (QUEUE_FILE.parent / "gate.txt").exists()
                if gate and pool == "game" and not is_prio(me):
                    first = False
                if first and fits(me):
                    st["queue"] = [e for e in st["queue"] if e["id"] != me["id"]]
                    me["since"] = time.strftime("%H:%M:%S")
                    # Slot numbers (0..max-1) not held by anyone: the game pool uses them as screen
                    # positions (tools/mp/instances.py tiles windows by slot).
                    taken = {n for e in st["holders"] for n in e.get("slots", [])}
                    me["slots"] = [n for n in range(st["max_clients"] + count) if n not in taken][:count]
                    st["holders"].append(me)
                    break
                pos = next((i for i, e in enumerate(order) if e["id"] == me["id"]), -1)
                busy = ", ".join(f"{e['root']} ({e['count']})" for e in st["holders"])
                limit = st["max_clients"]
            if time.time() - last_note > 30:
                print(f"{pool} queue: waiting (position {pos + 1}, {used}/{limit} in use: {busy})", flush=True)
                last_note = time.time()
            time.sleep(2)
        print(f"{pool} queue: got {count} slot(s) {me['slots']}", flush=True)
        yield me["slots"]
    finally:
        with _locked(pool) as st:
            st["holders"] = [e for e in st["holders"] if e["id"] != me["id"]]
            st["queue"] = [e for e in st["queue"] if e["id"] != me["id"]]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    pool = sys.argv[2] if len(sys.argv) > 2 else "game"
    with _locked(pool) as st:
        if cmd == "clear-dead":
            _prune(st)
        print(f"{pool}: max {st['max_clients']}  ({pool_file(pool)})")
        for e in st["holders"]:
            print(f"  running  {e['count']}  {e['root']}  since {e['since']}  pid {e['pid']}  {e.get('label', '')}"
                  f"{'' if _alive(e['pid']) else '  (dead)'}")
        for i, e in enumerate(st["queue"]):
            print(f"  wait #{i + 1} {e['count']}  {e['root']}  since {e['since']}  pid {e['pid']}  {e.get('label', '')}"
                  f"{'' if _alive(e['pid']) else '  (dead)'}")


if __name__ == "__main__":
    main()
