// SPDX-License-Identifier: GPL-3.0-or-later
// TEST AIDS for automated runs (tools/mp/instances.py); never on in normal play:
//
// (BB_TEST_NEWGAME / BB_TEST_PROLOGUE_SKIP = "party": only in a party process, BB_PARTY set - a
// single-player start that the party flow's menu restarts into the party game keeps its title.)
// BB_TEST_NEWGAME=1: a fresh character without a person at the pad. From the title of an EMPTY
//   save (New Game is the default item there; with a character in the save the title's default is
//   Continue and the run simply loads it) the driver presses, through the game's own pad path
//   (runtime_pad_director_press, no OS input):
//     New Game -> Brightness: Down, Next -> Controls: Up (wraps to Next), Next ->
//     the "Paleblood" movie: Options (skips) -> Contract: Cross on Enter Name (the ImeDialog HLE
//     fills BB_TEST_NAME, default "Hunter") -> Up (wraps to Finish), Cross -> "Finalise contract?"
//     Left (YES), Cross -> loading -> the opening cutscene: Options until 12410000 is ON.
//   Each step waits for the game's answer where one is visible in-process (the movie starting:
//   sceAvPlayerAddSource; the name box: ImeDialog count; the loading screen; the flags), so a step
//   the game ignored is pressed again instead of running the rest of the script blind. The
//   default character (Male, Young, Milquetoast) is accepted. Log lines start with "newgame:".
//   Taps are held 0.12 s: the menus auto-repeat a held direction (0.25 s moved two rows).
//   Event driven: a press that needs the next screen waits for it (the game looking up texts it
//   had not looked up in the last second, BbGameMenu::TextActivity, quiet for 0.3 s; or the movie
//   / name box / loading counters) and goes as soon as it is there; the old fixed waits remain
//   only as each step's maximum (no screen signal: the game menu hooks off).
//
// BB_TEST_PROLOGUE_SKIP=1: a prologue fast-forward, so the party's start (party_start.h) can be
//   tested on fresh characters without fighting through the clinic, the first death and the first
//   Dream. Once the opening is done (12410000 ON, 9180 OFF) in the player's own world it does what
//   the game would have done by the end of the prologue: flags 9402 (first death seen), 9401
//   (first Dream cutscene), 12417810 (sickroom lamp lit), 12101020 / 12101021 + choice flags
//   72101000 / 72101011 (the Dream messengers' weapons), 12100105 + 9404 (Doll awake), 9180 OFF;
//   gives the starting weapons the messengers' shop rows hand out (ShopLineupParam 2000: weapon
//   7000000, 2010: weapon 14000000 - they are shop rows with price 0, not item lots); and warps
//   to BB_TEST_PROLOGUE_WARP (default 2412951 = Central Yharnam lamp; "sickroom" 2412950,
//   "dream" 2102950, a ReturnPoint id, or 0 = stay). Not the vanilla way to get there: Insight
//   stays 0, the Notebook / Beckoning messengers' items are not given (the party director's bell
//   grants cover the bells), the save keeps these flags. Use test saves only.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace coop {

struct GameSnapshot; // game_state.h

// ---- Pure parts (unit-tested: tests/test_party_newgame.cpp, party-newgame-test) ----

constexpr std::uint32_t kPadUp = 0x10, kPadRight = 0x20, kPadDown = 0x40, kPadLeft = 0x80, kPadOptions = 0x8,
                        kPadCross = 0x4000, kPadCircle = 0x2000;

/// What the title / character creation driver sees each tick.
struct NewGameInput {
    double t = 0;             ///< seconds, monotonic
    bool title_ready = false; ///< no world, the title takes input (party: link up, FROM sign-in over)
    int ime_count = 0;        ///< ImeDialogs opened so far
    int movie_count = 0;      ///< movies started so far
    bool world_up = false, loading = false;
    bool flags_ok = false, opening_done = false, cutscene = false;
    /// Screen changes (BbGameMenu::TextActivity): +1 for each message the game looks up that it had
    /// not looked up in the last second - a new screen or dialog building its texts. 0 / unchanged
    /// when unknown (the game menu hooks off): the driver then waits its maximum per step.
    std::uint64_t screen_seq = 0;
    double screen_quiet = 1e9; ///< seconds since the last such lookup
};

enum class NewGamePhase {
    WaitTitle,
    NewGame,     ///< Cross on New Game
    Brightness,  ///< Down, Cross (Next)
    Controls,    ///< Up, Cross (Next)
    WaitMovie,   ///< the Paleblood movie: Options
    Contract,    ///< Cross on Enter Name until the name box opened
    Finish,      ///< Up, Cross, Left, Cross
    WaitLoad,
    Opening,     ///< in the world: Options until 12410000
    Done,
    Failed,
};
const char* NewGamePhaseName(NewGamePhase p);

class NewGameDriver {
public:
    /// One tick: the buttons to hold now (0 = none). `log` gets a line when something happened.
    std::uint32_t Step(const NewGameInput& in, std::string* log);
    NewGamePhase phase() const { return phase_; }
    bool finished() const { return phase_ == NewGamePhase::Done || phase_ == NewGamePhase::Failed; }

private:
    struct Tap {
        std::uint32_t buttons;
        double gap;        ///< at least this long after the release before the next tap
        double wait = 0;   ///< > 0: first wait (at most this long after the previous release) for a
                           ///< new screen since the previous press, quiet for kSettle
    };
    void Queue(std::initializer_list<Tap> taps);
    void Enter(NewGamePhase p, double t, std::string* log, const char* why);
    NewGamePhase phase_ = NewGamePhase::WaitTitle;
    std::vector<Tap> queue_;
    std::size_t next_ = 0;
    double tap_at_ = -1, busy_until_ = 0, phase_at_ = 0, ready_since_ = -1, released_at_ = 0;
    std::uint64_t screen_mark_ = 0; ///< screen_seq at the last press
    int tries_ = 0, ime_base_ = 0, movie_base_ = 0;
    bool holding_ = false;
};

/// A scripted menu walk for test fixtures (BB_PARTY_DIRECTOR_TEST stand=...,seq=...): one token
/// per press, '/'-separated: cross, circle, square, triangle, up, down, left, right, wait (a 2.5 s
/// pause). `button@text` waits until a message text containing `text` (ASCII, case-insensitive) is
/// on screen - looked up after the previous press (screens look their texts up once, when they
/// build) - for 0.4 s, then
/// presses (no text 3.5 s after a Cross: that Cross once more - it may have been swallowed; 7 s at
/// most, then it presses anyway and logs it). A '?' prefix makes such a step optional: no
/// text in 3 s -> skipped without a press (a first-time tutorial message). A '+' prefix waits for any
/// new screen (TextActivity; 8 s at most). Each press is held 0.12 s (menus auto-repeat a held
/// direction: 0.25 s moved a 2-item dialog twice), the next one no sooner than 1 s after it.
/// l1 / r1: the shoulder buttons (a menu's tabs, e.g. the chalice ritual's families).
/// Also (the party flow's live walk, tools/mp/flow_live.py): options; mark=NAME (a 2.5 s pause
/// named in the log: a screenshot point); type=TEXT / enter / esc (TEXT typed into the open text
/// box, or Enter / Esc there, in-process by the caller: TakeTyped, "\n" = Enter, "\x1b" = Esc). A
/// pause (wait, mark=) is not a press: a text wait after it looks back to the last real press. A
/// '~' prefix is accepted and means the default (looked up since the previous press).
/// A '!' prefix makes a text wait strict: no text in 10 s -> the script stops (STUCK), no blind press.
class MenuScript {
public:
    static constexpr std::uint32_t kWait = 0xffffffffu;
    /// type= / enter / esc steps (TakeTyped).
    static constexpr std::uint32_t kType = 0xfffffffeu;
    /// False (and *bad = the token) on an unknown token.
    bool Parse(const std::string& text, std::string* bad = nullptr);
    bool finished() const { return stuck_ || (next_ >= steps_.size() && !holding_); }
    std::size_t size() const { return steps_.size(); }
    /// Once a tick; the buttons to hold now. `log` gets a line when a press starts.
    /// `text_seen(text)`: the last time (t units) a text containing `text` was looked up, < 0 never.
    std::uint32_t Step(double t, std::uint64_t screen_seq, double screen_quiet, std::string* log,
                       const std::function<double(const std::string&)>& text_seen = {});
    bool stuck() const { return stuck_; }
    /// A `type=` / enter / esc step started: its keys, once.
    bool TakeTyped(std::string* text) {
        if (!has_typed_) return false;
        has_typed_ = false;
        *text = typed_;
        return true;
    }

private:
    struct Press {
        std::uint32_t buttons = 0;
        bool new_screen = false;
        std::string name;
        std::string text;   ///< @text: wait for it
        bool optional = false;
        bool strict = false;  ///< '!': no text in 10 s -> STUCK instead of a blind press
        std::string typed;  ///< type=TEXT / enter / esc
    };
    static std::uint32_t Held(std::uint32_t b) { return b == kWait || b == kType ? 0 : b; }
    std::string typed_;
    bool has_typed_ = false;
    bool stuck_ = false, retried_ = false;
    std::uint32_t last_buttons_ = 0;
    double pressed_at_ = -1e9, text_since_ = -1;
    std::vector<Press> steps_;
    std::size_t next_ = 0;
    bool holding_ = false;
    double until_ = 0, wait_from_ = -1;
    std::uint64_t mark_ = 0;
};

/// BB_TEST_PROLOGUE_WARP: empty -> 2412951 (Central Yharnam lamp); "0"/"off"/"none" -> 0 (no
/// warp); "sickroom" -> 2412950; "dream" -> 2102950; a number -> that ReturnPoint id.
std::uint32_t ParsePrologueWarp(const char* text);

/// The flags the fast-forward sets (in this order) and clears.
struct FlagWrite {
    std::uint32_t flag;
    bool on;
};
const std::vector<FlagWrite>& PrologueSkipFlags();
/// The starting weapons it gives (weapon ids, EquipParamWeapon).
struct StartWeapon {
    std::uint32_t weapon, choice_flag;
    const char* hand;
};
const std::vector<StartWeapon>& PrologueSkipWeapons();

// ---- Game glue (party director tick, main thread) ----

/// BB_TEST_NEWGAME or BB_TEST_PROLOGUE_SKIP is set.
bool NewGameTestRequested();
/// The title driver is on and has not finished: the director's auto-Continue stays off.
bool NewGameOwnsTitle();
/// The prologue fast-forward has started and not finished (flags set, warp pending / loading):
/// the director holds its bell grants and bells until the player stands at the warp target.
bool PrologueSkipBusy();
/// `title_ready`: see NewGameInput.
void NewGameTestTick(const GameSnapshot& s, bool title_ready);

} // namespace coop
