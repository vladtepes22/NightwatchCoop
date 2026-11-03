// SPDX-License-Identifier: GPL-3.0-or-later
// Registers the offensive-name display filter (party_display_name.h) for the game: the shared matcher
// (name_filter.h) behind the setting party_hide_bad_names. Logs one line per masked name (the tier and the mask, never
// the name itself).
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "../bbport_settings.h"
#include "party/name_filter.h"
#include "party/party_display_name.h"

namespace {

std::mutex g_mu;
std::map<std::string, std::string> g_masked;  // real name -> display name, for names found offensive
std::map<std::string, bool> g_clean;          // checked and fine (bounded)

std::string DisplayName(const std::string& real) {
    if (!BbSettings::Get().party_hide_bad_names) return real;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_masked.find(real);
        if (it != g_masked.end()) return it->second;
        if (g_clean.count(real)) return real;
    }
    const std::string tier = coop::namefilter::Reason(real);
    std::lock_guard<std::mutex> lk(g_mu);
    if (tier.empty()) {
        if (g_clean.size() > 512) g_clean.clear();
        g_clean[real] = true;
        return real;
    }
    if (g_masked.size() > 64) g_masked.clear();
    const std::string mask = coop::namefilter::MaskText(real);
    g_masked[real] = mask;
    std::printf("[coop] name filter: a party member name was hidden (tier %s) -> %s\n", tier.c_str(), mask.c_str());
    return mask;
}

std::string ScrubText(const std::string& text) {
    if (!BbSettings::Get().party_hide_bad_names) return text;
    std::string out = text;
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& kv : g_masked) {
        size_t pos = 0;
        while ((pos = out.find(kv.first, pos)) != std::string::npos) {
            out.replace(pos, kv.first.size(), kv.second);
            pos += kv.second.size();
        }
    }
    return out;
}

}  // namespace

namespace bbname {
void InstallGameFilter() { Register(&DisplayName, &ScrubText); }
}  // namespace bbname
