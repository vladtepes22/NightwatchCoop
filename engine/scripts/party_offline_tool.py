"""Commands for the launcher's "Copy party character to single-player" button (scripts/party_offline.py).

  list <user>                 JSON: party characters and the single-player slot each would replace
  copy <user> <party_slot>    copies into the same slot number of the single-player folder (backup first)
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import party_offline  # noqa: E402
from saveedit import store  # noqa: E402


def list_characters(user):
    solo = {r["slot"]: r for r in store.list_slots(user, "solo")}
    rows = []
    for r in store.list_slots(user, "party"):
        old = solo.get(r["slot"])
        rows.append({"slot": r["slot"], "name": r["name"], "level": r["level"],
                     "replaces": None if old is None else f'{old["name"]} (level {old["level"]})'})
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["list", "copy"])
    parser.add_argument("user", type=Path)
    parser.add_argument("slot", nargs="?", type=int)
    args = parser.parse_args(argv)
    if args.action == "list":
        print(json.dumps(list_characters(args.user)))
        return 0
    if args.slot is None:
        parser.error("copy needs the party slot")
    try:
        done = party_offline.copy_party_to_offline(args.user, args.slot, replace=True)
    except party_offline.CopyError as ex:
        print(str(ex), file=sys.stderr)
        return 1
    print(party_offline.describe(done["report"]))
    print(f"Written to single-player slot {done['solo_slot'] + 1}."
          + (f" Your single-player saves before it: {done['backup']}" if done["backup"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
