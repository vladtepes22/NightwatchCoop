// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.7 (CL-2eec68): splitscreen mode, for several game windows on one screen (the Nucleus Co-op splitscreen
// mod). The pure rules, header only (unit test: tests/test_party_splitscreen.cpp, ninja -C out/gpu
// party-splitscreen-test).
//
// One switch, party_splitscreen (bbport.ini, env BB_PARTY_SPLITSCREEN, launcher My Party, System > My party:
// Display), off by default, each player's own (nothing is sent to the party). On:
//   - the party HUD shows only the compass: per companion one row with the direction arrow (in the member's
//     colour) and the distance, no names, health bars, pings, state texts or rot line (the party_hud switch
//     still decides whether the panel is there at all);
//   - no name tags above the companions (party_name_tags / party_overhead are left as the player set them);
//   - party messages are cut to errors (below), unless the player chose Off.
// The messages setting party_messages (All / Errors only / Off) is separate and works without the switch.
//
// What an error is. Party messages come in two kinds: the notice (SetLastEvent: a toast in a corner, also the
// Party page's "Last event") and the alert (SetAlert: a message box). Every alert is an error: the host refused
// us, a connection warning, the host did not come back, the Chalice Dungeon cannot be entered. A notice is an
// error when its text says the connection or the join failed or the game fell out of step (IsErrorNotice):
// lost, failed, unstable, blocked, can't / could not reach, did not come back, waiting for the host, error,
// stuck, refused, kicked, not allowed, desync, mismatch, timed out, party is full, wrong password.
// Everything else (joined, left, travelling, gathering, votes, item notices, boss and bell notices) is
// information that only "All" shows. Only the corner notices and message boxes are filtered: the Party page's
// "Last event" line and the log keep everything.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace coop::splitscreen {

enum class Messages { All = 0, Errors = 1, Off = 2 };
enum class Kind { Notice, Alert };

inline Messages ClampMessages(int v) { return static_cast<Messages>(std::clamp(v, 0, 2)); }

/// The ini / env value: "all", "errors" ("error", "errors only"), "off", or 0 / 1 / 2. Anything else: All.
inline Messages ParseMessages(const std::string& text) {
    std::string t;
    for (const char c : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
    if (t == "1" || t.rfind("error", 0) == 0) return Messages::Errors;
    if (t == "2" || t == "off" || t == "none") return Messages::Off;
    return Messages::All;
}
inline const char* MessagesName(Messages m) {
    return m == Messages::Errors ? "errors" : m == Messages::Off ? "off" : "all";
}

/// The mode in force: splitscreen turns All into Errors only; Errors and Off stay as picked.
inline Messages Effective(bool splitscreen, Messages picked) {
    return splitscreen && picked == Messages::All ? Messages::Errors : picked;
}

/// Whether the text of a notice reports a failure (see the header). Case-insensitive.
inline bool IsErrorNotice(const std::string& text) {
    std::string t;
    for (const char c : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kWords[] = {
        "lost",          "failed",        "unstable",       "blocked",      "can't reach",    "cannot reach",
        "could not",     "did not come back", "waiting for the host", "error", "stuck",        "refused",
        "kicked by",     "not allowed",   "desync",         "mismatch",     "timed out",      "party is full",
        "wrong password", "wrong or missing"};
    for (const char* w : kWords) {
        if (t.find(w) != std::string::npos) return true;
    }
    return false;
}

/// Whether a party message is shown. An alert is an error by kind; a notice by its text.
inline bool MessageShown(Messages effective, Kind kind, const std::string& text) {
    switch (effective) {
    case Messages::All: return true;
    case Messages::Off: return false;
    case Messages::Errors: return kind == Kind::Alert || IsErrorNotice(text);
    }
    return true;
}

/// The party HUD panel draws only the compass rows.
inline bool CompassOnly(bool splitscreen) { return splitscreen; }
/// The name tags above the companions are drawn (`setting` = party_name_tags, `overhead` = party_overhead).
inline bool NameTagsShown(bool splitscreen, bool setting, int overhead) {
    return !splitscreen && setting && overhead != 0;
}

}  // namespace coop::splitscreen
