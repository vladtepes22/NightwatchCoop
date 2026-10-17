// SPDX-License-Identifier: GPL-3.0-or-later
// The in-game party flow (docs/party/ingame_flow.md): starting, joining and leaving a party game
// from the game's own menus instead of the launcher.
//
// Two kinds of game process, chosen at boot exactly as before:
//   solo   no BB_PARTY: the party layer stays inert (no sockets, no hooks, the offline title,
//          the single-player saves in user\savedata).
//   party  BB_PARTY=host|join: the party layer, the party patches, the online title and ALWAYS
//          the separate party saves (user\savedata_party, runtime_savepolicy.c).
// A menu action that needs the other kind (or the other role) does not switch in place: it
// writes the new environment to <user>\party_relaunch.bat and ends the game with exit code
// kRelaunchExitCode; run.bat runs that file, prepares the patches again and starts the game
// (docs/party/ingame_flow.md §2 has the evidence for why switching in place is not possible).
// The new process continues the party character on its own (the director's auto-Continue) or
// stops at the title for New Game.
//
// Persistence: <user>\party.json - the player's party name, the last role, the host address or
// party code a joiner uses, the password (Windows: DPAPI CryptProtectData, readable by this
// Windows user only; elsewhere plain text), the party port, whether a party character exists
// and the host's code secret (the party code stays the same from one session to the next).
//
// The pure parts (JSON, the action planner, the relaunch file) are unit-tested by
// tests/test_party_flow.cpp (party-flow-test); the game glue is in party_flow.cpp too, behind
// PARTY_FLOW_NO_GAME for the test build.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace party::flow {

/// run.bat / run.sh: "start the game again with <user>\party_relaunch.bat applied".
constexpr int kRelaunchExitCode = 75;
constexpr const char* kSettingsFile = "party.json";
constexpr const char* kRelaunchFile = "party_relaunch.bat";

/// user\party.json.
struct Saved {
    std::string name;          ///< party name (online id), empty: the user name
    std::string role;          ///< "host" / "join": the last party role ("" none yet)
    std::string host_address;  ///< joiner: a party code (BBP3-...) or host[:port]
    std::string password;      ///< in memory only in clear text (stored protected on Windows)
    int port = 0;              ///< the party port, 0 = default (9307)
    bool has_character = false;///< a party character reached the world in a party session
    int slot = -1;             ///< the party character's save slot when known (-1 unknown)
    std::string host_secret;   ///< host: the party code's secret (16 hex digits)
    bool password_plain = false; ///< read: the file held the password in clear text
};

/// JSON text <-> Saved. `protect` stores the password with DPAPI (Windows only; elsewhere and
/// when DPAPI fails, in clear text as "password"). A protected password that cannot be
/// unprotected (another Windows user, another PC) reads as empty.
std::string ToJson(const Saved& s, bool protect = true);
bool FromJson(const std::string& text, Saved* out, std::string* error);
bool Load(const std::string& path, Saved* out, std::string* error);
bool Store(const std::string& path, const Saved& s, std::string* error);

/// What the player picked.
enum class Action {
    NewParty,      ///< title: a new party game as host (character creation follows)
    JoinParty,     ///< join the saved host (title: also a new character when none exists)
    ContinueParty, ///< title: the party character, in the last role
    HostParty,     ///< party process: host (relaunch when this process joins)
    LeaveParty,    ///< party process: leave / stop hosting (stays in the party save)
    SinglePlayer,  ///< party process: back to the single-player game (relaunch, solo)
};
const char* ActionName(Action a);

/// Where the action is taken.
struct Context {
    bool party_process = false; ///< BB_PARTY set for this run
    bool host = false;          ///< this party process hosts
    bool party_save = false;    ///< the loaded saves are the party saves (savedata_party)
    bool in_world = false;      ///< a character is loaded (not the title)
    bool link_active = false;   ///< party process: the link runs (hosting / joined / connecting)
    Saved saved;
};

/// The plan for one action: refused (`error`), done in this process (`local`) or a relaunch with
/// these variables (an empty value unsets the variable).
struct Plan {
    enum class Local { None, Rejoin, Leave } local = Local::None;
    bool relaunch = false;
    std::vector<std::pair<std::string, std::string>> env;
    std::string error;    ///< why not (shown on the page's help line)
    std::string message;  ///< what happens ("Restarting into the party game ...")
    Saved saved;          ///< party.json after the action (role, ...)
    bool ok() const { return error.empty(); }
};
Plan PlanAction(Action a, const Context& c);

/// The party role / action rows only make sense with the party save loaded (never sync into a
/// solo character): false with a single-player character loaded.
bool PartyActionsAllowed(const Context& c);

/// `set "K=V"` lines for run.bat (values limited to [A-Za-z0-9_.:@-]; false otherwise).
bool RelaunchBat(const Plan& p, std::string* out, std::string* error);

/// The party name a player gets when party.json has none (the first party action): the user name
/// (its [A-Za-z0-9_-], blanks as '_', at most 11; "Hunter" when nothing is left) + '-' + 4 base-36
/// digits of `random`. The host admits one player per name, and every menu player used to be
/// "Hunter": the second friend was refused ("the name Hunter is already in the party").
std::string DefaultName(const std::string& user, std::uint32_t random);

/// A party address as typed: trimmed; "" when empty. Accepts a party code or host[:port].
std::string NormalizeAddress(const std::string& text);

// ---- game glue (party_flow.cpp; not in the unit test) ----

/// The party flow is on (BB_PARTY_FLOW != 0, default on).
bool Enabled();
/// This process: party (BB_PARTY set) / host / the party saves loaded.
Context CurrentContext();
/// user\party.json as last read or stored (cached; re-read after an edit).
Saved Current();
/// Runs an action: local ones queue the status command; a relaunch writes party.json and the
/// relaunch file, shuts the party link down and ends the process (a background thread; never
/// blocks the caller). Returns the plan (its error when refused).
Plan Run(Action a);

/// Text entry for a setting (the window's text box, as the character name): Address, Password,
/// Name. Begin opens it (false when another one is open); Poll (each frame) stores the result in
/// party.json when confirmed.
enum class Field { Address, Password, Name };
bool BeginEdit(Field f);
void Poll();
bool Editing();

/// Status for the page: the last message / error of an action ("" none).
std::string LastMessage();

/// Party runtime defaults (party_runtime.cpp): the menu flow's settings when the environment
/// does not set them (BB_PARTY_CODE, BB_PARTY_PASSWORD, BB_PARTY_NAME).
std::string RuntimeCode();
std::string RuntimePassword();
std::string RuntimeName();
/// Host: the saved secret (hex) when this run was started from the menu flow, else "".
std::string RuntimeHostSecret();
/// The host made a secret: kept in party.json for the next session.
void StoreHostSecret(const std::string& hex);
/// The director: a party character reached the world in this party session (party.json).
void NoteCharacterInWorld();
/// The save slot (0-9) of the character the game loaded last (GameDataMan +0x50 -> +2000), -1
/// when unknown. Per-character party files (party_summoned_gives.txt entries) carry it.
int CurrentSaveSlot();

/// BB_PARTY_FLOW_TEST=<action>[@seconds]: runs that action once at the title (tests).
void TestTick(bool at_title, bool world_up);
/// The rows on the game menu's Party page (party_flow_rows.cpp): before BbGameMenu's Define.
void RegisterMenuRows();
/// Each presented frame (BbGameMenu::Poll): the text box, the party character note, the test.
void Tick();

} // namespace party::flow
