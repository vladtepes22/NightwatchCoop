"""Name filter: shared vectors, the data-file/embed sync, mask format, and a differential run against the C++ twin.

The C++ binary (gpu target name-filter-test) is built by `ninja -C out/gpu name-filter-test`; this test uses it when it
exists (env BB_NAME_FILTER_TEST_EXE, or out/gpu/name-filter-test(.exe)) and skips the comparison otherwise.
"""
import os
import random
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "name_filter"))
import namefilter as nf  # noqa: E402

VECTORS = ROOT / "tests" / "name_filter_vectors.txt"


@pytest.fixture(scope="module")
def flt():
    return nf.NameFilter(nf.load_texts(str(ROOT / "data" / "name_filter")))


def vectors():
    pos, neg = [], []
    for ln in VECTORS.read_text(encoding="utf-8").split("\n"):
        if len(ln) > 2 and ln[0] in "+-" and ln[1] == "\t":
            (pos if ln[0] == "+" else neg).append(ln[2:])
    return pos, neg


def test_vector_counts():
    pos, neg = vectors()
    assert len(pos) >= 200 and len(neg) >= 500


def test_positive_vectors(flt):
    bad = [ascii(n) for n in vectors()[0] if not flt.offensive(n)]
    assert not bad, bad[:20]


def test_negative_vectors(flt):
    bad = [ascii(n) for n in vectors()[1] if flt.offensive(n)]
    assert not bad, bad[:20]


@pytest.mark.parametrize("name", ["Black ppl hunter", "B1ack p30ple hunt3r", "blackpplhunter"])
def test_owner_cases_masked(flt, name):
    assert flt.offensive(name)


@pytest.mark.parametrize("name", ["Black Hunter", "Beast Hunter", "Hunter of Hunters", "Gehrman", "Vileblood", "Scunthorpe",
                                  "Classic Assassin", "Hunter", "Niger"])
def test_owner_cases_pass(flt, name):
    assert not flt.offensive(name)


def test_mask_is_stable_and_distinct():
    a, b = nf.mask_text("alpha"), nf.mask_text("beta")
    assert re.fullmatch(r"Hunter [0-9A-F]{3}", a) and a == nf.mask_text("alpha") and a != b
    assert nf.mask_text("alpha") == nf.mask_text(b"alpha")


def test_masked_name_default_instance():
    assert nf.masked_name("Gehrman") == "Gehrman"
    assert nf.masked_name("Black ppl hunter").startswith("Hunter ")
    assert nf.masked_name("") == ""


def test_long_name_is_bounded(flt):
    import time
    t0 = time.time()
    for n in ("a" * 100000, "ni" * 50000, "́" * 100000, ("black ppl " * 20000)):
        flt.reason(n)
    assert time.time() - t0 < 2.0


def test_embedded_copy_in_sync():
    r = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "name_filter" / "gen_embed.py"), "--check"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout


def test_data_files_wellformed():
    texts = nf.load_texts(str(ROOT / "data" / "name_filter"))
    for f in nf.FILES:
        assert f in texts and texts[f].strip()
    for ln in texts["strict.txt"].splitlines():
        if ln and not ln.startswith("#"):
            p = ln.split()
            assert len(p) == 2 and set(p[1]) <= set("stx") and p[1][0] in "st", ln


def _exe():
    for c in (os.environ.get("BB_NAME_FILTER_TEST_EXE", ""), ROOT / "out" / "gpu" / "name-filter-test.exe",
              ROOT / "out" / "gpu" / "name-filter-test"):
        if c and Path(c).exists():
            return str(c)
    return None


def _random_names(n, seed=7):
    rnd = random.Random(seed)
    words = [w.split()[0] for w in (ROOT / "data" / "name_filter" / "strict.txt").read_text(encoding="utf-8").splitlines()
             if w and not w.startswith("#")]
    words += [w.strip() for f in ("identity.txt", "violence.txt", "people.txt", "link.txt")
              for w in (ROOT / "data" / "name_filter" / f).read_text(encoding="utf-8").split("\n") if w and w[0] != "#"]
    words = [w.split()[0] for w in words]
    cm = [int(ln.split("\t")[0].split("-")[0], 16) for ln in (ROOT / "data" / "name_filter" / "charmap.tsv").read_text(encoding="utf-8").split("\n")
          if ln and ln[0] != "#"]
    seps = [" ", ".", "_", "-", "​", "!", "@", "$", "1", "3", "0", "x", "*", "‮", "́"]
    out = []
    for _ in range(n):
        parts = []
        for _ in range(rnd.randint(1, 4)):
            w = rnd.choice(words)
            k = rnd.random()
            if k < 0.2:
                w = "".join(rnd.choice([c, c.upper(), c * 3]) for c in w)
            elif k < 0.4:
                w = "".join(chr(rnd.choice(cm)) if rnd.random() < 0.25 else c for c in w)
            elif k < 0.55:
                w = rnd.choice(seps).join(w)
            parts.append(w)
        s = rnd.choice(seps + [""]).join(parts)
        if rnd.random() < 0.3:
            s = s[:rnd.randint(0, len(s))] + chr(rnd.choice(cm)) + s[rnd.randint(0, len(s)):]
        out.append(s.replace("\n", " ").replace("\r", " ")[:100])
    return out


def test_python_and_cpp_agree(flt):
    exe = _exe()
    if not exe:
        pytest.skip("name-filter-test is not built")
    names = _random_names(6000) + vectors()[0] + vectors()[1]
    r = subprocess.run([exe, "--stdin"], input="\n".join(names) + "\n", capture_output=True, encoding="utf-8",
                       check=True, env=dict(os.environ, PYTHONUTF8="1"))
    got = r.stdout.split("\n")[:len(names)]
    diff = [(ascii(n), flt.reason(n) or "-", g) for n, g in zip(names, got) if (flt.reason(n) or "-") != g]
    assert not diff, diff[:10]


def _launcher():
    import importlib.util
    spec = importlib.util.spec_from_file_location("bb_launcher_namefilter", ROOT / "launcher.py")
    mod = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(mod)
    except Exception as ex:  # no tkinter here
        pytest.skip(f"launcher.py not importable here: {ex}")
    return mod


def test_launcher_masks_rows_unless_turned_off():
    L = _launcher()
    bad = "Black ppl hunter"
    entry = {"ip": "10.8.0.5", "port": 9307, "name": bad, "players": 1, "max": 3, "password": True, "own": False, "build": "0.7.8"}
    rows = L.lobby_rows([entry], "0.7.8")
    assert rows[0]["name"].startswith("Hunter ") and bad not in rows[0]["name"]
    assert L.lobby_rows([entry], "0.7.8", hide_bad_names=False)[0]["name"] == bad
    assert L.shown_party_name("Gehrman") == "Gehrman"


def test_iosefka_report_scrubs_member_names():
    sys.path.insert(0, str(ROOT / "tools" / "discord"))
    import netdoctor_bot as bot

    class Log:
        name = "Black ppl hunter"
        guests = {"Gehrman"}
        pings = {}

    class Nd:
        @staticmethod
        def parse(_path):
            return Log

    out = bot.scrub_offensive_names(Nd, ["x.log"], "host Black ppl hunter met Gehrman")
    assert "Black" not in out and "Gehrman" in out and "Hunter " in out
