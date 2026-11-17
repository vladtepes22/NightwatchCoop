"""Command line. Reads the player's game (and mods) and writes a mod folder.
  randomizer.py code --seed 123 [--chance 50] [--scaling off] ...     prints a code
  randomizer.py generate --game <folder> [--update <folder>] --code R2-... --out <Mods folder> [--mods <folder> ...] [--base-label nocturne@1.0]
The game and mod folders hold dvdroot_ps4. Mods are given in load order, last wins.
generate prints 'progress <step> <done> <total>' lines while it works, then 'wrote <folder>'."""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else __file__.rsplit("/", 1)[0])
from seedcode import CodeError, decode, encode


def options_from(args):
    out = {}
    if args.chance is not None:
        out["enemy_chance"] = args.chance
    if args.exclude:
        out["exclude"] = tuple(args.exclude)
    if args.include_only:
        out["include_only"] = tuple(args.include_only)
    if args.oops_all:
        out["oops_all"] = args.oops_all
    if args.scaling:
        out["scaling"] = {"slot": 0, "off": 1, "fixed": 2}[args.scaling]
    if args.fixed_effect:
        out["fixed_effect"] = args.fixed_effect
    if args.rewards:
        out["rewards"] = {"slot": 0, "replacement": 1}[args.rewards]
    if args.variety:
        out["variety"] = args.variety
    if args.bosses:
        out["boss_swap"] = 1
    if args.treasure:
        out["treasure"] = 1
    if args.shops:
        out["shops"] = 1
    if args.drops:
        out["drops"] = 1
    if args.headroom:
        out["headroom"] = args.headroom
    if args.treasure_by_type:
        out["treasure_by_type"] = 1
    if args.starters:
        out["starters"] = 1
    if args.starter_melee is not None:
        out["starter_melee"] = args.starter_melee
    if args.starter_guns is not None:
        out["starter_guns"] = args.starter_guns
    if args.boss_enemy_chance is not None:
        out["boss_enemy_chance"] = args.boss_enemy_chance
    if args.boss_enemy_max is not None:
        out["boss_enemy_max"] = args.boss_enemy_max
    if args.boss_enemy_hp is not None:
        out["boss_enemy_hp"] = args.boss_enemy_hp
    if args.chalice_materials:
        out["chalice_materials"] = 1
    for flag in ROUND2_FLAGS:
        if getattr(args, flag):
            out[flag] = 1
    if args.bell_maidens:
        out["bell_maidens"] = {"off": 0, "normal": 1, "difficult": 2}[args.bell_maidens]
    return out


# boss round 2 switches, CLI --witch-mix, --shadows-mix ... (on/off option ids 26-45, 48, 50 in seedcode)
ROUND2_FLAGS = ("witch_mix", "shadows_mix", "emissary_mix", "failures_mix", "chalice_bosses", "hard_rom",
                "hard_amygdala", "hard_ebrietas", "difficult_enemies", "second_stage", "away_from_home", "free_poses",
                "starter_armor", "shadows_summons", "chalice_enemies", "bosses_as_enemies", "coiled_host", "gem_roulette", "cursed_gems",
                "huge_bosses_as_enemies", "no_winter_lanterns", "scorched_beast")
ROUND2_HELP = {"witch_mix": "each Witch of Hemwick body draws its own boss", "shadows_mix": "each Shadow draws its own boss",
               "emissary_mix": "every Celestial Emissary becomes a boss with its own HP",
               "failures_mix": "every Living Failure becomes a boss with its own HP",
               "chalice_bosses": "strong Chalice Dungeon bosses join the boss pool",
               "hard_rom": "Rom fights with her deepest chalice row", "hard_amygdala": "Amygdala fights with her deepest chalice row",
               "hard_ebrietas": "Ebrietas fights with her deepest chalice row",
               "difficult_enemies": "replaced ordinary enemies draw from the difficult list where it fits",
               "second_stage": "Gascoigne, Ludwig and the Orphan turn into another random boss at their phase change, "
                               "wherever they fight, one that fights nowhere else (two chalice bosses join the pool for "
                               "it). Default: their own second phase while it has no arena of its own; else a boss that "
                               "fights nowhere else; else their fight ends with phase one",
               "away_from_home": "a replaced enemy is not one of its map's own kinds, where another kind fits",
               "free_poses": "an enemy the map's events force an animation on (a pose, appearing, a scripted attack) may become "
                             "any enemy, which stands where it lacks that animation",
               "starter_armor": "a new character starts in a random head, chest, arms and legs piece the game hands out",
               "shadows_summons": "the Shadows of Yharnam summon random enemies (without it, nothing is summoned)",
               "chalice_enemies": "the Chalice Dungeons' regular enemy kinds can replace enemies in the main game",
               "gem_roulette": "world blood gems, half the 1-2 Blood Vial / Quicksilver Bullet pickups and every enemy gem drop "
                               "become droplet gems with three effects fixed by the seed, stronger in later areas",
               "cursed_gems": "with --gem-roulette: about 15 %% of gems trade a side effect for a curse and are stronger for it",
               "bosses_as_enemies": "now and then a boss replaces an ordinary enemy, as a plain enemy (no boss health "
                                    "bar, fog wall, music or boss rewards)",
               "huge_bosses_as_enemies": "with --bosses-as-enemies, the huge bosses (wider than any ordinary enemy: "
                                         "Paarl, Ebrietas, Rom, Amelia, Ludwig ...) may stand in enemy slots too; they can "
                                         "get stuck in narrow places",
               "coiled_host": "with boss swaps: Nocturne's Coiled Host (the Shadows of Yharnam's second phase) joins the "
                              "boss pool and his own place draws a boss; no effect without Nocturne v2.0.0 or later",
               "scorched_beast": "with boss swaps: Nocturne's Scorched Beast (the Lesser Demon of Old Yharnam, with his second head) "
                                 "joins the boss pool and his plaza draws a boss; no effect without Nocturne v5.0.0 or later",
               "no_winter_lanterns": "Winter Lanterns never replace an enemy (their Frenzy reaches from afar); their own "
                                     "places still change as usual"}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("code")
    make.add_argument("--seed", type=int, required=True)
    make.add_argument("--chance", type=int)
    make.add_argument("--exclude", type=int, nargs="*")
    make.add_argument("--include-only", type=int, nargs="*", help="model numbers: only these replace an ordinary enemy")
    make.add_argument("--oops-all", type=int)
    make.add_argument("--scaling", choices=("slot", "off", "fixed"))
    make.add_argument("--fixed-effect", type=int)
    make.add_argument("--rewards", choices=("slot", "replacement"))
    make.add_argument("--variety", type=int)
    make.add_argument("--bosses", action="store_true")
    make.add_argument("--treasure", action="store_true")
    make.add_argument("--shops", action="store_true")
    make.add_argument("--drops", action="store_true")
    make.add_argument("--headroom", type=int)
    make.add_argument("--treasure-by-type", action="store_true")
    make.add_argument("--starters", action="store_true")
    make.add_argument("--starter-melee", type=int, help="free melee weapons offered at the start, 1-5 (default 3)")
    make.add_argument("--starter-guns", type=int, help="free firearms offered at the start, 1-3 (default 2)")
    make.add_argument("--boss-enemy-chance", type=int,
                      help="with --bosses-as-enemies: percent chance for a map to hold bosses as enemies, 0-100 in steps of 5 (default 25)")
    make.add_argument("--boss-enemy-max", type=int,
                      help="with --bosses-as-enemies: most bosses one map holds as enemies, 1-5 (default 1)")
    make.add_argument("--boss-enemy-hp", type=int, choices=(8, 10, 12, 14, 16),
                      help="with --bosses-as-enemies: a boss as an enemy has this many times the base HP of a typical enemy "
                           "of its area, then the area's scaling, never above its own boss fight's HP (default 8)")
    make.add_argument("--chalice-materials", action="store_true", help="shuffle chalice ritual materials too (not recommended)")
    for flag in ROUND2_FLAGS:
        make.add_argument("--" + flag.replace("_", "-"), action="store_true", help=ROUND2_HELP[flag])
    make.add_argument("--bell-maidens", choices=("off", "normal", "difficult"), help="One Reborn's bell maidens (default off: they stay)")
    gen = sub.add_parser("generate")
    gen.add_argument("--game", required=True)
    gen.add_argument("--update", help="the game's update folder when it is kept beside the game, not copied into it")
    gen.add_argument("--code", required=True)
    gen.add_argument("--out", required=True)
    gen.add_argument("--mods", nargs="*", default=[])
    gen.add_argument("--name", help="folder name for a test build, instead of the code")
    gen.add_argument("--base-label", help="the base mods' lobby label part, such as nocturne@1.0 (recorded in the manifest)")
    sub.add_parser("build-id", help="prints this generator's build id (part of every world's input hash and the label's g:)")
    args = parser.parse_args()
    if args.command == "code":
        print(encode(args.seed, options_from(args)))
        return 0
    if args.command == "build-id":
        from buildid import BuildIdError, build_id
        try:
            print(build_id())
        except BuildIdError as e:
            print(e, file=sys.stderr)
            return 6
        return 0
    from buildid import BuildIdError
    from generate import FolderInUse, generate
    try:
        decode(args.code)
    except CodeError as e:
        print("code %s: %s" % (e.kind, e), file=sys.stderr)  # the launcher shows the last stderr line
        return 3 if e.kind == "newer" else 4
    def progress(step, done, total):  # read by the launcher's progress bar as the lines come
        print("progress %s %d %d" % (step, done, total), flush=True)
    try:
        folder, spoiler = generate(args.game, args.mods, args.out, args.code, args.name, args.base_label, progress, args.update)
    except FolderInUse as e:
        print(e, file=sys.stderr)
        return 5
    except BuildIdError as e:  # no world under an id that may not be this generator's
        print(e, file=sys.stderr)
        return 6
    report = spoiler["report"]
    changed = sum(m["changed"] for m in report["maps"].values())
    print("wrote", folder)
    print("slots changed", changed, "| new NPC rows", spoiler["npc_rows_added"], "| slots left vanilla for lack of room", report["stayed_no_room"])
    arenas = report.get("arenas", {})
    if arenas:
        bodies = [k for a in arenas.values() for k in a["bodies"].values()]
        left = sum(len(a["left"]) for a in arenas.values())
        modes = {}
        for a in arenas.values():
            modes[a["mode"]] = modes.get(a["mode"], 0) + 1
        print("boss bodies swapped", len(bodies), "| fighters", len(set(bodies)), "| arenas", len(arenas),
              "(%s)" % ", ".join("%d %s" % (n, m) for m, n in sorted(modes.items())), "| bodies left vanilla for lack of room", left)
    for option, why in sorted(report.get("skipped_options", {}).items()):
        print("option %s skipped: %s" % (option, why))
    return 0


sys.exit(main())
