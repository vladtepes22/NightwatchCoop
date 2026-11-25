"""Writes r2_vectors.json: world codes and what they decode to, read by the Python tests and by the launcher's own
decoder tests. Run it only when the code format changes on purpose (then raise SCHEMA), never to make a test pass."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import seedcode as s  # noqa: E402

FULL = dict(boss_swap=1, treasure=1, shops=1, drops=1, treasure_by_type=1, starters=1,
            boss_cutscenes=1, chalice_materials=1, rewards=1, scaling=1, headroom=150, enemy_chance=75)


def raw(body, schema=s.SCHEMA, crc_delta=0, pad=0):
    """A code built by hand from a payload body, for the refused cases."""
    payload = s._varint(len(body)) + bytes(body)
    crc = s._crc10(payload) ^ crc_delta
    text = s._base32(payload)
    if pad:
        text = text[:-1] + s._ALPHABET[s._ALPHABET.index(text[-1]) | pad]
    text += s._ALPHABET[crc >> 5] + s._ALPHABET[crc & 31]
    return "R%d-" % schema + "-".join(text[i:i + 5] for i in range(0, len(text), 5))


def valid(name, seed, options, typed=None):
    code = s.encode(seed, options)
    return {"name": name, "code": typed or code, "canonical": code, "label": code.lower(), "seed": seed,
            "options": json.loads(json.dumps(dict(sorted(s.normalize(options).items()))))}


def refused(name, code, kind):
    return {"name": name, "code": code, "error": kind}


def main():
    seed = 0x89ABCDEF
    default_core = s._varint(20 << 11)  # every flag 0, enemy_chance 100 (step 20), headroom 100 (step 0)
    good = valid("all toggles", seed, FULL)["code"]
    vectors = [
        valid("defaults", 0, {}),
        valid("highest seed", (1 << 32) - 1, {}),
        valid("all toggles", seed, FULL),
        valid("enemy chance 25, headroom 300, scaling fixed", 7, {"enemy_chance": 25, "headroom": 300, "scaling": 2,
                                                                  "fixed_effect": 7010}),
        valid("excluded models", seed, dict(FULL, exclude=(2630, 1100, 1260))),
        valid("area percents", 42, {"area_chance": ((2401, 25), (2200, 0), (3200, 100))}),
        valid("oops all and caps", 9, {"oops_all": 1260, "map_models": 40, "map_mb": 500, "big_models": 5, "variety": 8}),
        valid("boss round options", 11, dict(boss_swap=1, witch_mix=1, shadows_mix=1, emissary_mix=1, failures_mix=1, bell_maidens=1,
                                             chalice_bosses=1, hard_rom=1, hard_amygdala=1, hard_ebrietas=1, difficult_enemies=1)),
        valid("every fight mixed", 12, dict(boss_swap=1, witch_mix=1, shadows_mix=1, emissary_mix=1, failures_mix=1, bell_maidens=2, difficult_enemies=1, second_stage=1)),
        valid("second stage", 13, dict(boss_swap=1, second_stage=1)),
        valid("away from home", 14, dict(away_from_home=1)),
        valid("free poses", 15, dict(free_poses=1)),
        valid("starter armor", 16, dict(starter_armor=1)),
        valid("shadows summons", 17, dict(shadows_summons=1)),
        valid("chalice enemies", 18, dict(chalice_enemies=1)),
        valid("gem roulette", 20, dict(gem_roulette=1)),
        valid("cursed gems", 19, dict(gem_roulette=1, cursed_gems=1)),
        valid("bosses as enemies", 21, dict(bosses_as_enemies=1)),
        valid("bosses as enemies, chance, max, huge", 22, dict(bosses_as_enemies=1, boss_enemy_chance=60, boss_enemy_max=4,
                                                               huge_bosses_as_enemies=1)),
        valid("bosses as enemies, hp 16", 24, dict(bosses_as_enemies=1, boss_enemy_hp=16)),
        valid("coiled host", 23, dict(boss_swap=1, coiled_host=1)),
        valid("no winter lanterns", 25, dict(no_winter_lanterns=1)),
        valid("scorched beast", 26, dict(boss_swap=1, scorched_beast=1)),
        valid("moon presence phase two", 27, dict(boss_swap=1, moon_presence_two=1)),
        valid("include only", 26, dict(include_only=(1100, 2630), exclude=(2630,))),
        valid("typed lowercase with look-alikes", seed, FULL, good.lower().replace("0", "o").replace("1", "l")),
        valid("typed without group dashes", seed, FULL, "R2-" + good[3:].replace("-", "")),
        refused("checksum wrong", raw(bytes(4) + default_core, crc_delta=1), "bad"),
        refused("character outside the alphabet", "R2-U" + good[4:], "bad"),
        refused("cut short", good[:-4], "bad"),
        refused("padding bits set", raw(bytes(4) + default_core, pad=1), "bad"),
        refused("option written at its default", raw(bytes(4) + default_core + s._varint(8) + s._varint(24)), "bad"),
        refused("option out of range (scaling 3)", raw(bytes(4) + s._varint(20 << 11 | 3 << 9)), "bad"),
        refused("bell maidens 3", raw(bytes(4) + default_core + s._varint(30) + s._varint(3)), "bad"),
        refused("second stage 2", raw(bytes(4) + default_core + s._varint(36) + s._varint(2)), "bad"),
        refused("unknown core bit", raw(bytes(4) + s._varint(20 << 11 | 1 << s.CORE_BITS)), "newer"),
        refused("unknown option id", raw(bytes(4) + default_core + s._varint(63) + s._varint(1)), "newer"),  # an id no option uses
        refused("newer schema", raw(bytes(4) + default_core, schema=s.SCHEMA + 1), "newer"),
        refused("old schema R1", "R1-1M002-D98XR-60238-11R0G-Y08QE", "bad"),
        refused("not a code", "hello", "bad"),
    ]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "r2_vectors.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"schema": s.SCHEMA, "core_bits": s.CORE_BITS, "vectors": vectors}, f, indent=1)
        f.write("\n")
    print("wrote", path, len(vectors), "vectors")


if __name__ == "__main__":
    main()
