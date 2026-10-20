// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op at the start (was scripts/patches.py, mods.py, online.py and party_launch.py in the
// seamless fork): the party patches, the identity files the party host compares (the gameplay
// patches and mods), and the restart loop for role changes and crash recovery.
#pragma once
#include "common.h"

#include <functional>

namespace start {

struct XmlElement;

/// BB_PARTY turns party mode on (gpu/party_env.h: unset, empty, 0, off, false, no = off).
bool PartyEnabled();
/// A party switch that is on by default (BB_PARTY_FAST_JOIN, ...): unset or blank = on.
bool PartySwitchOn(const char* name);

extern const char* const kDisableHttp;
extern const char* const kSkipNetworkChoice;
extern const char* const kSkipNetworkChoiceOnline;

/// The seamless and join-timing patches for this party session (none without BB_PARTY).
std::vector<std::string> PartyPatchNames();

/// One applied patch: its name, its Metadata's Party="cosmetic|gameplay" mark (empty: none) and
/// its writes. Copied, not pointed to: external patch files are gone by the time the file is written.
struct PartyPatch {
    std::string name;
    std::string mark;
    std::vector<std::pair<std::uint64_t, Bytes>> writes;
};
/// The Party mark of a Metadata (nullptr: none).
std::string PartyMark(const XmlElement* meta);
/// out/party_patch_hash.txt: the hash and names of the patches that change gameplay (graphics,
/// frame rate, resolution, sound workarounds ... do not count), read by party_runtime.cpp.
void WritePartyPatchFile(const fs::path& out, const std::vector<PartyPatch>& patches);

/// Keeps bb-start attached across the party flow's role changes (exit code 75 with
/// <user>\party_relaunch.bat) and restarts a crashed party game (BB_PARTY_RESTART, at most
/// BB_PARTY_RESTART_MAX in five minutes). `start` prepares and runs the game once.
int RunPartyLoop(const std::function<int()>& start);

} // namespace start
