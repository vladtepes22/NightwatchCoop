"""scripts/net_doctor.py on mangled log lines: it parses logs that players send, so it must never raise
and must stay fast on any input (deterministic seed; NET_DOCTOR_FUZZ_ITERS raises the count)."""
import importlib.util
import os
import random
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("net_doctor", ROOT / "scripts" / "net_doctor.py")
nd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nd)
seed_mod = importlib.util.spec_from_file_location("test_net_doctor", ROOT / "tests" / "test_net_doctor.py")
seeds = importlib.util.module_from_spec(seed_mod)
seed_mod.loader.exec_module(seeds)

PIECES = [b"", b"\x00", b"\xff\xfe", b"\xc3\x28", b"\r", b"\n", b"[coop] Net: ", b"[coop] Party: ", b"Party B2: ", b":", b"::",
          b"999.999.999.999", b"1.2.3.4:99999", b"1.2.3.4:0", b"-1", b"0x41", b"1e999", b"%s%n", b"{}", b"\\u0000",
          b"(slot ", b"host ", b"guest ", b", port ", b"up to ", b"9" * 40, b"A" * 5000, b" " * 3000, b"\t",
          b"eboot ", b"patches ", b"mods ", b"(", b")", b"[", b"]", b"26.121.14.68", b"fe80::1%eth0", b"[::1]:9307",
          b"NAT: ", b"symmetric", b"cone", b"stun ", b"joined (slot ", b"from ", b"\xe2\x80\xae", b"\xf0\x9f\x98\x80"]


def mangle(rng, text):
    lines = text.encode("utf-8").split(b"\n")
    for _ in range(rng.randint(1, 8)):
        if not lines:
            lines = [b""]
        k = rng.randrange(7)
        i = rng.randrange(len(lines))
        ln = lines[i]
        if k == 0:
            lines[i] = ln[:rng.randint(0, len(ln))]
        elif k == 1:
            at = rng.randint(0, len(ln))
            lines[i] = ln[:at] + rng.choice(PIECES) + ln[at:]
        elif k == 2 and ln:
            b = bytearray(ln)
            b[rng.randrange(len(b))] = rng.randrange(256)
            lines[i] = bytes(b)
        elif k == 3:
            del lines[i]
        elif k == 4:
            lines.insert(i, ln * rng.randint(2, 50))
        elif k == 5:
            j = rng.randrange(len(lines))
            lines[i], lines[j] = lines[j], lines[i]
        else:
            lines.insert(i, bytes(rng.randrange(256) for _ in range(rng.randint(0, 200))))
    return b"\n".join(lines)


def run_once(tmp, rng, flags):
    files = []
    for n, src in enumerate((seeds.HOST, seeds.GUEST, seeds.GUEST)):
        p = tmp / f"nightwatch-2026102{n}_20{rng.randint(10, 59)}{rng.randint(10, 59)}.log"
        p.write_bytes(mangle(rng, src))
        files.append(str(p))
    if rng.random() < 0.1:
        files.append(str(tmp / "missing.log"))
    return nd.main([*flags, *files])


def test_mangled_logs_never_raise(tmp_path, capsys):
    iters = int(os.environ.get("NET_DOCTOR_FUZZ_ITERS", "300"))
    rng = random.Random(0x77)
    flag_sets = [[], ["--evidence"], ["--redactnames", "--redactips"], ["--evidence", "--redactnames", "--redactips"]]
    slowest = 0.0
    for it in range(iters):
        t0 = time.time()
        rc = run_once(tmp_path, rng, flag_sets[it % len(flag_sets)])
        slowest = max(slowest, time.time() - t0)
        assert rc in (0, 1)
        capsys.readouterr()
    assert slowest < 5.0, f"slowest run {slowest:.1f} s"


def test_degenerate_logs(tmp_path, capsys):
    cases = [b"", b"\n" * 10000, b"\x00" * 100000, b"[coop] Net: party networking on (host, as , port 0)\n",
             b"[coop] Net: party networking on (guest, as " + b"A" * 100000 + b", port 99999999999999999999)\n",
             b"[coop] Party: joining :::::\n[coop] Party: link: X joined (slot 99999999999, from [::1]:0)\n",
             (b"[coop] Party: NAT: cone\n" * 20000)]
    for n, data in enumerate(cases):
        p = tmp_path / f"d{n}.log"
        p.write_bytes(data)
        assert nd.main([str(p)]) in (0, 1)
        capsys.readouterr()
