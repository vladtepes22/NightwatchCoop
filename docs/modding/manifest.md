# Mod manifest: `nightwatch.json` (Nightwatch 0.3.0)

Index: [README.md](README.md).

A mod folder may hold a `nightwatch.json` beside its `dvdroot_ps4`. It says which Nightwatch the mod needs and which
engine features it uses. A mod without one loads exactly as before (and can use no feature table).

```
Mods/
  Gold Bolt Paper/
    dvdroot_ps4/...            game files, as before
    nightwatch.json            the manifest
    nightwatch/
      hit_effects.json         one table per feature (see the feature's page)
```

```json
{
  "id": "gold-bolt-paper",
  "name": "Gold Bolt Paper",
  "version": "1.0.0",
  "author": "you",
  "requires": "0.3.0",
  "features": ["hit_effects"]
}
```

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | a short stable id (non-empty string) |
| `requires` | yes | the oldest Nightwatch that can run the mod: `X.Y.Z` (or `X.Y`) |
| `name`, `version`, `author` | no | strings (`version` is shown in the log line below) |
| `features` | no | the engine features the mod uses (list of non-empty strings). 0.3.0 has: `hit_effects`, `weapon_clip_groups` |

`requires` is digits and dots only (each part up to 99999); a `-suffix` or `+suffix` after it is ignored. The file may
be at most 1 MiB and may start with a UTF-8 BOM. Other keys are ignored, so later Nightwatch versions can add
optional ones.

## What Nightwatch does with it (at every game start)

- **Too old a Nightwatch:** `requires` is compared as major.minor.patch (a pre-release suffix such as `-beta.1` is
  ignored on both sides, so 0.3.0-beta.1 runs a mod that requires 0.3.0). If Nightwatch is older, the mod is **not
  loaded** and the log says `Mods: <folder>: needs Nightwatch 0.3.0 or newer (this is 0.2.9): not loaded`.
- **Unknown feature:** the mod is not loaded: `Mods: <folder>: uses the feature "x", which Nightwatch 0.3.0 does not
  have: not loaded`.
- **Broken manifest** (not JSON, not an object, no `id` / `requires`, wrong types, `requires` not a version): the mod
  is not loaded: `Mods: <folder>: bad manifest: nightwatch.json has no "id": not loaded`. The text after `bad
  manifest:` names the problem, for example `nightwatch.json is not a JSON object`, `nightwatch.json: "requires" is
  not a version like 0.3.0: abc`, `nightwatch.json: "features" is not a list`.
- **Fine:** `Mods: <folder>: manifest gold-bolt-paper 1.0.0, requires 0.3.0, features hit_effects` (`-` stands for a
  missing `version`, `none` for no features).
- Once per start: `Mods: Nightwatch 0.3.0, features: hit_effects, weapon_clip_groups`.
- `<folder>` is the mod's folder name. A mod that is not loaded contributes none of its files.

Nightwatch 0.2.9 and older do not read `nightwatch.json`: they load the mod's game files and ignore its tables. A mod
whose game files are harmless without the feature (a visual feature) therefore still works there, without the extra.

## Feature tables

`nightwatch/<feature>.json` inside the mod folder, one per feature. Rules for every table:

- A table is read only when the manifest lists the feature. A table file without that listing (or in a mod without a
  manifest) is ignored: `Mods: <folder>: has nightwatch/<feature>.json but its nightwatch.json does not list
  <feature>: not used`.
- A table is a JSON object `{"version": 1, "rows": [...]}`: `version` is the whole number 1 (each feature's page
  says which), `rows` a list. At most 1 MiB and 4096 rows per file; unknown fields are ignored.
- A bad table (not JSON, wrong version, one bad row, too large) is dropped **whole**, with one log line naming the
  first bad row (counted from 1), for example `Mods: <folder>: nightwatch/hit_effects.json: row 2 needs whole
  numbers ...: its hit_effects table is not used`. The mod's game files still load.
- Tables of all enabled mods merge in load order (the Mods page order: `mods.json` `order`, then the rest by name):
  **a later mod wins per key** (each feature's page names its key). The engine gets one file per feature in `out/`
  (`nightwatch_<feature>.txt`); none when no mod gave a row.
- A table format, once shipped, only grows by new optional fields; a breaking change gets a new feature name.

## Co-op classes

Every feature is one of two kinds, named on its page:

- **Visual:** each player's game decides alone, from state it already has (for example the attacker's active
  SpEffects). Players may run different mods; a player without the mod sees the vanilla look. Not in the party check.
- **Gameplay:** must give the same result on every machine. The merged rows of such a feature are hashed into the
  party check (`out/party_mods.txt`), so all players need the same mods and tables (`weapon_clip_groups` in 0.3.0).

## Features

- [`hit_effects`](hit_effects.md) (visual): replace the elemental hit overlay while the attacker has a SpEffect on, or
  draw one for an element the game draws nothing for.
- [`weapon_clip_groups`](weapon_clip_groups.md) (gameplay): give a weapon its own attack clip group while it keeps its
  `wepmotionCategory`.
