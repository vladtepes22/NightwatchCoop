// SPDX-License-Identifier: GPL-3.0-or-later
// Party logging, the game side (party_log.h, docs/party/logging.md): the periodic state snapshot,
// the area events polled once a frame (loads, deaths, dialogs, prompts, item-get box, link state),
// the link event trace both ways, run-id exchange and user marks over the party link.
#pragma once

#include <cstdint>
#include <string>

namespace coop::plog {

/// The link event the log uses itself: {"k":"hello"|"mark", ...}.
constexpr const char* kLogEventName = "plog";

/// PartyInit: the line tagger (verbose), the mark relay.
void GameInit();
/// Main thread, once a frame (CoopTick). Cheap at Normal (one link poll per second).
void GameTick();
/// A hint why the next load happens ("lamp warp 2412951", "host travel event"); any thread.
void NoteLoadReason(const std::string& why);
/// The party link's callbacks (party_runtime.cpp): any thread. OnEventReceived returns true for
/// the log's own event (consumed).
void OnEventSent(int to_slot, const std::string& name, const std::string& body);
bool OnEventReceived(int from_slot, const std::string& name, const std::string& body);

}  // namespace coop::plog
