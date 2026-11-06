// SPDX-License-Identifier: GPL-3.0-or-later
#include "events.h"

#include <functional>
#include <set>
#include <utility>

namespace bbdata {
namespace {

struct FlagOpDef {
    std::uint32_t bank, id;
    FlagOp op;
};
// instructions whose arguments name event flags (bank, id)
constexpr FlagOpDef kFlagOps[] = {
    {2003, 2, FlagOp::Set},           {2003, 9, FlagOp::Toggle},       {2003, 17, FlagOp::Random},
    {2003, 22, FlagOp::Batch},        {2003, 31, FlagOp::ValueInc},    {2003, 32, FlagOp::ValueClear},
    {2003, 41, FlagOp::ValueOp},      {2003, 42, FlagOp::ValueItemCount}, {2003, 43, FlagOp::ValueItemDirect},
    {2003, 25, FlagOp::AiSos},        {2003, 51, FlagOp::NpcSummon},   {2003, 33, FlagOp::HawkInfo},
    {2003, 34, FlagOp::HawkDrop},     {2009, 0, FlagOp::Ladder},       {2009, 1, FlagOp::WanderInit},
    {2009, 2, FlagOp::WanderReg},     {2009, 3, FlagOp::Bonfire},      {2009, 5, FlagOp::Lamp},
    {2005, 9, FlagOp::ObjDamage},     {2005, 12, FlagOp::ObjEventDel}, {2004, 45, FlagOp::NpcHumanity},
};
struct MarkerDef {
    std::uint32_t bank, id;
    MarkerKind kind;
};
constexpr MarkerDef kMarkers[] = {
    {2003, 12, MarkerKind::BossDefeat}, {2003, 53, MarkerKind::BossDefeat}, {2003, 15, MarkerKind::MidbossDefeat},
    {2003, 4, MarkerKind::ItemLot},     {2003, 36, MarkerKind::ItemLotClient}, {2003, 14, MarkerKind::MapMove},
    {2003, 28, MarkerKind::Trophy},     {2003, 21, MarkerKind::NgPlus},     {2003, 27, MarkerKind::TitleExit},
    {2003, 49, MarkerKind::WarpReturn}, {2000, 5, MarkerKind::SaveRequest}, {2000, 2, MarkerKind::NetworkSync},
    {2009, 6, MarkerKind::BossRoomEnter},
};
// conditions / control flow that read flags
constexpr std::pair<std::uint32_t, std::uint32_t> kReadOps[] = {
    {3, 0},    {3, 1},    {3, 10},   {3, 12},   {3, 20},   {1003, 0}, {1003, 1},
    {1003, 2}, {1003, 3}, {1003, 4}, {1003, 101}, {1003, 103}, {2004, 32},
};

bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

/// The flag ranges an instruction writes or registers: (first, last, kind, state). Too few values: none.
void FlagArgs(FlagOp op, const std::vector<std::int64_t>& v, std::vector<Op>* out) {
    auto need = [&](std::size_t n) { return v.size() >= n; };
    auto add = [&](std::int64_t first, std::int64_t last, StateKind k, std::int64_t state = 0) {
        Op o;
        o.op = op;
        o.first = first;
        o.last = last;
        o.kind = k;
        o.state = state;
        out->push_back(o);
    };
    switch (op) {
        case FlagOp::Set:
            if (need(2)) add(v[0], v[0], StateKind::Int, v[1]);
            break;
        case FlagOp::Toggle:
            if (need(1)) add(v[0], v[0], StateKind::Int, 2);
            break;
        case FlagOp::Random:
            if (need(2)) add(v[0], v[1], StateKind::Rand);
            break;
        case FlagOp::Batch:
            if (need(3)) add(v[0], v[1], StateKind::Int, v[2]);
            break;
        case FlagOp::ValueInc:
        case FlagOp::ValueClear:
        case FlagOp::ValueOp:
            if (need(2)) add(v[0], v[0] + v[1] - 1, StateKind::Value);
            break;
        case FlagOp::ValueItemCount:
        case FlagOp::ValueItemDirect:
            if (need(4)) add(v[2], v[2] + v[3] - 1, StateKind::Value);
            break;
        case FlagOp::AiSos:
        case FlagOp::NpcSummon:
            if (need(5)) {
                add(v[3], v[3], StateKind::Reg);
                add(v[4], v[4], StateKind::Reg);
            }
            break;
        case FlagOp::HawkInfo:
            if (need(1)) add(v[0], v[0], StateKind::Reg);
            break;
        case FlagOp::HawkDrop:
            if (need(3)) add(v[2], v[2], StateKind::Reg);
            break;
        case FlagOp::Ladder:
            if (need(2)) {
                add(v[0], v[0], StateKind::Reg);
                add(v[1], v[1], StateKind::Reg);
            }
            break;
        case FlagOp::WanderInit:
            if (need(3)) {
                add(v[0], v[0], StateKind::Reg);
                add(v[2], v[2], StateKind::Reg);
            }
            break;
        case FlagOp::WanderReg:
        case FlagOp::Lamp:
        case FlagOp::Bonfire:
        case FlagOp::ObjDamage:
        case FlagOp::ObjEventDel:
            if (need(1)) add(v[0], v[0], StateKind::Reg);
            break;
        case FlagOp::NpcHumanity:
            if (need(2)) add(v[1], v[1], StateKind::Reg);
            break;
    }
}

struct InsView {
    std::uint32_t bank, id;
    const std::uint8_t* p;
    std::size_t n;
};

struct Instance {
    std::int32_t slot = 0;
    bool has_params = false;  // false: Python's None
    std::string params;
    bool operator==(const Instance& o) const { return slot == o.slot && has_params == o.has_params && params == o.params; }
};

struct CallSite {
    std::int64_t caller;
    std::int32_t slot;
    std::string params;
    const EvEvent* ce;
};

class Expander {
public:
    Expander(const Emevd& ev, const Emedf& emedf, const ExternalCalls& external)
        : ev_(ev), emedf_(emedf), external_(external) {
        for (const EvEvent& e : ev.events) {
            by_id_[e.id] = &e;
        }
        for (const EvEvent& e : ev.events) {
            for (const EvInstr& in : e.ins) {
                const Span raw = ev.Args(in);
                if (in.bank == 2000 && in.id == 0 && raw.n >= 8) {
                    const std::int32_t slot = raw.Le<std::int32_t>(0);
                    const std::uint32_t target = raw.Le<std::uint32_t>(4);
                    calls_[target].push_back({e.id, slot, std::string(reinterpret_cast<const char*>(raw.p) + 8, raw.n - 8), &e});
                }
            }
        }
    }

    /// Instruction list of `e` with the parameters of `inst` substituted (storage keeps patched copies alive).
    void Concretise(const EvEvent& e, const Instance* inst, std::vector<std::string>& storage, std::vector<InsView>& out) const {
        out.clear();
        for (const EvInstr& in : e.ins) {
            out.push_back({in.bank, in.id, ev_.buf.data() + in.off, in.len});
        }
        if (!inst || !inst->has_params || e.params.empty()) {
            return;
        }
        storage.assign(out.size(), std::string());
        std::vector<bool> copied(out.size(), false);
        for (const EvParam& p : e.params) {
            if (p.instr < 0 || p.instr >= static_cast<std::int64_t>(out.size()) || p.target < 0 || p.source < 0 || p.count < 0 ||
                p.source + p.count > static_cast<std::int64_t>(inst->params.size())) {
                continue;
            }
            const std::size_t i = static_cast<std::size_t>(p.instr);
            if (!copied[i]) {
                storage[i].assign(reinterpret_cast<const char*>(out[i].p), out[i].n);
                copied[i] = true;
            }
            std::string& raw = storage[i];
            const std::size_t start = std::min<std::size_t>(static_cast<std::size_t>(p.target), raw.size());
            const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(p.target + p.count), raw.size());
            raw.replace(start, end - start, inst->params, static_cast<std::size_t>(p.source), static_cast<std::size_t>(p.count));
            out[i].p = reinterpret_cast<const std::uint8_t*>(raw.data());
            out[i].n = raw.size();
        }
        // pointers into `storage` strings stay valid only while a string is not reassigned: refresh them
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (copied[i]) {
                out[i].p = reinterpret_cast<const std::uint8_t*>(storage[i].data());
                out[i].n = storage[i].size();
            }
        }
    }

    std::vector<Instance> Instances(std::int64_t eid, int depth) const {
        const auto cit = calls_.find(eid);
        const auto xit = external_.find(eid);
        if (cit == calls_.end() && xit == external_.end()) {
            if (by_id_.count(eid)) {
                return {Instance{0, false, std::string()}};
            }
            return {};
        }
        std::vector<Instance> res;
        if (xit != external_.end()) {
            for (const ExternalCall& x : xit->second) {
                res.push_back({x.slot, true, x.params});
            }
        }
        if (cit != calls_.end()) {
            for (const CallSite& cs : cit->second) {
                if (!cs.ce->params.empty() && depth < 4) {
                    // a parameterised caller: expand its own instances first
                    for (const Instance& ci : Instances(cs.caller, depth + 1)) {
                        std::vector<std::string> storage;
                        std::vector<InsView> cins;
                        Concretise(*cs.ce, &ci, storage, cins);
                        for (const InsView& iv : cins) {
                            if (iv.bank == 2000 && iv.id == 0 && iv.n >= 8) {
                                const Span raw(iv.p, iv.n);
                                if (static_cast<std::int64_t>(raw.Le<std::uint32_t>(4)) == eid) {
                                    res.push_back({raw.Le<std::int32_t>(0), true,
                                                   std::string(reinterpret_cast<const char*>(iv.p) + 8, iv.n - 8)});
                                }
                            }
                        }
                    }
                } else {
                    res.push_back({cs.slot, true, cs.params});
                }
            }
        }
        // de-duplicate identical instances (first kept)
        std::vector<Instance> uniq;
        for (const Instance& r : res) {
            bool seen = false;
            for (const Instance& u : uniq) {
                if (u == r) {
                    seen = true;
                    break;
                }
            }
            if (!seen) uniq.push_back(r);
        }
        return uniq;
    }

    void Run(const std::unordered_map<std::int64_t, std::string>& names, std::vector<EventRec>* out) const {
        std::vector<std::string> storage;
        std::vector<InsView> ins;
        for (const EvEvent& e : ev_.events) {
            std::vector<Instance> insts = Instances(e.id, 0);
            if (insts.empty() && e.id != 0 && e.id != 50) {
                insts.push_back({0, false, std::string()});  // uncalled
            }
            if (insts.empty()) {
                insts.push_back({0, false, std::string()});  // root
            }
            const auto nit = names.find(e.id);
            for (const Instance& inst : insts) {
                Concretise(e, &inst, storage, ins);
                EventRec rec;
                rec.event = e.id;
                rec.slot = inst.slot;
                rec.rest = e.rest;
                if (nit != names.end()) rec.name = nit->second;
                for (const InsView& iv : ins) {
                    const Span raw(iv.p, iv.n);
                    if (iv.bank == 2000 && iv.id == 0) {
                        if (iv.n >= 8) {
                            rec.calls.push_back({raw.Le<std::int32_t>(0), raw.Le<std::uint32_t>(4),
                                                 std::string(reinterpret_cast<const char*>(iv.p) + 8, iv.n - 8)});
                        }
                        continue;
                    }
                    const EmedfEntry* def = emedf_.Find(iv.bank, iv.id);
                    std::vector<std::int64_t> vals;
                    if (def && !def->args.empty()) {
                        vals = UnpackArgs(*def, raw);
                    }
                    if ((iv.bank == 3 && (iv.id == 0 || iv.id == 2 || iv.id == 3 || iv.id == 4 || iv.id == 24)) || (iv.bank == 4 && iv.id == 0) || (iv.bank == 2004 && iv.id == 8)) {
                        rec.conds.push_back({iv.bank, iv.id, vals});
                    }
                    bool done = false;
                    for (const FlagOpDef& f : kFlagOps) {
                        if (f.bank == iv.bank && f.id == iv.id) {
                            FlagArgs(f.op, vals, &rec.ops);
                            done = true;
                            break;
                        }
                    }
                    if (done) continue;
                    for (const MarkerDef& m : kMarkers) {
                        if (m.bank == iv.bank && m.id == iv.id) {
                            rec.markers.push_back({m.kind, 0, std::move(vals)});
                            done = true;
                            break;
                        }
                    }
                    if (done) continue;
                    if (iv.bank == 2002 && iv.id >= 1 && iv.id <= 8) {
                        rec.markers.push_back({MarkerKind::Cutscene, static_cast<std::uint8_t>(iv.id), std::move(vals)});
                        continue;
                    }
                    bool read = false;
                    for (const auto& r : kReadOps) {
                        if (r.first == iv.bank && r.second == iv.id) {
                            read = true;
                            break;
                        }
                    }
                    if (read && def) {
                        std::vector<std::int64_t> ids;
                        for (std::size_t k = 0; k < def->args.size() && k < vals.size(); ++k) {
                            const std::string& n = def->args[k].name;
                            if ((Contains(n, "イベントフラグID") || Contains(n, "イベントフラグ先頭")) && !Contains(n, "タイプ")) {
                                ids.push_back(vals[k]);
                            }
                        }
                        if (!ids.empty()) rec.reads.push_back(std::move(ids));
                    }
                }
                out->push_back(std::move(rec));
            }
        }
    }

private:
    const Emevd& ev_;
    const Emedf& emedf_;
    const ExternalCalls& external_;
    std::unordered_map<std::int64_t, const EvEvent*> by_id_;
    std::unordered_map<std::int64_t, std::vector<CallSite>> calls_;
};

}  // namespace

std::vector<EventRec> ExpandFile(const Emevd& ev, const std::unordered_map<std::int64_t, std::string>& names,
                                 const Emedf& emedf, const ExternalCalls& external) {
    std::vector<EventRec> out;
    Expander(ev, emedf, external).Run(names, &out);
    return out;
}

}  // namespace bbdata
