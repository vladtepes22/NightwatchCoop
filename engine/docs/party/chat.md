# Party text chat (0.7.8, CL-5b356c)

A short line any member types (keyboard) or picks from a list (pad) reaches every member and shows in a small fading
log in the sender's party colour. Off by default. Code: `gpu/shim/party/party_chat.{h,cpp}` (pure: sanitising, JSON, rate
limits, masking, the log), `party_chat_game.cpp` (box, send, receive), overlay `PartyChat()` in `bbport_overlay.cpp`,
`src/runtime_pad.c` (pad combo), unit test `tests/test_party_chat.cpp` (`party-chat-test`), smoke phase `pc` (group `party_chat`).

## Settings

| What | Where |
|---|---|
| Party chat (default OFF) | `bbport.ini party_chat`, System > My party: Chat & names (Nightwatch: in-game menu only); `BB_PARTY_CHAT=0/1` over the setting |
| Show others' chat (default on) | `party_chat_receive`, same pages. Off: others' lines are dropped on arrival (yours still show; the host still passes lines on) |
| Chat key (default `T`) | `party_chat_key=<SDL key name>` in `bbport.ini`, `BB_PARTY_CHAT_KEY` over it |
| Offensive words masked | `party_hide_bad_names` (the name filter's switch, default on): other players' lines only |

The **host's** setting decides whether the party has chat: a host with chat off ignores guests' lines and passes none on (a
guest with chat on then sees only its own lines).

## Input

* Keyboard: the chat key opens the overlay's text box (`bbgpu_text_input_begin`); Enter sends, Esc cancels. The game gets no
  input while the box is open (as for the party name box).
* Pad: the touchpad held + `L1` (Xbox pads and others without a touch surface: `Back` + `L1`, `src/runtime_pad.c`
  `ping_combo` / `back_click_combo`) opens the same box with the preset list (`bbgpu_text_input_begin_list`): d-pad picks,
  Cross sends, Circle cancels. Presets: Help! / Follow me / Wait / Boss here / Thanks / Going to the Dream. A keyboard at
  the same time still types a free line.
* Not available in a loading screen, a cutscene or while the game's own menu is open.

## Wire

EVENT `party_chat` `{"text":"...","t":<host clock ms>}`; a guest sends to the host, the host broadcasts. The host adds `slot`
and `who` (the roster's name; a guest's own values are never trusted) and relays a guest's line to every other connected
guest. No new link message type and nothing in the version identity (`docs/party/version_check.md`): a member without chat
(an older build: an event name it does not know is not handled by any chat or party code) ignores the event, and a build with chat joins any host.

## Limits and sanitising

* UTF-8 is checked strictly (a malformed text is refused whole); controls, bidi controls, zero-width characters, soft hyphen,
  variation selectors, tags, private use and combining marks are dropped; every space run becomes one space; the line is
  cut at 120 characters (and 480 bytes).
* The sender waits 1.5 s between lines; the host passes on at most 4 lines per 10 s from each member; a guest accepts at
  most 6 per 10 s per sender. A line older than 14 s (host clock) is dropped. The log keeps 6 lines, each shown 14 s
  (fading over the last 2.5 s).
* The receiver sanitises again (`FromJson`), so a modified sender cannot show invisible text.

## Masking

`MaskWords` (party_chat.cpp) splits a line at spaces, takes the core of each word (leading/trailing ASCII punctuation kept),
asks the name filter (`coop::namefilter`, strict + phrase + code tiers, the same normaliser as names) about each word, then
about runs of 2 to 4 words that hold no masked word yet (spaced-out letters, phrases), and as a last resort about the whole
line. A match is replaced by `*` per character. Only other players' lines are masked, and only while `party_hide_bad_names`
is on; the real text stays in memory (switching the setting takes effect at once). A hateful sender name is shown through
`bbname::Display` as everywhere.

## Privacy

Chat text is never logged: `party_log_game.cpp` keeps `party_chat` bodies out of the event trace at every level, and the game
side logs slots and lengths only (`Party: chat: line from slot 1 (12 characters)`, plus `N word(s) masked` where the filter
changed words). `BB_PARTY_LOG=trace` has no chat text (smoke `pc` checks all three logs).

## Tests

* `party-chat-test`: sanitising (invisible characters, malformed UTF-8, caps), JSON round trip and refusals, both rate limits,
  masking (words, punctuation, spaced letters, no false positives on the presets and Bloodborne vocabulary), the log.
* `pad-test`: the touchpad + `L1` combo (and chat off = the game's `L1`).
* Smoke `pc` (`python tools/mp/smoke_party.py --only party_chat`): 3 players under `BB_NET_SIM=dsl`; `BB_PARTY_CHAT_TEST`
  (`S:type=Text_with_underscores`, `S:pick=N`, `S:recv=0|1`, `S:hide=0|1`; S = seconds after the game is in a world) drives the real
  box and list. Checks host/guest and guest/guest delivery, masking on/off, mute, no text in the logs; screenshots.
