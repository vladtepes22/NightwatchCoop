#include <cassert>
#include <cstdio>
#include <string>
#include "party/party_split_notice.h"
using namespace coop::splitnotice;
int main() {
    assert(WentOn("Hunter1", 0x1b000000u) ==
           "Hunter1 went on to the Forbidden Woods. You'll see them again while in the same area.");
    assert(WentOn("", 0x63000000u) == "A party member went on to another area. You'll see them again while in the same area.");
    assert(Apart("Hunter0", 0x18000000u, true) ==
           "Hunter0 stayed behind in the Cathedral Ward. You'll see them again while in the same area.");
    assert(Apart("Hunter1", 0x1b000000u, false) == WentOn("Hunter1", 0x1b000000u));
    assert(HereAgain("Hunter1") == "Hunter1 is here again.");
    assert(std::string(AreaNameOf(0x18000000u)) == "the Cathedral Ward");
    double last = 0;
    assert(MayNotify(&last, 100.0));
    assert(!MayNotify(&last, 105.0));
    assert(!MayNotify(&last, 109.9));
    assert(MayNotify(&last, 110.0));
    std::puts("party_split_notice ok");
    return 0;
}
