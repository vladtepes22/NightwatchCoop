// SPDX-License-Identifier: GPL-3.0-or-later
// Splitscreen mode (gpu/shim/party/party_splitscreen_rules.h): the messages setting, the error mapping, the HUD
// and name tag rules.
// Build and run: cmake --build out/gpu --target party-splitscreen-test && out/gpu/party-splitscreen-test.exe
#include "party/party_splitscreen_rules.h"

#include <cassert>
#include <cstdio>

using namespace coop::splitscreen;

static void Parse() {
    assert(ParseMessages("all") == Messages::All);
    assert(ParseMessages("") == Messages::All);
    assert(ParseMessages("nonsense") == Messages::All);
    assert(ParseMessages("Errors") == Messages::Errors);
    assert(ParseMessages("errors only") == Messages::Errors);
    assert(ParseMessages(" 1 ") == Messages::Errors);
    assert(ParseMessages("off") == Messages::Off);
    assert(ParseMessages("2") == Messages::Off);
    assert(ClampMessages(-4) == Messages::All && ClampMessages(9) == Messages::Off);
    assert(ParseMessages(MessagesName(Messages::Errors)) == Messages::Errors);
    assert(ParseMessages(MessagesName(Messages::Off)) == Messages::Off);
    assert(ParseMessages(MessagesName(Messages::All)) == Messages::All);
}

static void EffectiveMode() {
    assert(Effective(false, Messages::All) == Messages::All);
    assert(Effective(false, Messages::Errors) == Messages::Errors);
    assert(Effective(false, Messages::Off) == Messages::Off);
    // Splitscreen forces Errors only unless the player picked Off (or already Errors).
    assert(Effective(true, Messages::All) == Messages::Errors);
    assert(Effective(true, Messages::Errors) == Messages::Errors);
    assert(Effective(true, Messages::Off) == Messages::Off);
}

static void Mapping() {
    // Errors: the connection or the join failed, the game fell out of step.
    assert(IsErrorNotice("Connection to the host lost"));
    assert(IsErrorNotice("Connection to Hunter unstable"));
    assert(IsErrorNotice("Party error: bind failed"));
    assert(IsErrorNotice("UDP to the host is blocked: no answer"));
    assert(IsErrorNotice("Hunter can't reach you (UDP blocked)"));
    assert(IsErrorNotice("The host did not come back"));
    assert(IsErrorNotice("Waiting for the host to come back (12 s)..."));
    assert(IsErrorNotice("Summon stuck: session restarted"));
    assert(IsErrorNotice("host refused: different game"));
    assert(IsErrorNotice("wrong password or party code"));
    assert(IsErrorNotice("the party is full"));
    assert(IsErrorNotice("kicked by the host"));
    assert(IsErrorNotice("Gathering: could not reach the host"));
    assert(IsErrorNotice("Desync detected in lamps"));
    // Information.
    assert(!IsErrorNotice("Hunter joined"));
    assert(!IsErrorNotice("Hunter rejoined"));
    assert(!IsErrorNotice("Hunter left"));
    assert(!IsErrorNotice("Joined the party"));
    assert(!IsErrorNotice("Party open"));
    assert(!IsErrorNotice("Gathering: travelling to the host"));
    assert(!IsErrorNotice("Gathered: beside the host"));
    assert(!IsErrorNotice("You voted yes: waiting for the others"));
    assert(!IsErrorNotice("Boss fight started by a party member"));
    assert(!IsErrorNotice("Key Item: shared with the party"));
    assert(!IsErrorNotice("You left the party"));
    assert(!IsErrorNotice(""));
}

static void Shown() {
    // All shows everything; Off nothing, not even an alert; Errors: alerts and failing notices.
    assert(MessageShown(Messages::All, Kind::Notice, "Hunter joined"));
    assert(MessageShown(Messages::All, Kind::Alert, "x"));
    assert(!MessageShown(Messages::Off, Kind::Notice, "Connection to the host lost"));
    assert(!MessageShown(Messages::Off, Kind::Alert, "x"));
    assert(!MessageShown(Messages::Errors, Kind::Notice, "Hunter joined"));
    assert(MessageShown(Messages::Errors, Kind::Notice, "Connection to the host lost"));
    assert(MessageShown(Messages::Errors, Kind::Alert, "The party is full"));
    // The chain: splitscreen on with the default All.
    const Messages eff = Effective(true, Messages::All);
    assert(!MessageShown(eff, Kind::Notice, "Hunter left"));
    assert(MessageShown(eff, Kind::Notice, "Connection to the host lost"));
}

static void Hud() {
    assert(!CompassOnly(false) && CompassOnly(true));
    assert(NameTagsShown(false, true, 3));
    assert(!NameTagsShown(false, false, 3));
    assert(!NameTagsShown(false, true, 0));
    assert(!NameTagsShown(true, true, 3));
}

int main() {
    Parse();
    EffectiveMode();
    Mapping();
    Shown();
    Hud();
    std::printf("party-splitscreen-test: ok\n");
    return 0;
}
