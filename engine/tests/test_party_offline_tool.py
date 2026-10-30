r"""scripts/party_offline_tool.py: the launcher button's list and copy commands (synthetic saves only)."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tests"))
import party_offline as PO  # noqa: E402
import party_offline_tool as T  # noqa: E402
from test_party_offline import PATH, make_user  # noqa: E402
from test_saveedit import make_save  # noqa: E402


def test_list_names_what_a_copy_would_replace(tmp_path, capsys):
    user = make_user(tmp_path, solo_slots=(0,))
    assert T.main(["list", str(user)]) == 0
    rows = json.loads(capsys.readouterr().out)
    assert [r["slot"] for r in rows] == [0]
    assert rows[0]["replaces"].startswith("Solo (level")


def test_copy_writes_the_same_slot_and_reports(tmp_path, capsys):
    user = make_user(tmp_path, solo_slots=(0,))
    assert T.main(["copy", str(user), "0"]) == 0
    out = capsys.readouterr().out
    assert "single-player slot 1" in out and "save_backups" in out
    assert (user / PO.SOLO / PATH / "userdata0000").read_bytes() != make_save(name="Solo")


def test_copy_of_an_empty_party_slot_fails_with_a_message(tmp_path, capsys):
    user = make_user(tmp_path)
    assert T.main(["copy", str(user), "5"]) == 1
    assert "no party character" in capsys.readouterr().err
