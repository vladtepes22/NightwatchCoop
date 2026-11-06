// SPDX-License-Identifier: GPL-3.0-or-later
#include "party_options.h"
#include "../net/party_vpnlobby.h"

namespace party {

namespace {
const char* onoff(int v) { return v ? "on" : "off"; }
const char kPrefix[] = "[host options: ";
const char kModsPrefix[] = "[host mods: ";
}  // namespace

bool HeadstonesFromEnv(const char* v, bool lamps) {
    return lamps || !(v && v[0] == '0' && !v[1]);
}

bool GuestTalkFromEnv(const char* v) {
    return !(v && v[0] == '0');
}

std::string options_text(const HostOptions& o) {
    std::string t;
    const auto add = [&](const char* k, int v) {
        if (v < 0) return;
        t += (t.empty() ? "" : " ") + std::string(k) + "=" + std::to_string(v);
    };
    if (o.max_players > 0) add("max", o.max_players);
    add("lamps", o.lamps);
    add("headstones", o.headstones);
    add("talk", o.talk);
    add("droproll", o.droproll);
    return t;
}

bool options_parse(const std::string& text, HostOptions* out) {
    if (!out || text.empty() || text.size() > 120) return false;
    HostOptions o;
    std::size_t p = 0;
    while (p < text.size()) {
        std::size_t end = text.find(' ', p);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(p, end - p);
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos || eq + 2 != item.size()) return false;
        const std::string key = item.substr(0, eq);
        const int v = item[eq + 1] - '0';
        if (v < 0 || v > 9) return false;
        if (key == "max") {
            if (v < 2 || v > 5) return false;
            o.max_players = v;
        } else if (key == "lamps" || key == "headstones" || key == "talk" || key == "droproll") {
            if (v > 1) return false;
            int& dst = key == "lamps" ? o.lamps : key == "headstones" ? o.headstones : key == "talk" ? o.talk : o.droproll;
            dst = v;
        } else {
            return false;
        }
        p = end + 1;
    }
    *out = o;
    return true;
}

std::string reject_with_options(const std::string& reason, const HostOptions& host) {
    const std::string t = options_text(host);
    return t.empty() ? reason : kPrefix + t + "] " + reason;
}

bool reject_split_options(const std::string& text, std::string* reason, HostOptions* host) {
    const std::string pre = kPrefix;
    if (text.compare(0, pre.size(), pre) == 0) {
        const std::size_t close = text.find("] ", pre.size());
        HostOptions o;
        if (close != std::string::npos && options_parse(text.substr(pre.size(), close - pre.size()), &o)) {
            if (reason) *reason = text.substr(close + 2);
            if (host) *host = o;
            return true;
        }
    }
    if (reason) *reason = text;
    return false;
}

std::string reject_with_mods(const std::string& reason, const std::string& label) {
    return bbnet::lobby::valid_mods_label(label) ? kModsPrefix + label + "] " + reason : reason;
}

bool reject_split_mods(const std::string& text, std::string* reason, std::string* label) {
    const std::string pre = kModsPrefix;
    if (text.compare(0, pre.size(), pre) == 0) {
        const std::size_t close = text.find("] ", pre.size());
        if (close != std::string::npos) {
            const std::string l = text.substr(pre.size(), close - pre.size());
            if (bbnet::lobby::valid_mods_label(l)) {
                if (reason) *reason = text.substr(close + 2);
                if (label) *label = l;
                return true;
            }
        }
    }
    if (reason) *reason = text;
    return false;
}

std::vector<OptionDiff> options_diff(const HostOptions& host, const HostOptions& mine) {
    std::vector<OptionDiff> v;
    if (host.max_players > 0 && mine.max_players > 0 && host.max_players != mine.max_players) {
        v.push_back({"Max players", "BB_PARTY_MAX", std::to_string(host.max_players), std::to_string(mine.max_players),
                     std::to_string(host.max_players), true});
    }
    const auto flag = [&](const char* label, const char* env, int h, int m) {
        if (h < 0 || m < 0 || h == m) return;
        v.push_back({label, env, onoff(h), onoff(m), h ? "1" : "0", false});
    };
    flag("Guest lamps", "BB_PARTY_GUEST_LAMPS", host.lamps, mine.lamps);
    flag("Headstones", "BB_PARTY_HEADSTONES", host.headstones, mine.headstones);
    flag("Guest services", "BB_PARTY_GUEST_TALK", host.talk, mine.talk);
    flag("Shared enemy drop roll", "BB_PARTY_SHARED_DROP_ROLL", host.droproll, mine.droproll);
    return v;
}

std::string option_diff_line(const OptionDiff& d) {
    const std::string name = d.env == "BB_PARTY_MAX" ? "Max players (4-player party)" : d.label;
    return name + ": host " + d.host_text + ", you " + d.mine_text;
}

std::string adopt_explanation(const std::vector<OptionDiff>& diffs) {
    bool restart = false;
    std::string list;
    for (const OptionDiff& d : diffs) {
        list += (list.empty() ? "" : "; ") + option_diff_line(d);
        restart = restart || !d.live;
    }
    if (!restart) return {};
    return "The host's party options differ (" + list +
           "). They are read when the game starts: restart to join - the launcher uses the host's for your next "
           "join (your saved settings stay as they are).";
}

std::vector<std::pair<std::string, std::string>> adopt_env(const HostOptions& h) {
    std::vector<std::pair<std::string, std::string>> v;
    if (h.max_players >= 2) v.push_back({"BB_PARTY_MAX", std::to_string(h.max_players)});
    if (h.lamps >= 0) v.push_back({"BB_PARTY_GUEST_LAMPS", h.lamps ? "1" : "0"});
    if (h.headstones >= 0) v.push_back({"BB_PARTY_HEADSTONES", h.headstones ? "1" : "0"});
    if (h.talk >= 0) v.push_back({"BB_PARTY_GUEST_TALK", h.talk ? "1" : "0"});
    if (h.droproll >= 0) v.push_back({"BB_PARTY_SHARED_DROP_ROLL", h.droproll ? "1" : "0"});
    return v;
}

std::string adopt_file_text(const HostOptions& host) {
    std::string t = "# the host's party options for this PC's next join (written by the game, read once by the launcher)\n";
    for (const auto& [k, val] : adopt_env(host)) t += k + "=" + val + "\n";
    return t;
}

}  // namespace party
