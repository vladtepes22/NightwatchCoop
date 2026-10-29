# Robustness: after 100 % (backlog)

Items agreed for later (0.6.2 planning, 2026-10-15). They come after the parity rows A-H reach
100 % (`ERSC_PARITY.md`, "parity before plus"); 0.6.2 shipped the first three robustness items
(save safety after party sessions, host crash recovery, the launcher's update notice; see
`save_safety.md`, `network_robustness.md` section 8 and `version_check.md`).

| # | Item | What "done" means | How to measure |
|---|---|---|---|
| 4 | **Long unattended soak** (pre-release step) | A 4-8 h two-player session on one PC (loopback, `tools/mp/instances.py run --count 2 --party`, the director's test walks / fights on a loop) with no crash, no rejoin it did not need, and no drift: process memory (private bytes), VRAM, handle and thread counts and the frame time stay flat after the first hour | Sample both processes every minute (`tools/soak.sh` / a small Python sampler over `GetProcessMemoryInfo`, `GetProcessHandleCount`, DXGI `QueryVideoMemoryInfo`) plus the party perf log (`tools/party/perf_summary.py`); pass = growth under 2 % per hour after warm-up, frame-time p99 at the end within 10 % of the first hour. Run it before every release, on the release build (LTO). |
| 5 | **Frame cost of the party code** | The party layer costs at most 1 ms per frame in a two-player session | Same scene, same camera, solo vs. 2 players (host side and guest side): the per-zone timers (`party_perf.h`, `BB_PARTY_PERF=1`) and the frame time from PresentMon (`tools/windows/analyze_presentmon.py`); report mean and p99 of the party zones' sum and the frame-time difference. One session per measurement sweep (single-run rule). |
| 6 | **One co-op guide page in the launcher** | A player-facing page (Party tab, "Party help") that walks through host / join / crash / undo / update in the player's words, replacing the long text box | Written from `docs/PARTY.md` and the README party section; checked by someone who did not write it, on a fresh PC |

Not in this list (already done): guests rejoining by themselves after a guest crash (C6), short
outages keeping the session (F5), the version check naming what differs (A9).

## EMEVD stale-off audit (0.7.7, emevdaudit077)

Pattern (found in Rom's lake jump, 13204820): a host-copy event runs on a guest, skips its "on" behind a
multiplayer guard (1003[5] / 1003[105] / 1003[109]) and still runs the "off" later, undoing the guest's own state.
`python -I -X utf8 tools/party/stale_off_tool.py scan GAME/event` lists every such event (all maps + common).
Result over the whole game (1.09 EMEVD): exactly one real instance, 13204820 (2004[15] invincibility, fixed by the
client-only rule `13204820:2004:15@8`). The other hits are false positives: 12901693 (2003[36] item grants,
reward ids only), and with `--all` the boss-room rematch object toggles 2005[3] and boss speffect ids 2004[53].

Cross-event sweep (every instruction aimed at entity 10000 in banks 2003/2004/2005/2009; invincibility 2004[15],
undead 2004[12], gravity 2004[10], map hit 2004[31], speffects 2004[8]/[21], [54]) found no other un-guarded reset
of a state a host-trigger event skips for a guest. Unproven, low risk, not fixed:
- 12204892/12204893 (and the same pair in every boss map, 12304812/3, 12404842/3, 12414732/3, 12424812/3 ...):
  2004[54] "map hit & gravity off outside own world" for entity 10000, on near the fog (2 m) / off far (4 m). Gated
  to chr type 0 and a no-op in the player's own world; a guest in the host's world gets them by design.
- 12104870 (Moon Presence gaze): 2004[12] undead on, off 5 frames later, both unguarded, runs per player.
- 12405263 / 12800431 (Amygdala / Nightmare Frontier throw): 2004[12] undead on with no off in the event; the
  warp that follows ends it (host-only, end-if-client at [16]).
- 13204470 (Byrgenwerth new-NPC summon landing): 2004[15] on/off on the summoned NPC (param), not a player.
