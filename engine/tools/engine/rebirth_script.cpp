// SPDX-License-Identifier: GPL-3.0-or-later
#include "rebirth_script.h"

#include "esd.h"
#include "sha256.h"

namespace {
constexpr const char* kGameScript = "e56163a1bb8f46ae59368160e522e11f970e2268f6d356b823e4d72a7c969ff1";
constexpr const char* kOurScript = "f155f1d1a07f3a721d755d6e7af4a47144e14464c4b8f61d6d3889fcacb10a53";
constexpr std::int64_t kFillList = 2147483640, kTalk = 2147483641;
constexpr std::int64_t kListEnd = 3;
constexpr std::int64_t kChoice = 1, kEnd = 6;

esd::Command Cmd(std::int32_t id, std::vector<std::int64_t> args = {}) {
    esd::Command c{1, id, {}};
    for (const std::int64_t a : args) c.args.push_back(esd::End(esd::Int(a)));
    return c;
}
esd::Command Wait() { return {1, 68, {esd::End(esd::Real(0.33))}}; }
esd::Command Message(std::int32_t text) { return Cmd(17, {7, text, 1, 0, 1}); }
esd::Bytes Flag(std::uint32_t flag, std::int64_t value) { return esd::Equals(esd::Call(15, {flag}), value); }
}

bool rebirth_script(std::vector<std::uint8_t>& bytes, std::string* why) {
    const auto fail = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (sha256_hex(bytes.data(), bytes.size()) != kGameScript) return fail("not 1.09's t242307 (the Altar of Despair)");
    auto script = esd::Script::Read(bytes, why);
    if (!script) return false;
    esd::Script& s = *script;
    const esd::Ref list_end = s.State(kFillList, kListEnd), choice = s.State(kTalk, kChoice), end = s.State(kTalk, kEnd);
    if (list_end == esd::kNone || choice == esd::kNone || end == esd::kNone) return fail("the altar's menu states are missing");
    using namespace rebirth;

    // The altar's list (group kFillList): 4 picks 2 (two rows) or 3 (one row) by the game's own flags, 3 adds its row
    // and goes to 5. Rebirth's row comes before 3's through a gate: on kOffered, 7 adds it; either way then 3.
    enum { kGate = 6, kAddOurs };
    const esd::Ref gate = s.NewState(kFillList, kGate), add = s.NewState(kFillList, kAddOurs);
    int into_list_end = 0;
    for (std::int64_t id = 0; id < kListEnd + 2; ++id) {
        const esd::Ref state = s.State(kFillList, id);
        if (state == esd::kNone || state == list_end) continue;
        for (const esd::Ref c : s.Conditions(s.StateList(state, 0)))
            if (s.Target(c) == list_end) {
                s.SetTarget(c, gate);
                ++into_list_end;
            }
    }
    if (into_list_end != 2) return fail("the altar's list does not reach its last row as 1.09's does");
    s.SetStateList(gate, 0, s.NewConditions({s.NewCondition(add, Flag(kOffered, 1)), s.NewCondition(list_end, esd::True())}));
    s.SetStateList(add, 1, s.NewCommands({Cmd(19, {3, kTextOption, -1})}));
    s.SetStateList(add, 0, s.NewConditions({s.NewCondition(list_end, esd::True())}));

    enum { kReset = 7, kFailed, kLevelUp, kAcceptUndo, kUndo };
    esd::Ref st[kUndo + 1]{};
    for (int id = kReset; id <= kUndo; ++id) st[id] = s.NewState(kTalk, id);
    const auto conds = [&](esd::Ref state, std::vector<std::pair<esd::Ref, esd::Bytes>> to) {
        std::vector<esd::Ref> list;
        for (auto& [target, expression] : to) list.push_back(s.NewCondition(target, expression));
        s.SetStateList(state, 0, s.NewConditions(list));
    };
    const auto entry = [&](esd::Ref state, std::vector<esd::Command> c) { s.SetStateList(state, 1, s.NewCommands(c)); };
    const auto waits = [&](esd::Ref state) { s.SetStateList(state, 3, s.NewCommands({Wait()})); };
    const esd::Bytes box_closed = esd::Equals(esd::Call(58, {0}), 0);

    entry(st[kReset], {Cmd(11, {kFail, 0}), Cmd(11, {kReqReset, 1})});
    waits(st[kReset]);
    conds(st[kReset], {{st[kFailed], Flag(kFail, 1)}, {st[kLevelUp], Flag(kReqReset, 0)}});
    entry(st[kFailed], {Cmd(11, {kFail, 0}), Message(kTextFail)});
    conds(st[kFailed], {{end, box_closed}});
    entry(st[kLevelUp], {Cmd(11, {kMenu, 1}), Cmd(31)});
    conds(st[kLevelUp], {{st[kAcceptUndo], Flag(kMenu, 0)}});
    entry(st[kAcceptUndo], {Cmd(20), Cmd(19, {1, kTextAccept, -1}), Cmd(19, {2, kTextUndo, -1}), Cmd(76)});
    waits(st[kAcceptUndo]);
    conds(st[kAcceptUndo], {{end, esd::Equals(esd::Call(23, {}, true), 1)},
                            {st[kUndo], esd::Equals(esd::Register0(), 2)},
                            {st[kLevelUp], esd::Equals(esd::Call(53, {}), 0)}});
    entry(st[kUndo], {Cmd(11, {kReqUndo, 1})});
    waits(st[kUndo]);
    conds(st[kUndo], {{end, Flag(kReqUndo, 0)}});

    std::vector<esd::Ref> choices = s.Conditions(s.StateList(choice, 0));
    if (choices.empty()) return fail("the altar's choice has no conditions");
    choices.insert(choices.begin() + 1, s.NewCondition(st[kReset],esd::Equals(esd::Register0(), 3)));
    s.SetStateList(choice, 0, s.NewConditions(choices));

    std::vector<std::uint8_t> out = s.Write();
    if (!esd::Script::Read(out, why)) return false;
    if (const std::string got = sha256_hex(out.data(), out.size()); got != kOurScript) {
        if (why) *why = "the edited script is not the known result (" + got + ")";
        return false;
    }
    bytes = std::move(out);
    return true;
}
