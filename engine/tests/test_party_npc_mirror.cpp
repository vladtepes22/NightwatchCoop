// SPDX-License-Identifier: GPL-3.0-or-later
// NPC presence mirror (gpu/shim/party/party_npc_mirror.h): the guest's plan and the event body.
#include "party/party_npc_mirror.h"

#include <cassert>
#include <cstdio>
#include <iterator>

using namespace coop::npcmirror;

int main() {
    // The clinic (misc071 prorun1): the host has NPC 2410700 off, the guest shows it.
    std::vector<LocalNpc> local = {{2410700, true, false}, {2410710, true, false}, {2410950, false, false}};
    Plan p = PlanMirror({2410700, 2410950}, local);
    assert(p.disable.size() == 1 && p.disable[0] == 2410700 && p.enable.empty());
    // Turned off by us; the host turns it on again: on again. One the guest had off by itself stays.
    local = {{2410700, false, true}, {2410950, false, false}};
    p = PlanMirror({2410950}, local);
    assert(p.enable.size() == 1 && p.enable[0] == 2410700 && p.disable.empty());
    // Still off on the host: nothing to do; a chr the host does not name is never touched.
    p = PlanMirror({2410700}, local);
    assert(p.enable.empty() && p.disable.empty());
    p = PlanMirror({}, {{2410710, true, false}, {2410720, false, false}});
    assert(p.enable.empty() && p.disable.empty());

    // 0.7.2 (lamprule): only NPCs. The host's world keeps an unlit lamp's talk chr off; the guest's
    // must stay on (its talk lights the lamp). Lamps' and headstones' talk chrs, Dream messengers
    // and dummies, the lever messenger are never turned off; the NPCs still are.
    for (std::uint32_t npc : {2410700u, 2100600u, 2100700u, 2400700u, 2300710u, 3200100u, 3500930u})
        assert(Mirrored(npc));
    for (std::uint32_t obj : {2410950u, 2410951u, 2410952u, 2410953u, 2500951u, 2100950u, 2100960u, 2100200u,
                              2100211u, 2100231u, 2400650u, 2100999u, 2410870u, 2400660u - 1u})
        assert(!Mirrored(obj));
    local = {{2410951, true, false}, {2410700, true, false}, {2100211, true, false}};
    p = PlanMirror({2410951, 2410700, 2100211}, local);
    assert(p.disable.size() == 1 && p.disable[0] == 2410700 && p.enable.empty());
    // One turned off by an older build of this mirror: still turned on again when the host has it on.
    p = PlanMirror({}, {{2410951, false, true}});
    assert(p.enable.size() == 1 && p.enable[0] == 2410951);
    assert(std::is_sorted(std::begin(kNpcMirrorIds), std::end(kNpcMirrorIds)) && std::size(kNpcMirrorIds) > 100);

    // The event body.
    std::uint32_t map = 0;
    std::vector<std::uint32_t> ids;
    const std::string j = ToJson(0x18010000u, {2410700, 2410950});
    assert(FromJson(j, &map, &ids) && map == 0x18010000u && ids.size() == 2 && ids[1] == 2410950);
    assert(FromJson(ToJson(5, {}), &map, &ids) && map == 5 && ids.empty());
    assert(!FromJson("{\"map\":1,\"off\":[12]}", &map, &ids));          // not an entity id
    assert(!FromJson("{\"map\":1,\"off\":[2410700,x]}", &map, &ids));
    assert(!FromJson("{\"map\":1,\"off\":[2410700]} ", &map, &ids));
    assert(!FromJson("{\"off\":[2410700],\"map\":1}", &map, &ids));
    std::vector<std::uint32_t> many(kMaxIds + 5, 2410700);
    assert(FromJson(ToJson(1, many), &map, &ids) && ids.size() == kMaxIds);  // capped when written
    std::printf("party npc mirror: ok\n");
    return 0;
}
