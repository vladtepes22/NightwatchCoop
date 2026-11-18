# Randomizer live harness (headless solo boss-arena chains)

Proves a generated world's boss arenas in a headless game: per arena it warps to the lamp, walks in through the fog,
R1s the boss (AI + damage), kills it (HP steps at handovers, HP 0, ForceDead fallback) and reads the result.
Everything it writes goes to the tree's ignored `work\harness\` (worlds, steps, plans, logs).

1. Generate the world into `work\harness\worlds\<name>\`:
   `python -I tools\randomizer\randomizer.py generate --game C:\Games\CUSA03173 --code <R2-...> --out work\harness\worlds\<name> [--mods <Nocturne folder>]`
2. Build the chain: `python -I tools\randomizer-harness\chain.py <name> <out> <arena key> [...]` (keys: `arenas.BY_KEY`;
   `van` instead of a world = vanilla). Writes `work\harness\<out>.steps` and `<out>.json`.
3. Run it (one game at a time; takes the shared lock `AwakenSeamless\work\game-runs.lock`, waits 90 s after the last
   release): `bash tools/randomizer-harness/run.sh <run name> <name> <seconds> work/harness/<out>.steps <port>`
   (needs `engine/out` built in this tree). Run it in the background; it quits the game 30 s after the last step.
4. Read: `python -I tools\randomizer-harness\sum.py tools\randomizer-harness\read.py work\harness\<out>.json work\artifacts\smoke-<run name>\session.log`
   (one line per arena: attacks, R1 damage, defeat flag, faults); `read.py` alone gives the full per-arena detail.

Limits (LIVE): the two Hunter's Dream fights are started by flag, so their bosses stand idle (same in vanilla); a
handover that waits for the boss's AI (the Orphan's) never runs there. A boss 5-6 m away the whole time takes no R1 hits:
"0 damage" then proves nothing.
