# Mod manifest: `nightwatch.json` (Nightwatch 0.3.0)

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
| `name`, `version`, `author` | no | strings, shown in logs (and later on the Mods page) |
| `features` | no | the engine features the mod uses (list of names). 0.3.0 has: `hit_effects`, `weapon_clip_groups` |

Other keys are ignored, so later Nightwatch versions can add optional ones.

## What Nightwatch does with it (at every game start)

- **Too old a Nightwatch:** `requires` is compared as major.minor.patch (a pre-release suffix such as `-beta.1` is
  ignored on both sides, so a 0.3.0 beta runs a mod that requires 0.3.0). If Nightwatch is older, the mod is **not
  loaded** and the log says `Mods: <folder>: needs Nightwatch 0.3.0 or newer (this is 0.2.9): not loaded`.
- **Unknown feature:** the mod is not loaded: `Mods: <folder>: uses the feature "x", which Nightwatch 0.3.0 does not
  have: not loaded`.
- **Broken manifest** (not JSON, no `id` / `requires`, wrong types): the mod is not loaded, with the reason.
- **Fine:** `Mods: <folder>: manifest gold-bolt-paper 1.0.0, requires 0.3.0, features hit_effects`.

Nightwatch 0.2.9 and older do not read `nightwatch.json`: they load the mod's game files and ignore its tables. A mod
whose game files are harmless without the feature (a visual feature) therefore still works there, without the extra.

## Feature tables

`nightwatch/<feature>.json`, one per feature, used only when the manifest lists the feature. Rules for every table:

- Tables are JSON objects with a `version` (each feature's page says which) and their rows.
- A bad table (not JSON, wrong version, one bad row) is dropped **whole**, with one log line naming the row; the mod's
  game files still load.
- Tables of all enabled mods merge in load order (the Mods page order): **a later mod wins per key** (each feature's
  page names its key).
- A table format, once shipped, only grows by new optional fields; a breaking change gets a new feature name.

## Co-op classes

Every feature is one of two kinds, named on its page:

- **Visual:** each player's game decides alone, from state it already has (for example the attacker's active
  SpEffects). Players may run different mods; a player without the mod sees the vanilla look. Not in the party check.
- **Gameplay:** must give the same result on every machine (inputs synced, or decided by the host). Such features
  count in the party check, so all players need the same mods. (None in 0.3.0.)

## Features

- [`hit_effects`](hit_effects.md) (visual): replace the elemental hit overlay while the attacker has a SpEffect on.
- [`weapon_clip_groups`](weapon_clip_groups.md) (gameplay): give a weapon its own attack clip group while it keeps its `wepmotionCategory`.
