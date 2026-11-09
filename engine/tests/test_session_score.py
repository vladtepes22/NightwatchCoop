"""tools/party/session_score.py on synthetic party logs (host + guests written here; no real log is copied).

Covers a proven row, a failed row, an unexercised row, IP / name masking, the net_doctor rows, save hashes,
zipped logs and that the marker table matches docs/party/ERSC_PARITY.md and tools/mp/smoke_party.py."""
import importlib.util
import json
import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("session_score", ROOT / "tools" / "party" / "session_score.py")
ss = importlib.util.module_from_spec(spec)
sys.modules["session_score"] = ss
spec.loader.exec_module(ss)

PUBLIC = "80.12.34.56"      # an arbitrary public address (net_doctor treats documentation ranges as non-public)
VPNIP = "26.10.20.30"
HOST_LINES = [
    "Nightwatch Co-op v0.2.6 beta 4, engine x, 2026-11-01 12.26.06",
    "[coop] Net: party networking on (host, as HostPlayer, port 9307)",
    "[coop] Party: host HostPlayer, port 9307, up to 4 players; version check: eboot 071df19c8880..., patches d8e32e8ab600..., mods 000000000000... (6 gameplay patches, no gameplay mods)",
    f"[coop] Party: link: GuestOne joined (slot 1, from {PUBLIC}:40000)",
    "[coop] Party: cooperators 0 -> 1 (t 12 s)",
    "GPU noise line that must not matter",
    "[coop] Party progress: host flag 11100100 = 1 (shortcut_door) seq 4",
    "[coop] Party BN: host: slot 1 arrived".replace("BN", "B2"),
    "[coop] Party: link: lost GuestOne (slot 1): connection reset; keeping the slot 130 s",
    "[coop] Party: link: GuestOne rejoined (slot 1, from %s:40000)" % PUBLIC,
]
GUEST_LINES = [
    "Nightwatch Co-op v0.2.6 beta 4, engine x, 2026-11-01 12.26.06",
    "[coop] Net: party networking on (guest, as GuestOne, port 9307)",
    f"[coop] Party: joining {PUBLIC}:9307 as GuestOne (Internet code)",
    "[coop] Party: link: connected: slot 1",
    "[coop] Party progress: guest applied 12 flags in load mode 0 (NPC state cleared 0, already 3, skipped 1)",
]
LIFT_FAIL = "[coop] Party: world: lift ride: riding an object at (1.0, 2.0, 3.0) with no table car playing within 4 m: nearest car 2421250 not playing"


def write(d, name, lines):
    p = Path(d) / name
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p


def score(d):
    out = ss.run(d)
    assert out is not None
    md, js, res = out
    return md, json.loads(js), res


@pytest.fixture
def session(tmp_path):
    write(tmp_path, "host.log", HOST_LINES)
    write(tmp_path, "guest1.log", GUEST_LINES)
    return tmp_path


def test_proven_row_cites_file_and_line(session):
    _md, js, res = score(session)
    assert res["A1"]["status"] == ss.PROVEN
    assert res["D2"]["status"] == ss.PROVEN
    ev = {e["marker"]: e for e in res["D2"]["ev"]}
    assert ev["host flags sent to the party"]["file"] == "host.log" and ev["host flags sent to the party"]["line"] == 7
    assert ev["a guest applied them"]["file"] == "guest1.log" and ev["a guest applied them"]["line"] == 5
    rows = {r["id"]: r for r in js["rows"]}
    assert rows["D2"]["status"] == "PROVEN" and rows["D2"]["evidence"][0]["line"] > 0
    assert len(js["rows"]) == 49 and js["summary"]["rows"] == 49


def test_marker_needs_the_right_kind_of_pc(tmp_path):
    # the host's flag line is only a proof on a host log: the same text in the guest log proves nothing
    write(tmp_path, "host.log", HOST_LINES[:2])
    write(tmp_path, "guest1.log", GUEST_LINES + [HOST_LINES[6]])
    _md, _js, res = score(tmp_path)
    assert res["D2"]["status"] == ss.NOT


def test_failed_row_wins_but_keeps_the_proof(session):
    write(session, "guest1.log", GUEST_LINES + [LIFT_FAIL])
    host = HOST_LINES + ["[coop] Party: world: host: slot 1 changed the host's world (guest input)"]
    write(session, "host.log", host)
    md, _js, res = score(session)
    r = res["D3"]
    assert r["status"] == ss.FAILED and r["ok"]
    assert r["fails"][0]["file"] == "guest1.log" and r["fails"][0]["line"] == len(GUEST_LINES) + 1
    assert "| D3 |" in md and "FAILED" in md and "proof markers were also seen" in md


def test_unexercised_row_lists_missing_markers(session):
    md, _js, res = score(session)
    r = res["G1"]
    assert r["status"] == ss.NOT and r["ev"] == [] and r["missing"]
    assert res["G3"]["status"] == ss.NOT
    assert any("| G1 |" in line and "NOT EXERCISED" in line and "missing:" in line for line in md.splitlines())


def test_ips_and_names_never_printed(session):
    write(session, "guest1.log", GUEST_LINES + [f"[coop] Party: link: retry to {PUBLIC}:9307 and {VPNIP}:9307 by GuestOne  BBP1-AB12-CD34"])
    md, js, _res = score(session)
    for text in (md, json.dumps(js)):
        assert PUBLIC not in text and VPNIP not in text
        assert "GuestOne" not in text and "HostPlayer" not in text
        assert "BBP1-AB12-CD34" not in text
    assert "<public IP>" in ss.mask(f"x {PUBLIC}", [])
    assert ss.mask(f"x {VPNIP}:1", []).count("26.x.x.x") == 1
    # IPv6 and party codes
    assert "2a02:85a3" not in ss.mask("from 2a02:85a3:0:0:8a2e:370:7334:1 here", [])


def test_net_rows_from_net_doctor(session):
    _md, _js, res = score(session)
    assert res["F2"]["status"] == ss.PROVEN          # the guest joined from a public address
    assert res["F5"]["status"] == ss.PROVEN          # the link was lost, the slot kept, the member rejoined
    assert res["F3"]["status"] == ss.NOT and res["A9"]["status"] == ss.NOT
    guest = GUEST_LINES + ["[coop] Party: link: refused by the host: wrong password or party code",
                           "[coop] Party: link: refused by the host: version mismatch with the host: patches differ"]
    write(session, "guest2.log", [GUEST_LINES[0], "[coop] Net: party networking on (guest, as GuestTwo, port 9307)"] + guest[2:])
    _md, _js, res = score(session)
    assert res["F3"]["status"] == ss.PROVEN and res["A9"]["status"] == ss.PROVEN


def test_net_doctor_failure_marks_the_row(tmp_path):
    write(tmp_path, "host.log", HOST_LINES[:2])
    write(tmp_path, "guest1.log", GUEST_LINES[:3] + ["[coop] Party: link: reconnecting: no traffic for 10 s"] * 6)
    _md, _js, res = score(tmp_path)
    assert any(r["status"] == ss.FAILED for r in (res["A1"], res["F1"], res["F2"]))


def test_save_hashes_a2(session, tmp_path):
    user = tmp_path / "game" / "user"
    (user / "savedata").mkdir(parents=True)
    (user / "savedata_party").mkdir()
    (user / "savedata" / "userdata0000").write_bytes(b"single")
    (user / "savedata_party" / "userdata0000").write_bytes(b"party1")
    pre = ss.hash_saves(tmp_path / "game")
    assert set(pre["files"]) == {"savedata/userdata0000", "savedata_party/userdata0000"}
    (session / "savehash_pre.json").write_text(json.dumps(pre))
    (user / "savedata_party" / "userdata0000").write_bytes(b"party2")
    (session / "savehash_post.json").write_text(json.dumps(ss.hash_saves(user)))
    _md, js, res = score(session)
    assert res["A2"]["status"] == ss.PROVEN and js["saves"][0]["party_changed"] == 1
    (user / "savedata" / "userdata0000").write_bytes(b"touched")
    (session / "savehash_post.json").write_text(json.dumps(ss.hash_saves(user)))
    _md, _js, res = score(session)
    assert res["A2"]["status"] == ss.FAILED and "userdata0000" in res["A2"]["fails"][0]["text"]


def test_a2_without_hashes_is_not_exercised(session):
    _md, _js, res = score(session)
    assert res["A2"]["status"] == ss.NOT and "savehash" in res["A2"]["missing"][0]


def test_zipped_logs_and_screenshots(tmp_path):
    with zipfile.ZipFile(tmp_path / "support_bundle.zip", "w") as z:
        z.writestr("logs/last_session.log", "\n".join(HOST_LINES))
        z.writestr("launcher.log", "Party: link: connected: slot 9")   # not a party log: ignored
    write(tmp_path, "guest1.log", GUEST_LINES)
    (tmp_path / "a8_system_menu.png").write_bytes(b"\x89PNG")
    _md, js, res = score(tmp_path)
    assert res["A1"]["status"] == ss.PROVEN
    assert any("support_bundle.zip!/logs/last_session.log" == l["file"] for l in js["logs"])
    assert res["A8"]["status"] == ss.PROVEN and res["A8"]["ev"][0]["file"] == "a8_system_menu.png"


def test_no_party_logs(tmp_path):
    write(tmp_path, "launcher.log", ["hello"])
    assert ss.run(tmp_path) is None
    assert ss.main([str(tmp_path)]) == 2


def test_rows_match_the_parity_doc():
    assert ss.ROW_IDS == ss.doc_row_ids()


def test_smoke_check_regexes_resolve():
    # every smoke_pattern() call in the row table must still find its regex in tools/mp/smoke_party.py
    assert ss.sp("check_cd", "host revive #") and ss.sp("check_emf", "guest: went through boss fog")
    with pytest.raises(KeyError):
        ss.smoke_pattern("check_cd", "no such text in the check")
    for r in ss.ROWS:
        for m in r["proof"] + r["fail"]:
            assert m.rx.pattern


def test_cli_writes_md_and_json(session, tmp_path, capsys):
    out_md, out_js = tmp_path / "o.md", tmp_path / "o.json"
    assert ss.main([str(session), "--md", str(out_md), "--json", str(out_js)]) == 0
    assert "| A1 |" in capsys.readouterr().out
    assert json.loads(out_js.read_text())["summary"]["rows"] == 49
    assert out_md.read_text().startswith("# Session score")


def test_runbook_has_a_block_for_every_row():
    text = (ROOT / "docs" / "party" / "PLAYTEST_RUNBOOK.md").read_text(encoding="utf-8")
    import re
    heads = " ".join(re.findall(r"^### (.*)$", text, re.M))
    missing = [rid for rid in ss.ROW_IDS if not re.search(r"(?<![A-Z0-9])" + rid + r"(?!\d)", heads)]
    assert not missing, missing
    assert "save_hash.py" in text and "Save support bundle" in text and "Max players" in text
