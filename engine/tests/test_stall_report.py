"""tools/party/stall_report.py: long-frame classes from synthetic log text (no player data)."""
import importlib.util
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stall_report", ROOT / "tools" / "party" / "stall_report.py")
sr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sr)


def block(ms, cause="loading or other work", user=0.0, kernel=0.0, alloc=0.0, wf=0.0):
    return [
        "       fault handlers: 0 read faults 0.0 ms, write faults %.1f ms" % wf,
        "Stall: %.1f ms frame (%s); GPU thread on CPU 0.0 ms; 0 pipelines built, 0.0 ms; 0 images registered, "
        "0.0 MB image uploads, 0.0 MB buffer uploads" % (ms, cause),
        "       1 draws, 1 dispatches, 1 submissions; GPU thread user %.1f ms, kernel %.1f ms, 0 preempted, 0 waits"
        % (user, kernel),
        "       ms in: resident 0.0, protect 0.0, image create 0.0, image refresh 0.0, staging 0.0, waiting for host copies 0.0",
        "       of which: format query 0.0, allocation %.1f; hashing 0.0, source data 0.0, detiling 0.0" % alloc,
    ]


def pad(n=25):
    return ["Frame stats: 60.0 FPS, worst frame 16.7 ms"] + ["filler line %d" % i for i in range(n)]


def test_classes_and_role():
    lines = ["Party: party networking on (guest, as X, port 1)"] + pad()
    lines += block(500, cause="pipelines built") + pad()
    lines += block(600, alloc=400) + pad()
    lines += block(450, wf=200) + pad()
    lines += block(420, user=300) + pad()
    lines += block(700) + ["Runtime: save data 'SPRJ0005' mounted at /savedata0 (existing)"] + pad()
    lines += block(800) + pad()
    lines += block(2000) + ["Texture cache: memory pressure, 3300 of 3393 MiB budget"] + pad()
    lines += block(200) + ["Guest memory: 12 MiB of loaded blocks moved from in place to VRAM"] + pad()
    lines += block(30) + pad()  # under 150 ms: not counted
    r = sr.scan(lines)
    assert r["role"] == "guest"
    s = r["stalls"]
    assert s[("400-1500", "pipelines")] == 1
    assert s[("400-1500", "image-alloc")] == 1
    assert s[("400-1500", "fault-handlers")] == 1
    assert s[("400-1500", "gpu-thread-busy")] == 1
    assert s[("400-1500", "save-near")] == 1
    assert s[("400-1500", "game-thread")] == 1
    assert s[("1500+", "vram-pressure")] == 1
    assert s[("150-400", "mem-move-near")] == 1
    assert sum(s.values()) == 8
    assert r["hours"] > 0


def test_probe_lines_and_report():
    lines = pad(3) + [
        "Stall probe: 812 ms frame: main thread 790 ms, frame end 22 ms (render thread done 1.0 ms into the frame end); "
        "main samples 700 (party tick 10, in the frame end 20): +0x1234 60%",
        "Stall probe: 300 ms frame: main thread 20 ms, frame end 280 ms (render thread done: not seen)",
        "Stall probe: 400 ms frame: main thread 380 ms, frame end 20 ms (x); main samples 300 (party tick 200, in the frame end 0): +0x1 1%",
    ]
    r = sr.scan(lines)
    assert r["role"] == "solo"
    assert len(r["probe"]) == 3
    assert r["probe"][0][:3] == (812.0, 790.0, 22.0)
    assert r["probe"][1][3] == 0.0  # no samples part
    text = sr.report("x.log", r, True)
    assert "3 long frames; main work the larger part in 2, render wait in 1" in text
    assert "party tick half the main samples or more in 1" in text
    assert "worst 812 ms" in text


def test_no_probe_lines():
    r = sr.scan(pad(2))
    assert "BB_STALL_PROBE off?" in sr.report("y.log", r, True)


def test_harness_timestamps_stripped(tmp_path, capsys):
    lines = ["[+  12.345] " + x for x in pad(2) + block(600) + pad(2)]
    f = tmp_path / "run.log"
    f.write_text(chr(10).join(lines), encoding="utf-8")
    sr.main([str(f)])
    out = capsys.readouterr().out
    assert "400-1500      1" in out
