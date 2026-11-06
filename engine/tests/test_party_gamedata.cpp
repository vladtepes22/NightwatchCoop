// SPDX-License-Identifier: GPL-3.0-or-later
// party_gamedata.h: the cache parser, with made-up numbers (no game data). The tables built from a real game
// are exercised by the other party tests (tests/party_tables_test.h).
#include "party/party_gamedata.h"
#include "party/party_ids.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main() {
    using namespace coop::gamedata;
    const std::string good =
        "BBPARTYTABLES 1\r\nkey x\r\n[maps] 2\r\n20\r\n10\r\n[returnPoints] 1\r\n7\r\n"
        "[returnSpots] 1\r\n7\t1.5000\t-2.0000\t3.2500\t0.500000\r\n"
        "[npcFog] 1\r\n11\t12\t13\t14\t15\t-1.25\t2.50\t3.75\t45.0\r\n"
        "[itemLotFlag] 2\r\n9\t90\r\n5\t50\r\n[itemLedger] 1\r\n3\t5\t77\tnote text\r\n"
        "[bossEntities] 3\r\n9\t0\r\n8\t1\r\n7\t0\r\n"
        "[bossGates] 1\r\n1\t2\t3\t4\t5\t6\t7\r\n[gateRules] 2\r\n10\t20\t0\r\n11\t21\t12\r\n"
        "[shopGates] 2\r\n5501\t23020000\r\n5500\t23010000\r\n"
        "[startWeapons] 2\r\n72101000\t25500000\r\n72101011\t14500000\r\n"
        "[bossArenas] 2\r\n2300810\t134.63\t-120.12\t-85.41\r\n2200800\t-329.75\t1.60\t699.30\r\n"
        "[firstFightSpots] 2\r\n12301702\t113.50\t-120.31\t-63.68\t0\t0\r\n12401802\t57.70\t35.21\t322.60\t1\t1\r\n"
        "[stateIds] 2\r\n12400151\r\n12400150\r\n[runtimeIds] 1\r\n12100500\r\n"
        "[itemUnshared] 2\r\n8\r\n2\r\n[itemKillAward] 1\r\n1\t2\t3\t4\t5\r\n[futureSection] 1\r\nwhatever\r\n";
    Tables t;
    std::string err;
    CHECK(Parse(good, &t, &err) && t.loaded);
    CHECK(t.maps.size() == 2 && t.maps[0] == 10 && t.maps[1] == 20);  // sorted
    CHECK(t.return_spot_ids.size() == 1 && t.return_spots.size() == 4 && t.return_spots[2] == 3.25f);
    CHECK(t.npc_fog.size() == 1 && t.npc_fog[0].host_flag == 14 && t.npc_fog[0].x == -1.25f && t.npc_fog[0].yaw == 45.0f);
    CHECK(t.lot_flags.size() == 2 && t.lot_flags[0].lot == 5 && t.lot_flags[1].flag == 90);
    CHECK(t.ledger.size() == 1 && t.ledger[0].done_flag == 77 && t.ledger[0].note == "note text");
    CHECK(t.unshared.size() == 2 && t.unshared[0] == 2);
    CHECK(t.kill_awards.size() == 1 && t.kill_awards[0].spot_flag == 5);
    CHECK(t.boss_entities.size() == 2 && t.boss_entities[0] == 7 && t.chalice_boss_entities.size() == 1);
    CHECK(t.boss_gates.size() == 1 && t.boss_gates[0].fog_entity == 6 && t.boss_gates[0].landing_event == 7);
    CHECK(t.shop_gates.size() == 2 && t.shop_gates[0].flag == 5500 && t.shop_gates[0].weapon == 23010000);  // sorted
    CHECK(t.start_weapons.size() == 2 && t.start_weapons[0].flag == 72101000 && t.start_weapons[0].weapon == 25500000 &&
          t.start_weapons[1].weapon == 14500000);
    CHECK(t.boss_arenas.size() == 2 && t.boss_arenas[0].boss == 2200800 && t.boss_arenas[1].z == -85.41f);
    CHECK(t.first_fights.size() == 2 && !t.first_fights[0].scene && t.first_fights[1].chained && t.first_fights[1].scene);
    CHECK(t.state_ids.size() == 2 && t.state_ids[0] == 12400150 && t.runtime_ids.size() == 1);
    CHECK(t.gate_rules.size() == 2 && t.gate_rules[1].gate == 21 && t.gate_rules[1].also == 12);

    // The consumers' view of an installed table.
    Install(t);
    CHECK(coop::ids::ReturnPoints().size() == 1 && coop::ids::InSorted(coop::ids::Maps(), 20));
    float spot[4];
    CHECK(coop::ids::TemplateReturnSpot(7, spot) && spot[0] == 1.5f && !coop::ids::TemplateReturnSpot(8, spot));

    // Broken files: nothing loaded, a reason, no crash.
    for (const char* bad : {"", "nonsense\n", "BBPARTYTABLES 2\n", "BBPARTYTABLES 1\n[maps] 2\n1\n",
                            "BBPARTYTABLES 1\n[maps] 1\nabc\n", "BBPARTYTABLES 1\n[maps] x\n",
                            "BBPARTYTABLES 1\n[itemLotFlag] 1\n1\n", "BBPARTYTABLES 1\nstray\n",
                            "BBPARTYTABLES 1\n[bossGates] 1\n1\t2\t3\n", "BBPARTYTABLES 1\n[gateRules] 1\n1\t2\tx\n",
                            "BBPARTYTABLES 1\n[bossEntities] 1\n5\t2\n", "BBPARTYTABLES 1\n[shopGates] 1\n5500\n",
                            "BBPARTYTABLES 1\n[bossArenas] 1\n1\t2\t3\n", "BBPARTYTABLES 1\n[firstFightSpots] 1\n1\t2\t3\t4\t2\t0\n",
                            "BBPARTYTABLES 1\n[stateIds] 1\nx\n"}) {
        Tables b;
        std::string e;
        CHECK(!Parse(bad, &b, &e) && !b.loaded && !e.empty() && b.maps.empty());
    }
    Tables missing;
    CHECK(!LoadFile("definitely/not/here.txt", &missing, &err) && !missing.loaded);
    Install(Tables{});
    CHECK(!Loaded() && !coop::ids::MapKnown(10) && !coop::ids::ReturnSpot(7, spot));

    std::printf("party-gamedata-test: %d failures\n", g_fail);
    return g_fail ? 1 : 0;
}
