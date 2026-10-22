// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op at the start (see party.h). Ported from the seamless fork's scripts/patches.py
// (party_patches, patch_is_cosmetic, party_patch_set) and scripts/party_launch.py (run_loop).
#include "party.h"


#include <algorithm>
#include <chrono>
#include <regex>
#include <set>
#include <thread>

#include "../gpu/party_env.h"
#include "xml.h"

namespace start {

const char* const kDisableHttp = "Disable HTTP Requests";
const char* const kSkipNetworkChoice = "Skip Online/Offline Choice";
const char* const kSkipNetworkChoiceOnline = "Party: Skip Online/Offline Choice (Online)";

namespace {

// The seamless patches (docs/party/seamless_rules.md), unless BB_PARTY_SEAMLESS=0.
const std::vector<std::string> kSeamless{"Party: Bells anywhere", "Party: Bells after boss defeated",
                                         "Party: Keep session on map reload", "Party: SOS sign timeout 30s"};
const char* const kNoInsight = "Party: Bells without Insight";
// Join timing (docs/party/join_timing.md): local pacing, left out of the version check.
const std::vector<std::string> kFastMessages{"Party: Fast multiplayer messages", "Party: Hide bell searching messages"};
const std::vector<std::string> kFastJoin{"Party: Fast join", "Party: Fast rejoin"};
const char* const kFastSign = "Party: Fast bell sign";

// Patches that change only what one machine draws or plays back: friends may differ in them.
const std::set<std::string> kCosmetic{
    "30 FPS++", "60 FPS++", "90 FPS++", "Uncap FPS++", "Sprint Fix (High FPS)",
    "Optimal 1080p", "Increased Graphics Heap Sizes",
    "Disable AA", "Disable Chromatic Aberration", "Disable DoF", "Disable Dynamic Light Shadows (perf increase)",
    "Disable Motion Blur (perf increase)", "Disable SSAO", "Enable Screen Space Reflections (READ NOTE)",
    "Model LOD -2 (Highest)", "Model LOD 1 (Lower)", "Model LOD 2 (Lowest)", "Performance Patch (perf increase)",
    "Intel Black Tonemap Fix", "Intel 12th Gen+ SFX workaround", "FMOD Crash Fix",
    "50% Text scale", "Skip Intro", "Unlock Game Region", "Bookmark and Capture outputs",
    "Increased camera distance", "Disable Camera Auto Rotation via Movement", "Sensitive Analog Input (easier to run)",
    "Party: Fast join", "Party: Fast rejoin", "Party: Fast bell sign", "Party: Fast multiplayer messages",
    "Party: Hide bell searching messages",
};

bool PatchIsCosmetic(const std::string& name, const std::string& mark) {
    if (mark == "cosmetic") return true;
    if (mark == "gameplay") return false;
    return kCosmetic.count(name) || StartsWith(name, "Resolution Patch ") || name.find("Light Grid") != std::string::npos;
}

template <typename T> void Pack(Sha256Stream& hash, T value) {
    std::uint8_t bytes[sizeof value];
    std::memcpy(bytes, &value, sizeof value);
    hash.Update(bytes, sizeof bytes);
}

fs::path UserDir() {
    if (const auto user = Env("BB_USER_DIR"); user && !user->empty()) return Path(*user);
    return Path(Env("BB_DATA_DIR", ".")) / "user";
}

// <user>\party_relaunch.bat (party_flow.cpp RelaunchBat): only `set "NAME=value"` lines for the
// party's own variables are accepted, and nothing is executed.
bool ApplyRelaunch(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return false;
    std::string text = ReadText(file);
    if (StartsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
    static const std::regex assignment(R"re(set "([A-Z][A-Z0-9_]*)=([A-Za-z0-9_.:@-]*)")re", std::regex::icase);
    std::vector<std::pair<std::string, std::string>> updates;
    for (const std::string& raw : Split(text, '\n')) {
        const std::string line = Trim(raw);
        const std::string lower = Lower(line);
        if (line.empty() || StartsWith(lower, "rem ") || StartsWith(lower, "@rem ") || StartsWith(lower, "@echo ")) continue;
        std::smatch match;
        if (!std::regex_match(line, match, assignment))
            throw Error("Invalid party relaunch settings; no commands were executed");
        const std::string key = match[1], value = match[2];
        if (!StartsWith(key, "BB_PARTY") && !StartsWith(key, "BB_SKIP_NETWORK") && !StartsWith(key, "BB_SAVE_") &&
            !StartsWith(key, "BB_AUTO_CONTINUE"))
            throw Error("Unexpected party relaunch setting: " + key);
        updates.emplace_back(key, value);
    }
    for (const auto& [key, value] : updates) {
        if (value.empty()) UnsetEnv(key.c_str());
        else SetEnv(key.c_str(), value);
    }
    fs::remove(file, error);
    return true;
}

} // namespace

std::string PartyMark(const XmlElement* meta) { return meta ? Lower(Trim(meta->Get("Party", ""))) : std::string(); }

bool PartyEnabled() {
    const auto value = Env("BB_PARTY");
    return value && bb_party_value_on(value->c_str());
}

bool PartySwitchOn(const char* name) {
    const auto value = Env(name);
    return !value || Trim(*value).empty() || bb_party_value_on(value->c_str());
}

std::vector<std::string> PartyPatchNames() {
    std::vector<std::string> names;
    if (!PartyEnabled()) return names;
    if (Trim(Env("BB_PARTY_SEAMLESS", "1")) != "0") {
        names.insert(names.end(), kSeamless.begin(), kSeamless.end());
        if (Trim(Env("BB_PARTY_BELL_NO_INSIGHT", "1")) != "0") names.push_back(kNoInsight);
    }
    const bool fast_join = PartySwitchOn("BB_PARTY_FAST_JOIN");
    if (fast_join) names.insert(names.end(), kFastJoin.begin(), kFastJoin.end());
    if (fast_join && PartySwitchOn("BB_PARTY_FAST_SIGN")) names.push_back(kFastSign);
    if (PartySwitchOn("BB_PARTY_FAST_MESSAGES")) names.insert(names.end(), kFastMessages.begin(), kFastMessages.end());
    return names;
}

void WritePartyPatchFile(const fs::path& out, const std::vector<PartyPatch>& patches) {
    std::vector<const PartyPatch*> gameplay;
    std::vector<std::string> cosmetic;
    for (const PartyPatch& patch : patches) {
        if (PatchIsCosmetic(patch.name, patch.mark)) cosmetic.push_back(patch.name);
        else gameplay.push_back(&patch);
    }
    // Ordered as Python sorts (name, writes) tuples, so the hash does not depend on load order.
    std::sort(gameplay.begin(), gameplay.end(), [](const PartyPatch* a, const PartyPatch* b) {
        return a->name != b->name ? a->name < b->name : a->writes < b->writes;
    });
    std::sort(cosmetic.begin(), cosmetic.end());
    Sha256Stream hash;
    hash.Update("bbparty-patches-1");
    for (const PartyPatch* patch : gameplay) {
        hash.Update(std::string_view("\0patch\0", 7));
        hash.Update(patch->name);
        hash.Update(std::string_view("\0", 1));
        for (const auto& [offset, data] : patch->writes) {
            Pack<std::uint64_t>(hash, offset);
            Pack<std::uint64_t>(hash, data.size());
            hash.Update(data.data(), data.size());
        }
    }
    std::string text = "# Party version check (bb-start): the gameplay patches of patches.bin. Players\n"
                       "# with another hash are refused by the party host; cosmetic patches do not count.\n"
                       "hash " + hash.HexDigest() + "\n";
    for (const PartyPatch* patch : gameplay) text += "patch " + patch->name + "\n";
    for (const std::string& name : cosmetic) text += "# cosmetic " + name + "\n";
    WriteText(out / "party_patch_hash.txt", text);
}

int RunPartyLoop(const std::function<int()>& start) {
    SetEnv("BB_RELAUNCH_LOOP", "1");
    int attempts = 0;
    auto window = std::chrono::steady_clock::now();
    for (;;) {
        const int status = start();
        // The game has exited (Main waits for it): its pool goes, the next start makes a new one.
        PoolRelease();
        if (status == 0 && PartyEnabled()) {
            std::error_code error;
            fs::remove(UserDir() / "party_state.json", error);
        }
        if (status == 75) {
            // A role change from the game's menu, or "Apply and restart": prepare and start again.
            try {
                ApplyRelaunch(UserDir() / "party_relaunch.bat");
            } catch (const Error& e) {
                Log("Party: %s", e.what());
                return 1;
            }
            UnsetEnv("BB_PARTY_RESTARTED");
            continue;
        }
        if (status == 0 || !PartyEnabled() || !PartySwitchOn("BB_PARTY_RESTART")) return status;
        if (status == 76) {
            // Windows refused the game memory for BB_POOL_COMMIT_WAIT seconds (win32_memory.c): a
            // restart now is refused the same way.
            Log("Party: no restart: Windows had no memory left for the game (exit 76)");
            return status;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - window > std::chrono::minutes(5)) {
            attempts = 0;
            window = now;
        }
        ++attempts;
        const int limit = std::max(0, std::atoi(Env("BB_PARTY_RESTART_MAX", "3").c_str()));
        if (attempts > limit) {
            Log("Party: stopped after %d crash restarts in five minutes", limit);
            return status;
        }
        Log("Party: game exited with %d; restart %d/%d to rejoin", status, attempts, limit);
        SetEnv("BB_PARTY_RESTARTED", "1");
        SetEnv("BB_PARTY_AUTOCONTINUE", "1");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace start
