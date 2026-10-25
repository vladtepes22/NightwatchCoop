// SPDX-License-Identifier: GPL-3.0-or-later
// Unit test of the enemy AI ownership rules. ninja -C out/gpu party-enemyai-rules-test
#include "party/party_enemyai_rules.h"

#include <cstdio>
#include <cstdlib>

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);             \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main() {
    using namespace enemyai_rules;
    View v;
    v.client = true;
    v.world_up = true;
    v.host_apart = true;
    v.members_apart = 1;
    // a lone guest, the host in another area: the guest's game runs the AI
    CHECK(GuestRunsAi(v));
    // the host is in the same area: its game owns the AI
    View same = v;
    same.host_apart = false;
    same.members_apart = 0;
    CHECK(!GuestRunsAi(same));
    // another guest apart from us does not make the host's world ours
    View other = v;
    other.host_apart = false;
    other.members_apart = 1;
    CHECK(!GuestRunsAi(other));
    // a host never takes over (its game is the owner of its world)
    View host = v;
    host.client = false;
    CHECK(!GuestRunsAi(host));
    // loading screen / no world: nothing to switch
    View load = v;
    load.world_up = false;
    CHECK(!GuestRunsAi(load));
    // switched off (BB_PARTY_LONE_AI=0)
    View off = v;
    off.enabled = false;
    CHECK(!GuestRunsAi(off));

    // take over once, keep, hand back when the host drives again
    CHECK(Next(false, true) == Step::Take);
    CHECK(Next(true, true) == Step::Keep);
    CHECK(Next(true, false) == Step::HandBack);
    CHECK(Next(false, false) == Step::Keep);
    CHECK(!OwningAfterLoad(true));

    // limbo target: the nearest living member
    const float d[] = {-1.0f, 9.0f, 4.0f, -1.0f};
    CHECK(PickLimboTarget(d, 4) == 2);
    const float dead[] = {-1.0f, -1.0f};
    CHECK(PickLimboTarget(dead, 2) == -1);
    CHECK(PickLimboTarget(nullptr, 0) == -1);
    const float tie[] = {5.0f, 5.0f};
    CHECK(PickLimboTarget(tie, 2) == 0);
    const float near_zero[] = {0.0f, 3.0f};
    CHECK(PickLimboTarget(near_zero, 2) == 0);

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("party-enemyai-rules-test: ok\n");
    return 0;
}
