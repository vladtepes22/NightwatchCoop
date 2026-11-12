# Feature `sounds` (Nightwatch 0.3.0, cosmetic)

Index: [README.md](README.md). Co-op class: **cosmetic** (not in the party hash; each player hears what their own game
plays; a player without the mod hears the game's own sound).

Minimal mod:

```
Mods/Repeating Crossbow/
  dvdroot_ps4/...              the weapon, its bullets and effects (the bolt effect asks for sound s000252531)
  nightwatch.json              {"id": "repeating-crossbow", "requires": "0.3.0", "features": ["sounds"]}
  nightwatch/sounds.json       the table below
  sound/xbow_shot.wav          the file the row names
```

The game asks for a positioned sound by an event name: a type letter and 9 digits (`s000252531`: `s` sounds from
effects and TAE, `c` character sounds, ...). Normally FMOD looks the name up in the loaded sound banks; a new name
needs a new bank. `sounds` lets a mod answer a name with its own WAV file instead: the engine plays it at the
position the game gives, mixed with the game's audio. FMOD's per-event instance limit does not apply.

## Table: `nightwatch/sounds.json`

```json
{
  "version": 1,
  "rows": [
    {"event": "s000252531", "file": "sound/xbow_shot.wav", "volume": 1.0, "replace": false}
  ]
}
```

| Field | Meaning |
|---|---|
| `event` | required: the event name, one of the letters `a c f o p s m v x b g y z` and exactly 9 digits |
| `file` | required: a WAV file inside the mod, relative to the mod folder (`/` or `\`; no `..`, no drive), 1 byte..4 MiB |
| `volume` | 0..4, default 1 (1 = the file as it is at 3 m or closer) |
| `replace` | `true`: the game's own sound for this event is not played; `false` (default): both play (for a name in no bank, as a new name, the game plays nothing anyway) |

The manifest must list `"sounds"` in `features` ([manifest](manifest.md)). Key: `event`; a later mod's row replaces an
earlier one. A table that lists an event twice is refused. At most 256 rows per file and after the merge. A bad table is
dropped whole (`Mods: <folder>: nightwatch/sounds.json: row N needs ...: its sounds table is not used`); a row whose file
is missing or too big is dropped alone (`Mods: <folder>: sounds: <event>: <file> is missing ...: row not used`).

WAV: PCM 8/16/24/32-bit, float or ADPCM, any rate, mono or stereo (the engine converts it once at start to 48 kHz mono;
the sound is placed in the world, so stereo is mixed down). At most 30 s are played.

## What changes in game

- Every positioned request (TAE sound events, effect sounds, hit sounds; the game's function at 0x27D3A80) with a listed
  name plays the WAV once, at the position of the request. It does not follow a moving source.
- Volume falls off past 3 m (volume x 3 / distance) and the sound is not played past 60 m; it is panned left / right
  from the camera. Without a camera (title screen) it plays centred.
- Up to 32 mod sounds at once; a 33rd takes the place of the one that has played longest. It is mixed into the game's
  main output and follows the game's Sound Effects option and master fades like the game's own sound effects
  (`volume` x that level; a change of the slider applies to a sound already playing). See "Volume" below.
- 2D sounds (menus) are not covered.

## Volume (how the engine reads the game's level; 1.09 disassembly, 12 Nov 2026)

- The game's sound volume manager (pointer at 0x556BCE0, created by 0x1F8D1DA, built by 0x17F3F50) has six faders
  of {current, target, time left} from +0x1C: Master, MenuMaster, InGameMaster, Bgm, Se, Voice (names: its debug
  printer 0x17F67F0). Its frame update 0x17F4DC0 sets the Bgm / Se / Voice targets from the option bytes +4 / +5 / +6 of
  `[[0x553B130]+0x48]` (GameDataMan's option block) divided by 10, clamped to 0..1.
- Six FMOD categories (set up at 0x17F44D3, names from sprj_mixer.fev): Default, Ghost, SE (fader Se), Music (Bgm),
  Voice (Voice), Multi. Each gets fader x InGameMaster x MenuMaster (flags per category), sent with 0x27D7BC0 (sound
  message 6) when it changes; the value sent is kept at +0x6C of its 12-byte slot, for SE at manager +0x84. The Master
  fader goes to 0x27D7D60 (message 10, the master level), the value sent kept at +0xAC.
- The engine multiplies mod sounds by `[manager+0x84] x [manager+0xAC]` (each clamped to 0..1; unreadable = 1),
  read on the audio thread once per mixed buffer, and by the main port's own front volume (sceAudioOutSetVolume).
  Unproven by ear: that message 10 scales every category (LIKELY: one float, named Master in the debug list).

## Logs

- `Mods: sounds: N rows from M mods` (bb-start): the table was read.
- `feature sounds: installed (N rows from ...)`: the hook is in; `N` counts the rows whose WAV could be read.
- `sounds: <file>: not a WAV SDL can read (...)`: that row is not used.
- `sounds: s000252531 at (x, y, z): played (gains l / r, game SFX level v)`: the first 8 plays (v = the
  game's level at that moment, 0..1); `BB_SOUNDS_LOG=1` logs up to 400 and each
  file's length. `BB_SOUNDS=0` turns the feature off.
- `sounds: the game asks for c000000911 at (x, y, z)`: with `BB_SOUNDS_LOG=1`, the first 50 names the game asks for
  that no row lists: a way to find the event name of a sound you want to answer.
