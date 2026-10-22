// SPDX-License-Identifier: GPL-3.0-or-later
// Guest services (gpu/shim/party/party_guestsvc.h): the gate and the record change text.
// ninja -C out/gpu party-guestsvc-test && out/gpu/party-guestsvc-test
#include "party/party_guestsvc.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace coop;

static GuestServicesInput Guest(std::uint32_t map) {
    GuestServicesInput in;
    in.enabled = true;
    in.world_up = true;
    in.loading = false;
    in.session_role = kSessionRoleClient;
    in.map_id = map;
    return in;
}

int main() {
    // The lower bath messengers' animation event: per player (its 0x31 sync dropped), nothing else.
    assert(PerPlayerMessengerEvent(12105040u));
    // 0.7.3: the gift messengers' events (starting weapons, Notebook, bells, Eye) are per player too.
    for (std::uint32_t e : {12101020u, 12101021u, 12101022u, 12101024u, 12101026u, 12101028u}) assert(PerPlayerMessengerEvent(e));
    assert(!PerPlayerMessengerEvent(12101027u)); // the Old Hunter Bell talk flag, not an event
    assert(!PerPlayerMessengerEvent(12101800u)); // a boss event
    assert(!PerPlayerMessengerEvent(12105043u)); // Insight messenger: the host's copy drives it (ends on a client)
    assert(!PerPlayerMessengerEvent(12105041u)); // the talk flag, not an event
    assert(!PerPlayerMessengerEvent(12105064u)); // dress-up messenger: ends in multiplayer itself
    assert(!PerPlayerMessengerEvent(12105070u)); // dress-up flag control: world state, stays synced
    assert(!PerPlayerMessengerEvent(12101010u)); // lineup time-of-day flags
    assert(!PerPlayerMessengerEvent(12400750u)); // a door event (synced by design)

    // Dream maps.
    assert(IsDreamMap(0x15000000u)); // m21_00_00_00
    assert(IsDreamMap(0x15010000u)); // m21_01_00_00
    assert(!IsDreamMap(0x16000000u)); // m22
    assert(!IsDreamMap(0x18010000u)); // m24_01
    assert(!IsDreamMap(0xffffffffu));
    assert(!IsDreamMap(0));

    // The gate: a client in the Dream only.
    assert(GuestServicesGate(Guest(0x15000000u)));
    assert(GuestServicesGate(Guest(0x15010000u)));
    assert(!GuestServicesGate(Guest(0x18000000u))); // host's world outside the Dream: vanilla
    {
        GuestServicesInput in = Guest(0x15000000u);
        in.enabled = false;
        assert(!GuestServicesGate(in));
        in = Guest(0x15000000u);
        in.session_role = 3; // host
        assert(!GuestServicesGate(in));
        in.session_role = 0; // solo / own world
        assert(!GuestServicesGate(in));
        in.session_role = 7; // leaving
        assert(!GuestServicesGate(in));
        in = Guest(0x15000000u);
        in.loading = true;
        assert(!GuestServicesGate(in));
        in = Guest(0x15000000u);
        in.world_up = false;
        assert(!GuestServicesGate(in));
    }

    // Talk scripts.
    assert(GuestServiceTalk(210304));  // Doll
    assert(GuestServiceTalk(210692));  // workbench
    assert(GuestServiceTalk(210698));  // Insight messenger
    assert(!GuestServiceTalk(210305)); // Gehrman (last choice)
    assert(!GuestServiceTalk(210000)); // headstone
    assert(!GuestServiceTalk(210721)); // Dream warp menu
    assert(!GuestServiceTalk(210100)); // chalice altar
    assert(!GuestServiceTalk(0));
    assert(GuestServiceTalkAllowed(210304, ""));
    assert(!GuestServiceTalkAllowed(210305, ""));
    // Gehrman t210305: his last event (ending choice) is up with 1028, or 1029 without 1030.
    assert(!GehrmanLastEvent(false, false, false));
    assert(!GehrmanLastEvent(false, false, true));
    assert(!GehrmanLastEvent(false, true, true));
    assert(GehrmanLastEvent(false, true, false));
    assert(GehrmanLastEvent(true, false, false));
    assert(GehrmanLastEvent(true, true, true));
    assert(kGehrmanTalk == 210305);
    assert(GuestServiceTalkAllowed(210305, "all"));
    assert(GuestServiceTalkAllowed(210305, "123,210305"));
    assert(!GuestServiceTalkAllowed(210305, "123,2103050"));
    assert(!GuestServiceTalkAllowed(210305, "x,,210305x"));

    // Headstones (lamp part).
    assert(HeadstoneTalk(210000) && HeadstoneTalk(210003) && HeadstoneTalk(210721));
    assert(!HeadstoneTalk(210004) && !HeadstoneTalk(210720) && !HeadstoneTalk(210304));
    assert(LampTalk(241000) && LampTalk(241003) && LampTalk(210001) && LampTalk(361002) && LampTalk(242000));
    assert(LampLitFlagAllowed(12417830) && LampLitFlagAllowed(12417831) && LampLitFlagAllowed(72410201));
    assert(!LampLitFlagAllowed(12417820) && !LampLitFlagAllowed(12417832) && !LampLitFlagAllowed(12416830) &&
           !LampLitFlagAllowed(72410101) && !LampLitFlagAllowed(9401) && !LampLitFlagAllowed(12411800) &&
           !LampLitFlagAllowed(14017830));
    {
        std::vector<std::uint32_t> ids;
        std::string err;
        assert(LampLitFromJsonText(LampLitToJsonText({12417830, 72410201}), &ids, &err) && ids.size() == 2 &&
               ids[0] == 12417830 && ids[1] == 72410201);
        assert(!LampLitFromJsonText(LampLitToJsonText({12411800}), &ids, &err));
        assert(!LampLitFromJsonText("{\"ids\":[1,2", &ids, &err));
        std::vector<std::uint32_t> many(17, 72410201);
        assert(!LampLitFromJsonText(LampLitToJsonText(many), &ids, &err));
        // Statsaudit finding 5: a guest's own-save lamps carry "own" (the host's shared-lamps rule).
        bool own = true;
        assert(LampLitFromJsonText(LampLitToJsonText({12417830}), &ids, &err, &own) && !own);
        assert(LampLitFromJsonText(LampLitToJsonText({12417830}, true), &ids, &err, &own) && own && ids.size() == 1);
        assert(!LampLitFromJsonText(LampLitToJsonText({12411800}, true), &ids, &err, &own));
    }
    assert(!LampTalk(241004) && !LampTalk(241190) && !LampTalk(210304) && !LampTalk(243000) && !LampTalk(100000));
    // The Chalice Dungeons' lamp scripts (t290110-t290119; the tester's t290114) are not world lamps
    // (LampTalk) and not the dungeon's other talks (t290000, t290300).
    assert(ChaliceLampTalk(290110) && ChaliceLampTalk(290114) && ChaliceLampTalk(290119));
    assert(!ChaliceLampTalk(290109) && !ChaliceLampTalk(290120) && !ChaliceLampTalk(290000) && !ChaliceLampTalk(290300) &&
           !ChaliceLampTalk(241000));
    assert(!LampTalk(290114) && !LampTalk(290110));

    // Lamp requests.
    {
        LampRequest r;
        r.id = 2102950; // Dream from the Great Bridge (ReturnPointParam)
        r.seq = 7;
        LampRequest back;
        std::string err;
        assert(LampRequestFromJsonText(LampRequestToJsonText(r), &back, &err));
        assert(back.id == r.id && back.seq == 7 && !back.rest);
        assert(LampRequestToJsonText(r).find("rest") == std::string::npos);  // old hosts see the same text
        // Playtest 1: a guest's lamp in the loaded map never moves the host (bug 10); a guest's
        // alone rest asks for a rest load (bug 3).
        assert(ChooseLampRequestAction(back, false) == LampRequestAction::PartyTravel);
        assert(ChooseLampRequestAction(back, true) == LampRequestAction::IgnoreSameMap);
        {
            LampRequest rest = r;
            rest.rest = true;
            LampRequest rb;
            assert(LampRequestFromJsonText(LampRequestToJsonText(rest), &rb, &err) && rb.rest && rb.id == r.id);
            assert(ChooseLampRequestAction(rb, false) == LampRequestAction::RestLoad);
            assert(ChooseLampRequestAction(rb, true) == LampRequestAction::RestLoad);
            std::string t = LampRequestToJsonText(rest);
            t.replace(t.find("true"), 4, "1");  // only a JSON true is a rest
            assert(LampRequestFromJsonText(t, &rb, &err) && !rb.rest);
        }
        assert(SanitizePeerLampRequest(back, &err));
        back.id = 12345;
        assert(!SanitizePeerLampRequest(back, &err) && !err.empty());
        assert(!LampRequestFromJsonText("[]", &back, &err));
        assert(!LampRequestFromJsonText("{\"seq\":\"1\"}", &back, &err));
        assert(!LampRequestFromJsonText("not json", &back, &err));
    }
    {
        LampRequestGate gate;
        std::string why;
        assert(!gate.Accept(false, 100.0, &why));
        assert(gate.Accept(true, 100.0, &why));
        assert(!gate.Accept(true, 105.0, &why));
        assert(gate.Accept(true, 111.0, &why));
    }
    {
        const std::vector<EmevdSkipRule> rules = LampSkipRules();
        assert(rules.size() == 2);
        assert(EmevdSkipMatches(rules[0], 7200, 1003, 6, 0));
        assert(EmevdSkipMatches(rules[1], 12107000, 1003, 6, 0));
        assert(!EmevdSkipMatches(rules[1], 12107000, 2003, 49, 4));
        assert(!EmevdSkipMatches(rules[0], 7200, 1003, 6, 1));
        assert(!EmevdSkipMatches(rules[0], 7300, 1003, 6, 0));
    }
    {
        // chalicelamp: a guest's dungeon lamp "Return to the Hunter's Dream" (m29 12907020's client end).
        const std::vector<EmevdSkipRule> rules = ChaliceLampSkipRules();
        assert(rules.size() == 1);
        assert(EmevdSkipMatches(rules[0], 12907020, 1003, 6, 0));
        assert(!EmevdSkipMatches(rules[0], 12907020, 2003, 49, 8));   // the warp itself runs
        assert(!EmevdSkipMatches(rules[0], 12907020, 1003, 1, 7));    // the slot tests stay
        assert(!EmevdSkipMatches(rules[0], 12907010, 1003, 6, 0));    // the lamp's lighting event: as is
        assert(!rules[0].client_only && !rules[0].needs_travel);      // the host never ends there anyway
    }

    // Doll placement: client-only skips of the re-roll in 12100110.
    {
        const std::vector<EmevdSkipRule> rules = DollPlacementRules();
        assert(rules.size() == 2);
        for (const EmevdSkipRule& r : rules) {
            assert(r.client_only && !r.needs_travel && r.event == 12100110);
        }
        assert(EmevdSkipMatches(rules[0], 12100110, 2003, 22, 25));
        assert(EmevdSkipMatches(rules[1], 12100110, 2003, 17, 26));
        assert(!EmevdSkipMatches(rules[0], 12100110, 2003, 22, 28)); // the 12105034 clear stays
        assert(!EmevdSkipMatches(rules[1], 12100111, 2003, 17, 10)); // auto-talk roll stays
        assert(DescribeSkipRule(rules[0]).find("on a client") != std::string::npos);
    }

    // The Doll's level-up kneel event: only its player chr type tests.
    assert(DollLevelUpCharTypeTest(12100112, 4, 3, 10000, 0));
    assert(DollLevelUpCharTypeTest(12100112, 4, 3, 10000, 1));
    assert(!DollLevelUpCharTypeTest(12100112, 4, 3, 2100700, 0));
    assert(!DollLevelUpCharTypeTest(12100112, 3, 3, 10000, 0));
    assert(!DollLevelUpCharTypeTest(12100110, 4, 3, 10000, 0));
    assert(DollLevelUpCharTypeTest(12100113, 4, 3, 10000, 0)); // the Doll's gesture: 72100102
    assert(DollLevelUpCharTypeTest(12100114, 4, 3, 10000, 0)); // level-up SFX
    assert(!DollLevelUpCharTypeTest(12100115, 4, 3, 10000, 0));
    assert(!DollLevelUpCharTypeTest(12100112, 4, 3, 10000, 8));

    // Env.
    assert(GuestServicesEnabledFromEnv(nullptr));
    assert(GuestServicesEnabledFromEnv(""));
    assert(GuestServicesEnabledFromEnv("1"));
    assert(!GuestServicesEnabledFromEnv("0"));
    assert(GuestServicesEnabledFromEnv("00"));
    assert(GuestLampsEnabledFromEnv(nullptr) && GuestLampsEnabledFromEnv("") && !GuestLampsEnabledFromEnv("0"));
    assert(GuestLampsEnabledFromEnv("1") && GuestLampsEnabledFromEnv("1", false));
    assert(!GuestLampsEnabledFromEnv(nullptr, false) && !GuestLampsEnabledFromEnv("x", false));

    // Record changes.
    PlayerRecord a{10, 5000, 3}, b{11, 3200, 3};
    assert(DescribeRecordChange(a, b) == "level 10 -> 11, echoes 5000 -> 3200");
    assert(DescribeRecordChange(a, a).empty());
    PlayerRecord c{10, 5000, 1};
    assert(DescribeRecordChange(a, c) == "Insight 3 -> 1");
    PlayerRecord unknown;
    assert(DescribeRecordChange(unknown, a).empty());
    assert(a != b && a == a);

    std::puts("party-guestsvc-test: ok");
    return 0;
}
