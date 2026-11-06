// SPDX-License-Identifier: GPL-3.0-or-later
// 0.7.8: the options the HOST owns and a guest adopts instead of being refused (hostopts078).
//
// Max players (BB_PARTY_MAX), Guest lamps (BB_PARTY_GUEST_LAMPS), Headstones (BB_PARTY_HEADSTONES), Guest
// services (BB_PARTY_GUEST_TALK) and the Shared enemy drop roll (BB_PARTY_SHARED_DROP_ROLL). Most of them
// decide hooks installed at boot, so a running game cannot change them: the guest learns the host's values
// BEFORE its game starts (the discovery announcement carries them; the launcher sets them for that launch)
// or, with the game already running, from the host's refusal, which carries them as a "[host options: ...]"
// prefix; the guest writes them to <user dir>/party_adopt.txt and the launcher's next join uses them.
// Max players also reaches a connected guest live (WELCOME, party_fourp FourpTick). Pure code, no game.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace party {

/// A value is -1 when not known. Flags are 0 / 1.
struct HostOptions {
    int max_players = 0;  // 2..5, 0 unknown
    int lamps = -1;       // BB_PARTY_GUEST_LAMPS (the ini's Guests use lamps)
    int headstones = -1;  // BB_PARTY_HEADSTONES (on unless "0", or lamps on)
    int talk = -1;        // BB_PARTY_GUEST_TALK (on unless "0")
    int droproll = -1;    // BB_PARTY_SHARED_DROP_ROLL (on unless "0")
    bool operator==(const HostOptions& o) const {
        return max_players == o.max_players && lamps == o.lamps && headstones == o.headstones && talk == o.talk &&
               droproll == o.droproll;
    }
};

/// BB_PARTY_HEADSTONES as party_guestsvc reads it: on unless "0", and always on with guest lamps.
bool HeadstonesFromEnv(const char* v, bool lamps);
/// BB_PARTY_GUEST_TALK: on unless "0" (first character).
bool GuestTalkFromEnv(const char* v);

/// "max=4 lamps=1 headstones=1 talk=1 droproll=1" (unknown values left out).
std::string options_text(const HostOptions& o);
/// Strict: only the known keys, values 0/1 (max 2..5); false on anything else (untrusted input).
bool options_parse(const std::string& text, HostOptions* out);

/// The Mismatch refusal's reason with the host's options in front: "[host options: ...] reason".
std::string reject_with_options(const std::string& reason, const HostOptions& host);
/// The reverse: the reason without the prefix; `*host` is set only when a valid prefix was there (true).
bool reject_split_options(const std::string& text, std::string* reason, HostOptions* host);

/// 0.7.9: a Mismatch refusal over gameplay mods carries the host's mods label (party_vpnlobby.h mods_label, the
/// label the VPN lobby and LAN discovery show) after the options prefix: "[host mods: nocturne@1.0.1] reason", so
/// a refused guest can name the mods it needs. An older guest shows it as part of the reason. `label` empty or
/// not valid (valid_mods_label): the reason unchanged.
std::string reject_with_mods(const std::string& reason, const std::string& label);
/// The reverse (after reject_split_options): `*label` is set only for a valid prefix with a valid label (true).
bool reject_split_mods(const std::string& text, std::string* reason, std::string* label);

struct OptionDiff {
    std::string label;      // player-facing name
    std::string env;        // the variable that sets it (launcher / ini)
    std::string host_text;  // "4" / "on"
    std::string mine_text;
    std::string env_value;  // the host's value as the variable takes it ("4", "1")
    bool live = false;      // the running game takes the host's by itself (Max players)
};
/// Every option known on both sides whose values differ.
std::vector<OptionDiff> options_diff(const HostOptions& host, const HostOptions& mine);
/// "Max players (4-player party): host 4, you 3" / "Headstones: host on, you off".
std::string option_diff_line(const OptionDiff& d);
/// What the guest is told with the game already running: what differs, that the launcher's next start uses
/// the host's values (the saved preference stays), and to restart. Empty when nothing needs a restart.
std::string adopt_explanation(const std::vector<OptionDiff>& diffs);

/// The environment pairs of the host's known values (the launcher's list uses the same mapping).
std::vector<std::pair<std::string, std::string>> adopt_env(const HostOptions& host);
/// <user dir>/party_adopt.txt: "BB_PARTY_MAX=4" lines for the host's known values (the launcher's next join).
std::string adopt_file_text(const HostOptions& host);

}  // namespace party
