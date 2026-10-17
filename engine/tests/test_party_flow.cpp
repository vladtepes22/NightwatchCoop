// SPDX-License-Identifier: GPL-3.0-or-later
// The party flow's pure parts (gpu/shim/party/party_flow.h): party.json, the action planner and
// the relaunch file. ninja -C out/gpu party-flow-test
#include "party/party_flow.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace party::flow;

static std::string EnvOf(const Plan& p, const std::string& k, bool* found = nullptr) {
    for (const auto& [key, v] : p.env) {
        if (key == k) {
            if (found) *found = true;
            return v;
        }
    }
    if (found) *found = false;
    return {};
}

static void TestJson() {
    Saved s;
    s.name = "Hunter_1";
    s.role = "join";
    s.host_address = "  bbp1-abcd-efgh ";
    s.password = "pa ss \"word\" ü";
    s.port = 47600;
    s.has_character = true;
    s.slot = 3;
    s.host_secret = "0123456789abcdef";
    Saved r;
    std::string err;
    // Protected (DPAPI on Windows; clear text elsewhere): reads back the same.
    assert(FromJson(ToJson(s, true), &r, &err));
    assert(r.name == "Hunter_1" && r.role == "join" && r.port == 47600 && r.has_character && r.slot == 3);
    assert(r.host_address == "BBP1-ABCD-EFGH");
    assert(r.password == s.password);
    assert(r.host_secret == s.host_secret);
#if defined(_WIN32)
    assert(ToJson(s, true).find("pa ss") == std::string::npos);  // not in clear text
    assert(ToJson(s, true).find("password_dpapi") != std::string::npos);
    assert(!r.password_plain);
#endif
    // Clear text (hand-written / not protected): read, flagged plain.
    assert(FromJson(ToJson(s, false), &r, &err));
    assert(r.password == s.password && r.password_plain);
    // Bad values are dropped, not trusted.
    assert(FromJson(R"({"role":"admin","host_secret":"xyz","port":99999,"password_dpapi":"!!!"})", &r, &err));
    assert(r.role.empty() && r.host_secret.empty() && r.port == 65535 && r.password.empty());
    assert(!FromJson("[1,2]", &r, &err));
    assert(!FromJson("{", &r, &err));
    std::puts("PASS: party.json");
}

static Context Title(bool party, bool host) {
    Context c;
    c.party_process = party;
    c.host = host;
    c.party_save = party;
    return c;
}

static void TestPlans() {
    // Solo title: New party game -> relaunch as host, no auto-Continue (New Game follows).
    Context c = Title(false, false);
    Plan p = PlanAction(Action::NewParty, c);
    assert(p.ok() && p.relaunch && EnvOf(p, "BB_PARTY") == "host" && EnvOf(p, "BB_PARTY_AUTOCONTINUE") == "0");
    assert(EnvOf(p, "BB_PARTY_SAVE") == "separate" && EnvOf(p, "BB_PARTY_MENU") == "1");
    assert(EnvOf(p, "BB_SKIP_NETWORK_CHOICE") == "online");
    bool found = false;
    assert(EnvOf(p, "BB_PARTY_PASSWORD", &found).empty() && found);  // unset: party.json has it
    assert(EnvOf(p, "BB_PARTY_RESTARTED", &found).empty() && found);
    assert(p.saved.role == "host");

    // Join without an address: refused.
    p = PlanAction(Action::JoinParty, c);
    assert(!p.ok() && !p.relaunch);
    c.saved.host_address = "127.0.0.1:47600";
    p = PlanAction(Action::JoinParty, c);
    assert(p.ok() && EnvOf(p, "BB_PARTY") == "join" && EnvOf(p, "BB_PARTY_AUTOCONTINUE") == "0");
    c.saved.has_character = true;
    p = PlanAction(Action::JoinParty, c);
    assert(p.ok() && EnvOf(p, "BB_PARTY_AUTOCONTINUE") == "1" && p.saved.role == "join");

    // Continue: needs a party character; keeps the last role.
    Context d = Title(false, false);
    p = PlanAction(Action::ContinueParty, d);
    assert(!p.ok());
    d.saved.has_character = true;
    p = PlanAction(Action::ContinueParty, d);
    assert(p.ok() && EnvOf(p, "BB_PARTY") == "host" && EnvOf(p, "BB_PARTY_AUTOCONTINUE") == "1");
    d.saved.role = "join";
    p = PlanAction(Action::ContinueParty, d);
    assert(!p.ok());  // a guest without an address
    d.saved.host_address = "BBP1-XXXX";
    d.saved.port = 9400;
    d.saved.name = "Lady_Maria";
    p = PlanAction(Action::ContinueParty, d);
    assert(p.ok() && EnvOf(p, "BB_PARTY") == "join" && EnvOf(p, "BB_PARTY_PORT") == "9400");
    assert(EnvOf(p, "BB_PARTY_NAME") == "Lady_Maria");

    // A single-player character loaded: no party action (never sync into a solo save).
    Context w = Title(false, false);
    w.in_world = true;
    w.saved.host_address = "1.2.3.4";
    w.saved.has_character = true;
    for (Action a : {Action::NewParty, Action::JoinParty, Action::ContinueParty, Action::HostParty, Action::LeaveParty}) {
        p = PlanAction(a, w);
        assert(!p.ok() && !p.relaunch);
    }
    assert(!PartyActionsAllowed(w));

    // Party process in the world (party save): switch roles by relaunch, again in place.
    Context h = Title(true, true);
    h.in_world = true;
    h.link_active = true;
    h.saved.host_address = "1.2.3.4:9307";
    assert(PartyActionsAllowed(h));
    p = PlanAction(Action::HostParty, h);
    assert(!p.ok());  // already hosting
    h.link_active = false;
    p = PlanAction(Action::HostParty, h);
    assert(p.ok() && p.local == Plan::Local::Rejoin && !p.relaunch);
    p = PlanAction(Action::JoinParty, h);
    assert(p.ok() && p.relaunch && EnvOf(p, "BB_PARTY") == "join" && EnvOf(p, "BB_PARTY_AUTOCONTINUE") == "1");
    p = PlanAction(Action::NewParty, h);
    assert(!p.ok());  // in the world
    Context g = Title(true, false);
    g.in_world = true;
    g.link_active = true;
    g.saved.host_address = "1.2.3.4:9307";
    p = PlanAction(Action::LeaveParty, g);
    assert(p.ok() && p.local == Plan::Local::Leave);
    p = PlanAction(Action::HostParty, g);
    assert(p.ok() && p.relaunch && EnvOf(p, "BB_PARTY") == "host" && EnvOf(p, "BB_PARTY_AUTOCONTINUE") == "1");
    p = PlanAction(Action::SinglePlayer, g);
    assert(p.ok() && p.relaunch && EnvOf(p, "BB_PARTY", &found).empty() && found);
    p = PlanAction(Action::SinglePlayer, Title(false, false));
    assert(!p.ok());
    std::puts("PASS: action plans");
}

static void TestBat() {
    Context c = Title(false, false);
    Plan p = PlanAction(Action::NewParty, c);
    std::string bat, err;
    assert(RelaunchBat(p, &bat, &err));
    assert(bat.find("set \"BB_PARTY=host\"\r\n") != std::string::npos);
    assert(bat.find("set \"BB_PARTY_PASSWORD=\"\r\n") != std::string::npos);
    p.env.push_back({"BB_PARTY_NAME", "x&del *"});
    assert(!RelaunchBat(p, &bat, &err));
    p.env.back() = {"BB_X", "a\"b"};
    assert(!RelaunchBat(p, &bat, &err));
    p.env.back() = {"BB_X", "100%"};
    assert(!RelaunchBat(p, &bat, &err));
    std::puts("PASS: relaunch file");
}

int main() {
    TestJson();
    TestPlans();
    TestBat();
    // Automatic party names: the user name's allowed characters + '-' + 4 base-36 digits.
    assert(DefaultName("Visa Pimi\xc3\xa4", 0) == "Visa_Pimi-0000");
    assert(DefaultName("", 35) == "Hunter-z000");
    assert(DefaultName("averyveryverylongname", 1).size() == 16);
    assert(DefaultName("x", 1) != DefaultName("x", 2));
    assert(NormalizeAddress(" bbp1-ab ") == "BBP1-AB");
    assert(NormalizeAddress("bbp2-ab12-cd") == "BBP2-AB12-CD");
    assert(NormalizeAddress(" host.example:9307 ") == "host.example:9307");
    std::puts("PASS: party flow");
    return 0;
}
