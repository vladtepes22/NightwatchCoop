# Making Nightwatch mods (0.3.0)

A Nightwatch mod is a folder of game files with an optional manifest and optional feature tables. This page is the
index; the other pages are the details.

## What a mod is

```
Mods/
  My Mod/
    dvdroot_ps4/...            game files (loose files, laid over the game's)
    nightwatch.json            manifest (optional, needed for feature tables)
    nightwatch/
      <feature>.json           one table per engine feature the mod uses
```

- **Loose files:** a file under `dvdroot_ps4` replaces the game's file of the same path. When two enabled mods ship
  the same file, the later mod wins the whole file (no merging inside files).
- **Load order:** the order on the Mods page (`mods.json` `order`); mods not listed there follow by name.
- **Manifest:** `nightwatch.json` says which Nightwatch the mod needs and which features it uses
  ([manifest.md](manifest.md)). A mod without one loads as before and cannot use a feature table.
- **Feature tables:** JSON rows that configure an engine feature, merged across all enabled mods.

## Features in 0.3.0

| Feature | Co-op class | What it does |
|---|---|---|
| [`hit_effects`](hit_effects.md) | visual | replaces the elemental hit overlay while the attacker has a SpEffect on; draws one for an element the game draws nothing for |
| [`weapon_clip_groups`](weapon_clip_groups.md) | gameplay | gives a weapon its own attack clip group while it keeps its `wepmotionCategory` |

## Co-op classes

- **Visual:** each player's game decides alone. Players may run different mods; a player without the mod sees the
  vanilla look. The table is not in the party version check.
- **Gameplay:** the result must be the same on every machine. The merged rows are hashed into the party version
  check (`out/party_mods.txt`), so every player needs the same mods and tables.

Game files are classed the same way: texture, shader, sound, font and similar files do not count in the party check;
parameters, events, animations and the like do.

## Version policy

- A mod names the oldest Nightwatch that runs it (`requires`). An older Nightwatch that reads manifests does not load
  the mod and says why in its log. (Nightwatch 0.2.9 and older do not read manifests: they load the game files and
  ignore the tables.)
- A feature table format, once shipped, only grows: new fields are optional, and unknown fields are ignored. A
  breaking change gets a new feature name, so an existing mod keeps working.
- A mod naming a feature the running Nightwatch does not have is not loaded.

## How to test

Start the game with the mod enabled, then read `logs\last_session.log` (a crash restart starts a new log; the crash
is then in `previous_session.log`). Look for:

- `Mods: Nightwatch <version>, features: hit_effects, weapon_clip_groups`: what this build offers.
- `Mods: <folder>: manifest <id> <version>, requires <x.y.z>, features <list>`: your manifest was read. Or
  `Mods: <folder>: ...: not loaded` with the reason.
- `Mods: hit_effects: N rows from M mods` / `Mods: weapon_clip_groups: N rows from M mods`: your table was read. A bad
  table logs `... its <feature> table is not used` with the row.
- `feature hit_effects: installed ...` / `feature weapon_clip_groups: installed ...`: the engine hook is in.
- Per-feature debug switches (`BB_HIT_EFFECTS_LOG=1`, `BB_WEAPON_CLIP_GROUPS_LOG=1`) log each use; see the pages.

The exact lines and their meaning are on each page.
