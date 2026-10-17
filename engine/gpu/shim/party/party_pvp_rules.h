// SPDX-License-Identifier: GPL-3.0-or-later
// I1, friendly fire / PvP (ERSC Judicator's Rulebook): the pure part, no game access (unit test:
// tests/test_party_pvp.cpp). The game side is party_pvp.h / party_pvp.cpp.
//
// How Bloodborne keeps co-op phantoms from hurting each other (1.09, our offsets; RE notes in
// docs/party/pvp.md): every ChrIns has a team type (+0x88; measured live: the host's body 19, a
// cooperator 20, on both games; map enemies 23), resolved by 0x18C2910 (SpEffect stateInfo 132
// makes it 29, indiscriminate). The
// game asks "is team b an enemy of team a" in two 32 x 32 byte tables, index a * 32 + b:
//   - the relation table *(0x53E4560) -> 0x5127B90 (data): attacks, lock-on, damage display;
//   - the SpEffect target table 0x4731560 (rodata): an attack's / bullet's effects on "enemies".
// Host and cooperator are friends there (19 -> 20, 20 -> 19 and 20 -> 20 are 0). Friendly fire
// opens exactly the cells between the teams of
// the party's player chrs, in both tables, on every member's game (the hit is detected by the
// attacker's game and accepted by the victim's), and puts the original bytes back when it is
// switched off or the session ends. Enemy, NPC and boss relations are never touched.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coop::pvp {

constexpr int kTeams = 32;             ///< the tables are kTeams x kTeams bytes
constexpr int kSessionRoleHost = 3;    ///< game_state.h RoleHost
constexpr int kSessionRoleClient = 6;  ///< game_state.h RoleClient

/// The rule acts only in a party session: a host with cooperators in its world, or a guest in the
/// host's world (solo play, the title and the guest's own world are never changed).
inline bool PartySession(int session_role, int cooperators) {
    return (session_role == kSessionRoleHost && cooperators > 0) || session_role == kSessionRoleClient;
}

/// The rule on this player: the host's own setting; on a guest only the host's rule once it came
/// (EVENT "balance"; before that, and a guest's own setting: off).
inline bool RuleInForce(bool host, bool own_setting, bool host_known, bool host_value) {
    return host ? own_setting : (host_known && host_value);
}

/// The table cells (row a = the asking team, column b = the other) to open between the party's
/// player chrs: every ordered pair of two different chrs (two guests share team 2: 2 -> 2 too).
/// Teams outside 1..31 are skipped (0 = none / unread). Sorted, no duplicates.
inline std::vector<std::pair<int, int>> TeamPairs(const std::vector<int>& teams) {
    std::vector<std::pair<int, int>> out;
    for (std::size_t i = 0; i < teams.size(); ++i) {
        for (std::size_t j = 0; j < teams.size(); ++j) {
            if (i == j) continue;
            const int a = teams[i], b = teams[j];
            if (a <= 0 || a >= kTeams || b <= 0 || b >= kTeams) continue;
            out.emplace_back(a, b);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// The toast on every member when the rule changes (and once on join when it is on).
inline const char* ToastText(bool on) {
    return on ? "Friendly fire ON: party members can hurt each other" : "Friendly fire OFF";
}

/// BB_PARTY_PVP_TEST (live verification): steps run in order, each DELAY s after the previous one
/// (the first after the party session is steady): "on@S" / "off@S" (host: the setting),
/// "face@S" (the local chr warped 1.1 m in front of the first other party player, facing it;
/// "face=T@S": the first other party player on team T, e.g. 20 for a guest-guest hit),
/// "hit=N@S" (N taps of R1, 1.3 s apart), "shoot=N@S" (N taps of L2: the firearm's bullets),
/// "use=N@S" (N taps of Square: the selected quick item, e.g. a thrown knife),
/// "item=GOODS@S" (test aid: the goods topped up to 10 and put in the first quick-item slot, selected),
/// "hp=N@S" (the local chr's HP set to N, a test aid for a quick kill), "log@S" (teams and table
/// cells).
struct TestStep {
    enum class Kind { On, Off, Face, Hit, Shoot, Use, Item, Hp, Log } kind = Kind::Log;
    int arg = 0;
    double delay = 0;
};

/// False on a malformed item (*bad = the item); empty / null text: no steps.
inline bool ParseTestSteps(const char* text, std::vector<TestStep>* out, std::string* bad = nullptr) {
    out->clear();
    if (!text) return true;
    std::string s(text);
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) {
            if (end >= s.size()) break;
            continue;
        }
        const std::size_t amp = item.find('@');
        std::string head = item.substr(0, amp);
        TestStep st;
        if (amp != std::string::npos) {
            char* e = nullptr;
            st.delay = std::strtod(item.c_str() + amp + 1, &e);
            if (!e || *e || st.delay < 0) {
                if (bad) *bad = item;
                return false;
            }
        }
        std::string arg;
        if (const std::size_t eq = head.find('='); eq != std::string::npos) {
            arg = head.substr(eq + 1);
            head = head.substr(0, eq);
        }
        if (head == "on") st.kind = TestStep::Kind::On;
        else if (head == "off") st.kind = TestStep::Kind::Off;
        else if (head == "face") st.kind = TestStep::Kind::Face, st.arg = arg.empty() ? 0 : std::atoi(arg.c_str());
        else if (head == "hit") st.kind = TestStep::Kind::Hit, st.arg = arg.empty() ? 1 : std::atoi(arg.c_str());
        else if (head == "shoot") st.kind = TestStep::Kind::Shoot, st.arg = arg.empty() ? 1 : std::atoi(arg.c_str());
        else if (head == "use") st.kind = TestStep::Kind::Use, st.arg = arg.empty() ? 1 : std::atoi(arg.c_str());
        else if (head == "item" && !arg.empty()) st.kind = TestStep::Kind::Item, st.arg = std::atoi(arg.c_str());
        else if (head == "hp" && !arg.empty()) st.kind = TestStep::Kind::Hp, st.arg = std::atoi(arg.c_str());
        else if (head == "log") st.kind = TestStep::Kind::Log;
        else {
            if (bad) *bad = item;
            return false;
        }
        const bool taps = st.kind == TestStep::Kind::Hit || st.kind == TestStep::Kind::Shoot || st.kind == TestStep::Kind::Use;
        if ((taps && (st.arg < 1 || st.arg > 50)) || (st.kind == TestStep::Kind::Face && (st.arg < 0 || st.arg >= kTeams)) ||
            (st.kind == TestStep::Kind::Item && st.arg <= 0)) {
            if (bad) *bad = item;
            return false;
        }
        out->push_back(st);
        if (end >= s.size()) break;
    }
    return true;
}

}  // namespace coop::pvp
