"""Dry check of the lift-walk smoke choreography (tools/mp/smoke_party.py lfw_chains): a ride with two riders can only be
started by the second rider stepping into the trigger once the first stands beside it, and no route ends inside the trigger."""
import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "mp"))
import smoke_party as S  # noqa: E402

R_TRIG = 0.55


def _targets(step):
    """(x, z) points a stick / route / move step walks to or through."""
    body = step.split("=", 1)[1].split("@")[0]
    if "_route=" in step:
        return [(float(w.split(":")[0]), float(w.split(":")[2])) for w in body.split("/") if w]
    if "_stick=" in step:
        p = body.split(":")
        return [(float(p[0]), float(p[1]))]
    return []


def _cases():
    for key, slow in (("yh", False), ("ca", False), ("ybs", True), ("cbs", True)):
        yield key, slow


def test_nobody_walks_into_the_trigger_before_its_turn():
    for key, slow in _cases():
        spec = S.LW[key]
        host, guest = S.lfw_chains(spec, slow)
        plan = S.lfw_plan(spec, slow)
        for chain, role in ((host, "host"), (guest, "guest")):
            for step in chain:
                if "_route=" not in step:
                    continue
                for ride in plan:
                    tx, _ty, tz = spec["trig"][ride[2]]
                    # a route may run past the OTHER end's trigger coordinates only far away (different floor); same end: outside
                    pts = [p for p in _targets(step) if math.dist(p, (tx, tz)) < 3.0]
                    for p in pts:
                        assert math.dist(p, (tx, tz)) > R_TRIG + 0.1, (key, role, step[:80], p)


def test_first_of_two_riders_is_put_beside_the_trigger_and_the_second_waits_for_it():
    for key, slow in _cases():
        spec = S.LW[key]
        host, guest = S.lfw_chains(spec, slow)
        for k, (d, riders, end) in enumerate(S.lfw_plan(spec, slow)):
            if riders != "both":
                continue
            sx, sy, sz = spec["side"][end]
            tx, _ty, tz = spec["trig"][end]
            assert math.dist((sx, sz), (tx, tz)) > R_TRIG + 0.1
            # the host (first rider): a native warp to the side spot, never a stick step into the cylinder
            warp = "host_move=%.2f:%.2f:%.2f:0:native@" % (sx, sy, sz)
            assert any(s.startswith(warp) for s in host), (key, k, "no warp to the side spot")
            # the guest steps into the trigger only with the host beside it
            near = "?near=%.2f:%.2f:%.2f:0.9" % (sx, sy, sz)
            # (the trigger stick itself, or the in-world warp just before it that carries the condition)
            gated = [s for s in guest if near in s and (s.startswith("guest_stick=%.2f:%.2f:" % (tx, tz)) or ":0:native@" in s)]
            assert gated, (key, k, "the guest's trigger step has no near-the-host condition")


def test_every_ride_waits_for_the_previous_one_to_end():
    for key, slow in _cases():
        spec = S.LW[key]
        host, guest = S.lfw_chains(spec, slow)
        plan = S.lfw_plan(spec, slow)
        # the waiting player's walk starts on the car's start animation; the riders after ride 1 are gated on the end animation
        gates = [s for s in host + guest if re.search(r"\?obj=%s:\d+" % spec["car"], s)]
        assert len(gates) >= len(plan) - 1, (key, len(gates))


def test_copy_freeze_detector_separates_a_stopped_copy_from_a_lagging_one():
    real = [(float(t), 0.0, -40.0 + 8.0 * t, 0.0) for t in range(0, 20)]
    lagging = [(float(t), 0.0, -40.0 + 3.0 * t, 0.0) for t in range(0, 20)]
    frozen = [(float(t), 0.0, -40.0, 0.0) for t in range(0, 20)]
    assert S.copy_freeze(real, lagging)[0] == 0.0
    assert S.copy_freeze(real, frozen)[0] > 40.0
    gappy = [c for c in frozen if not 3 <= c[0] <= 16]  # a gap in the log is not a freeze
    assert S.copy_freeze(real, gappy)[0] == 0.0


def _simulate(spec, slow):
    """A coarse run of both chains: a step runs when its ?near / ?obj condition holds; a stick step onto a trigger starts a ride (the car's
    start / arrival / end animations are then seen). Returns the steps left over (a deadlock: two steps waiting on each other)."""
    host, guest = S.lfw_chains(spec, slow)
    chains = {"host": list(host), "guest": list(guest)}
    pos = {"host": None, "guest": None}
    seen = {"host": set(), "guest": set()}  # per player: an animation event is consumed by the step that waited for it
    trig_xz = {(round(t[0], 2), round(t[2], 2)) for t in spec["trig"].values()}  # both ends share the xz: the car's end tells which
    car = [spec["start"]]
    last_trig = {"host": False, "guest": False}
    idx = {"host": 0, "guest": 0}
    progress = True
    while progress:
        progress = False
        for role, other in (("host", "guest"), ("guest", "host")):
            for _once in range(1):  # one step per player per round: both chains advance together, as in the game (a step's delay is ~1 s)
                if idx[role] >= len(chains[role]):
                    break
                step = chains[role][idx[role]]
                head, _, cond = step.partition("?")
                if cond.startswith("near="):
                    x, _y, z, r = (float(c) for c in cond[5:].split(":"))
                    p = pos[other]
                    if p is None or math.dist(p, (x, z)) > r:
                        break
                elif cond.startswith("obj="):
                    if int(cond.split(":")[1]) not in seen[role]:
                        break
                    seen[role].discard(int(cond.split(":")[1]))
                body = head.split("=", 1)[1].split("@")[0]
                if "_move=" in head or "_stick=" in head:
                    f = body.split(":")
                    pos[role] = (float(f[0]), float(f[2 if "_move=" in head else 1]))
                    key = (round(pos[role][0], 2), round(pos[role][1], 2))
                    retry = last_trig[role]
                    last_trig[role] = "_stick=" in head and key in trig_xz
                    if "_stick=" in head and key in trig_xz and not retry:  # the repeated stick step is a retry, not a second ride
                        d = "down" if car[0] == "top" else "up"
                        car[0] = "bottom" if car[0] == "top" else "top"
                        for r_ in seen:
                            seen[r_] = {*spec["start_anim"][d], int(spec["arr"][d]), spec["end_anim"][d]}  # a ride's events replace the last ride's
                elif "_route=" in head:
                    w = body.split("/")[-1].split(":")
                    pos[role] = (float(w[0]), float(w[2]))
                idx[role] += 1
                progress = True
    return {r: chains[r][idx[r]:] for r in chains}


def test_the_two_chains_never_wait_on_each_other():
    for key, slow in _cases():
        left = _simulate(S.LW[key], slow)
        assert not left["host"] and not left["guest"], (key, {r: [s[:70] for s in v][:2] for r, v in left.items()})


def test_the_wait_simulation_sees_a_real_deadlock(monkeypatch):
    """The old choreography (the first rider never placed beside the trigger) must be reported as a deadlock."""
    real = S.lfw_chains

    def broken(spec, slow=False):
        host, guest = real(spec, slow)
        return [s for s in host if ":0:native@0.5" not in s], guest

    monkeypatch.setattr(S, "lfw_chains", broken)
    left = _simulate(S.LW["yh"], False)
    assert left["guest"], "a guest waiting for a host that never stands beside the trigger must be left over"
