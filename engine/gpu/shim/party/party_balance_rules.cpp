// SPDX-License-Identifier: GPL-3.0-or-later
// Party balance rules, the pure part (see party_balance_rules.h).
#include "party_balance_rules.h"

#include "../net/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace coop::balance {

const int kHpTable[11] = {0, 25, 50, 75, 100, 125, 150, 200, 250, 300, 400};
const int kDamageTable[11] = {0, 5, 10, 15, 20, 25, 30, 40, 50, 75, 100};
const int kPoiseTable[11] = {-100, -75, -50, -25, 0, 25, 50, 100, 150, 200, 300};

bool Rules::operator==(const Rules& o) const {
    return rot == o.rot && boss_hp == o.boss_hp && boss_damage == o.boss_damage && boss_poise == o.boss_poise &&
           enemy_hp == o.enemy_hp && enemy_damage == o.enemy_damage && enemy_poise == o.enemy_poise &&
           friendly_fire == o.friendly_fire && share_lamps == o.share_lamps;
}

bool Doping::operator==(const Doping& o) const {
    return hp_rate == o.hp_rate && resist_rate == o.resist_rate && attack_rate == o.attack_rate &&
           super_armor == o.super_armor;
}

Rules Vanilla() {
    Rules r;
    r.rot = false;
    r.boss_hp = 100;
    r.boss_damage = 0;
    r.boss_poise = 100;
    r.enemy_hp = 0;
    r.enemy_damage = 0;
    r.enemy_poise = 0;
    return r;
}

int Position(const int* table, int value) {
    int best = 0;
    for (int i = 1; i < 11; ++i) {
        if (std::abs(table[i] - value) < std::abs(table[best] - value)) best = i;
    }
    return best;
}

int AtPosition(const int* table, int position) {
    return table[std::clamp(position, 0, 10)];
}

Rules Clamped(const Rules& r) {
    Rules c = r;
    c.boss_hp = std::clamp(r.boss_hp, kHpTable[0], kHpTable[10]);
    c.enemy_hp = std::clamp(r.enemy_hp, kHpTable[0], kHpTable[10]);
    c.boss_damage = std::clamp(r.boss_damage, kDamageTable[0], kDamageTable[10]);
    c.enemy_damage = std::clamp(r.enemy_damage, kDamageTable[0], kDamageTable[10]);
    c.boss_poise = std::clamp(r.boss_poise, kPoiseTable[0], kPoiseTable[10]);
    c.enemy_poise = std::clamp(r.enemy_poise, kPoiseTable[0], kPoiseTable[10]);
    return c;
}

std::string ToJson(const Rules& r) {
    json::Value v = json::Value::make_object();
    v.set("rot", r.rot);
    v.set("boss_hp", r.boss_hp);
    v.set("boss_damage", r.boss_damage);
    v.set("boss_poise", r.boss_poise);
    v.set("enemy_hp", r.enemy_hp);
    v.set("enemy_damage", r.enemy_damage);
    v.set("enemy_poise", r.enemy_poise);
    v.set("friendly_fire", r.friendly_fire);
    v.set("share_lamps", r.share_lamps);
    return json::dump(v, 0);
}

bool FromJson(const std::string& text, Rules* out, std::string* error) {
    json::Value v;
    std::string err;
    if (text.size() > 4096 || !json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not a JSON object" : err;
        return false;
    }
    Rules r;
    if (const json::Value* m = v.find("rot"); m && m->type == json::Value::Type::Bool) r.rot = m->boolean;
    if (const json::Value* m = v.find("friendly_fire"); m && m->type == json::Value::Type::Bool)
        r.friendly_fire = m->boolean;
    if (const json::Value* m = v.find("share_lamps"); m && m->type == json::Value::Type::Bool)
        r.share_lamps = m->boolean;
    const auto num = [&](const char* key, int* to) {
        const json::Value* m = v.find(key);
        if (m && m->type == json::Value::Type::Number && std::isfinite(m->number) && std::fabs(m->number) < 1e6) {
            *to = static_cast<int>(std::lround(m->number));
        }
    };
    num("boss_hp", &r.boss_hp);
    num("boss_damage", &r.boss_damage);
    num("boss_poise", &r.boss_poise);
    num("enemy_hp", &r.enemy_hp);
    num("enemy_damage", &r.enemy_damage);
    num("enemy_poise", &r.enemy_poise);
    *out = Clamped(r);
    return true;
}

namespace {

Doping Make(int hp, int damage, int poise, int cooperators) {
    Doping d;
    const int n = std::clamp(cooperators, 0, 3);
    if (n == 0) return d;
    const float step = kVanillaHpStep * static_cast<float>(hp) / 100.0f;
    d.hp_rate = 1.0f + static_cast<float>(n) * step;
    d.resist_rate = d.hp_rate;
    d.attack_rate = 1.0f + static_cast<float>(n) * static_cast<float>(damage) / 100.0f;
    d.super_armor = static_cast<int>(std::lround(static_cast<double>(n) * kVanillaPoiseStep * poise / 100.0));
    return d;
}

}  // namespace

Doping Boss(const Rules& r, int cooperators) {
    const Rules c = Clamped(r);
    return Make(c.boss_hp, c.boss_damage, c.boss_poise, cooperators);
}

Doping Enemy(const Rules& r, int cooperators) {
    const Rules c = Clamped(r);
    return Make(c.enemy_hp, c.enemy_damage, c.enemy_poise, cooperators);
}

bool EnemyScalingActive(const Rules& r) {
    const Rules c = Clamped(r);
    return c.enemy_hp != 0 || c.enemy_damage != 0 || c.enemy_poise != 0;
}

int RotExtraStaminaDrain(int prev, int now, int prev_max, int now_max, float factor, float* carry) {
    if (prev < 0 || now < 0 || prev_max <= 0 || prev_max != now_max || now >= prev || !(factor > 1.0f)) {
        return 0;
    }
    float extra = static_cast<float>(prev - now) * (factor - 1.0f) + (carry ? *carry : 0.0f);
    int whole = static_cast<int>(extra);
    if (whole > now) whole = now;  // never below 0
    if (carry) *carry = extra - static_cast<float>(static_cast<int>(extra));
    return whole < 0 ? 0 : whole;
}

int RotRallyTakeBack(int prev, int now, int prev_max, int now_max, float factor, bool vial, float* carry) {
    if (vial || prev <= 0 || now <= prev || prev_max <= 0 || prev_max != now_max || !(factor < 1.0f)) {
        return 0;
    }
    const float f = factor < 0.0f ? 0.0f : factor;
    // (+1e-4: 0.15 is not exact in a float; 100 * (1 - 0.85) must give 15, not 14.99)
    const float back = static_cast<float>(now - prev) * (1.0f - f) + (carry ? *carry : 0.0f) + 1e-4f;
    int whole = static_cast<int>(back);
    if (whole > now - prev) whole = now - prev;
    if (carry) *carry = back - 1e-4f - static_cast<float>(static_cast<int>(back));
    if (carry && *carry < 0) *carry = 0;
    return whole < 0 ? 0 : whole;
}

bool RotRallyGainInWindow(double since_damage_s) {
    return since_damage_s >= 0.0 && since_damage_s <= kRotRallyWindowS;
}

float RotItemDiscovery(float own, int stacks) {
    const int s = std::clamp(stacks, 0, kRotMaxStacks);
    if (!(own > 0.0f) || s == 0) return own;
    return std::max(kRotDiscoveryFloor, own - kRotDiscoveryPerStack * static_cast<float>(s));
}

float RotHpFactor(int stacks) {
    const int s = std::clamp(stacks, 0, kRotMaxStacks);
    return 1.0f - static_cast<float>(s * kRotHpPercentPerStack) / 100.0f;
}

bool LampWarpIsRespawn(bool alive_at_warp, double since_death_s, bool up_since_death) {
    if (!alive_at_warp) return true;
    return since_death_s >= 0 && since_death_s < kRespawnWarpS && !up_since_death;
}

bool LampMenuRest(bool dialog_opened, bool alive, float lamp_dx, float lamp_dy, float lamp_dz) {
    return dialog_opened && alive && lamp_dx * lamp_dx + lamp_dz * lamp_dz <= kLampMenuRadiusM * kLampMenuRadiusM &&
           std::fabs(lamp_dy) <= kLampMenuHeightM;
}

bool RotFromAnotherCharacter(int stacks, bool own_prologue, double prologue_s) {
    return stacks > 0 && own_prologue && prologue_s >= kStaleRotPrologueS;
}

int SlotRot::Stacks() const {
    int n = 0;
    for (int k : kind) n += k;
    return n;
}

SlotRot RotFile::Get(int slot) const {
    for (const SlotRot& r : slots)
        if (r.slot == slot) return r;
    SlotRot none;
    none.slot = slot;
    return none;
}

void RotFile::Set(const SlotRot& r) {
    slots.erase(std::remove_if(slots.begin(), slots.end(), [&](const SlotRot& o) { return o.slot == r.slot; }),
                slots.end());
    if (r.Stacks() > 0) slots.push_back(r);
}

RotFile ParseRotFile(const std::string& text) {
    RotFile f;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string key;
        if (!(l >> key)) continue;
        if (key == "stacks") {  // the old unkeyed file
            int v = 0;
            if (l >> v) f.legacy_stacks = std::clamp(v, 0, kRotMaxStacks);
            continue;
        }
        if (key != "slot") continue;  // "kindK N" of the old file, or anything unknown
        SlotRot r;
        std::string word;
        if (!(l >> r.slot >> word) || word != "kinds" || r.slot < -1 || r.slot > 9) continue;
        int left = kRotMaxStacks;
        for (int& k : r.kind) {
            int n = 0;
            if (!(l >> n)) break;
            k = std::clamp(n, 0, left);
            left -= k;
        }
        f.Set(r);  // a later line for the same slot wins
    }
    return f;
}

std::string FormatRotFile(const RotFile& f) {
    std::string out;
    for (const SlotRot& r : f.slots) {
        if (r.Stacks() <= 0) continue;
        out += "slot " + std::to_string(r.slot) + " kinds";
        for (int k : r.kind) out += " " + std::to_string(k);
        out += "\n";
    }
    return out;
}

bool RotSlotChanged(int active_slot, int current_slot) {
    return current_slot >= 0 && current_slot != active_slot;
}

std::string DescribeHp(int percent, bool russian) {
    char b[96];
    const int per = static_cast<int>(std::lround(percent * kVanillaHpStep));
    std::snprintf(b, sizeof b, russian ? "%d %% (+%d %% здоровья за охотника)" : "%d %% (+%d %% HP per extra hunter)",
                  percent, per);
    return b;
}

std::string DescribeDamage(int percent, bool russian) {
    char b[96];
    std::snprintf(b, sizeof b, russian ? "+%d %% урона за охотника" : "+%d %% damage per extra hunter", percent);
    return b;
}

std::string DescribePoise(int percent, bool russian) {
    char b[96];
    const int per = static_cast<int>(std::lround(percent * kVanillaPoiseStep / 100.0));
    std::snprintf(b, sizeof b, russian ? "%d %% (%+d стойкости за охотника)" : "%d %% (%+d poise per extra hunter)",
                  percent, per);
    return b;
}

}  // namespace coop::balance
