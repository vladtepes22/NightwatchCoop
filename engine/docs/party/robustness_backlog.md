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
