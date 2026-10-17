// SPDX-License-Identifier: GPL-3.0-or-later
// The in-game party flow (party_flow.h, docs/party/ingame_flow.md).
#include "party_flow.h"

#include "../net/json.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace party::flow {

namespace fs = std::filesystem;

// ---- base64 and DPAPI ----------------------------------------------------------------------

namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(const std::string& in) {
    std::string out;
    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += kB64[v >> 6 & 63];
        out += kB64[v & 63];
        i += 3;
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < in.size() ? kB64[v >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

bool Unbase64(const std::string& in, std::string* out) {
    out->clear();
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const char* p = std::strchr(kB64, c);
        if (!p || !c) return false;
        v = v << 6 | unsigned(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(char(v >> bits & 0xff));
        }
    }
    return true;
}

#if defined(_WIN32)
// crypt32 loaded on first use (no link dependency; the unit test uses it too).
struct Blob {
    DWORD cbData;
    BYTE* pbData;
};
using ProtectFn = BOOL(WINAPI*)(Blob*, LPCWSTR, Blob*, PVOID, PVOID, DWORD, Blob*);
using UnprotectFn = BOOL(WINAPI*)(Blob*, LPWSTR*, Blob*, PVOID, PVOID, DWORD, Blob*);
constexpr DWORD kUiForbidden = 0x1;  // CRYPTPROTECT_UI_FORBIDDEN

HMODULE Crypt32() {
    static HMODULE m = LoadLibraryW(L"crypt32.dll");
    return m;
}
// Fixed extra entropy: the blob is only for party.json's password.
const char kEntropy[] = "bbport-party-password-1";

bool Protect(const std::string& clear, std::string* blob) {
    auto fn = Crypt32() ? reinterpret_cast<ProtectFn>(reinterpret_cast<void*>(GetProcAddress(Crypt32(), "CryptProtectData"))) : nullptr;
    if (!fn) return false;
    Blob in{DWORD(clear.size()), reinterpret_cast<BYTE*>(const_cast<char*>(clear.data()))};
    Blob ent{DWORD(sizeof kEntropy - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    Blob out{0, nullptr};
    if (!fn(&in, L"bbport party password", &ent, nullptr, nullptr, kUiForbidden, &out)) return false;
    blob->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool Unprotect(const std::string& blob, std::string* clear) {
    auto fn = Crypt32() ? reinterpret_cast<UnprotectFn>(reinterpret_cast<void*>(GetProcAddress(Crypt32(), "CryptUnprotectData"))) : nullptr;
    if (!fn) return false;
    Blob in{DWORD(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    Blob ent{DWORD(sizeof kEntropy - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    Blob out{0, nullptr};
    if (!fn(&in, nullptr, &ent, nullptr, nullptr, kUiForbidden, &out)) return false;
    clear->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
#else
bool Protect(const std::string&, std::string*) { return false; }
bool Unprotect(const std::string&, std::string*) { return false; }
#endif

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    std::size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    return s.substr(a);
}

std::string Str(const json::Value& v, const char* key) {
    const json::Value* m = v.find(key);
    return m && m->type == json::Value::Type::String ? m->string : std::string();
}
int Int(const json::Value& v, const char* key, int fallback) {
    const json::Value* m = v.find(key);
    return m && m->type == json::Value::Type::Number ? int(m->number) : fallback;
}
bool Bool(const json::Value& v, const char* key) {
    const json::Value* m = v.find(key);
    return m && m->type == json::Value::Type::Bool && m->boolean;
}

bool ValidRole(const std::string& r) { return r == "host" || r == "join"; }

}  // namespace

// ---- party.json -----------------------------------------------------------------------------

std::string ToJson(const Saved& s, bool protect) {
    json::Value v = json::Value::make_object();
    v.set("version", 1);
    v.set("note", "bbport party settings (the game's System > Party page). The password is protected "
                  "with Windows DPAPI for this Windows user when password_dpapi is used; a plain "
                  "\"password\" is stored in clear text.");
    v.set("name", s.name);
    v.set("role", s.role);
    v.set("host_address", s.host_address);
    std::string blob;
    if (!s.password.empty() && protect && Protect(s.password, &blob)) {
        v.set("password_dpapi", Base64(blob));
    } else if (!s.password.empty()) {
        v.set("password", s.password);
    }
    v.set("port", s.port);
    v.set("has_character", s.has_character);
    v.set("slot", s.slot);
    if (!s.host_secret.empty()) v.set("host_secret", s.host_secret);
    return json::dump(v) + "\n";
}

bool FromJson(const std::string& text, Saved* out, std::string* error) {
    json::Value v;
    std::string err;
    if (!json::parse(text, v, err) || v.type != json::Value::Type::Object) {
        if (error) *error = err.empty() ? "not a JSON object" : err;
        return false;
    }
    Saved s;
    s.name = Str(v, "name");
    s.role = Str(v, "role");
    if (!ValidRole(s.role)) s.role.clear();
    s.host_address = NormalizeAddress(Str(v, "host_address"));
    const std::string prot = Str(v, "password_dpapi");
    std::string blob;
    if (!prot.empty() && Unbase64(prot, &blob) && Unprotect(blob, &s.password)) {
    } else if (!Str(v, "password").empty()) {
        s.password = Str(v, "password");
        s.password_plain = true;
    }
    s.port = std::clamp(Int(v, "port", 0), 0, 65535);
    s.has_character = Bool(v, "has_character");
    s.slot = Int(v, "slot", -1);
    s.host_secret = Str(v, "host_secret");
    if (s.host_secret.size() != 16 ||
        !std::all_of(s.host_secret.begin(), s.host_secret.end(), [](char c) { return std::isxdigit((unsigned char)c); }))
        s.host_secret.clear();
    *out = s;
    return true;
}

bool Load(const std::string& path, Saved* out, std::string* error) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        if (error) *error = "no " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return FromJson(ss.str(), out, error);
}

bool Store(const std::string& path, const Saved& s, std::string* error) {
    std::error_code ec;
    const fs::path p = fs::path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        f << ToJson(s);
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    if (ec && error) *error = "cannot replace " + path + ": " + ec.message();
    return !ec;
}

std::string DefaultName(const std::string& user, std::uint32_t random) {
    std::string base;
    for (const char c : user) {
        if (base.size() >= 11) break;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') base += c;
        else if (c == ' ') base += '_';
    }
    if (base.empty()) base = "Hunter";
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    base += '-';
    for (int i = 0; i < 4; ++i) {
        base += kDigits[random % 36];
        random /= 36;
    }
    return base;
}

std::string NormalizeAddress(const std::string& text) {
    std::string s = Trim(text);
    // A party code (BBP3- / BBP2- / BBP1-...): upper case. A host name starting with "bbp" has no '-' at [4].
    if (s.size() > 5 && (s[0] | 0x20) == 'b' && (s[1] | 0x20) == 'b' && (s[2] | 0x20) == 'p' && (s[4] == '-' || s[4] == ' ')) {
        for (char& c : s) c = char(std::toupper((unsigned char)c));
    }
    return s;
}

// ---- actions --------------------------------------------------------------------------------

const char* ActionName(Action a) {
    switch (a) {
    case Action::NewParty: return "new";
    case Action::JoinParty: return "join";
    case Action::ContinueParty: return "continue";
    case Action::HostParty: return "host";
    case Action::LeaveParty: return "leave";
    case Action::SinglePlayer: return "solo";
    }
    return "?";
}

bool PartyActionsAllowed(const Context& c) { return !c.in_world || (c.party_process && c.party_save); }

namespace {

void PartyEnv(Plan* p, const Saved& s, bool host, bool auto_continue) {
    p->relaunch = true;
    p->env = {
        {"BB_PARTY", host ? "host" : "join"},
        {"BB_PARTY_HOST", ""},
        {"BB_PARTY_SAVE", "separate"},
        {"BB_PARTY_MENU", "1"},
        {"BB_PARTY_AUTOCONTINUE", auto_continue ? "1" : "0"},
        {"BB_PARTY_RESTARTED", ""},
        // party.json has them (the password protected): not in the environment or the logs.
        {"BB_PARTY_CODE", ""},
        {"BB_PARTY_PASSWORD", ""},
        {"BB_SKIP_NETWORK_CHOICE", "online"},
    };
    if (s.port > 0) p->env.push_back({"BB_PARTY_PORT", std::to_string(s.port)});
    if (!s.name.empty()) p->env.push_back({"BB_PARTY_NAME", s.name});
}

}  // namespace

Plan PlanAction(Action a, const Context& c) {
    Plan p;
    p.saved = c.saved;
    Saved& s = p.saved;
    const bool title = !c.in_world;
    if (!PartyActionsAllowed(c) && a != Action::SinglePlayer) {
        p.error = "A single-player character is loaded: quit to the title, then System > Party";
        return p;
    }
    switch (a) {
    case Action::NewParty:
        if (!title) {
            p.error = "Quit to the title first: a new party game starts there";
            return p;
        }
        s.role = "host";
        PartyEnv(&p, s, true, false);
        p.message = "Restarting into the party game (host): choose New Game";
        return p;
    case Action::JoinParty:
        if (s.host_address.empty()) {
            p.error = "Enter the host's address or party code first";
            return p;
        }
        if (c.party_process && !c.host) {
            if (c.link_active) {
                p.error = "Already in the party";
                return p;
            }
            p.local = Plan::Local::Rejoin;
            p.message = "Joining the party again";
            return p;
        }
        s.role = "join";
        PartyEnv(&p, s, false, !title || s.has_character);
        p.message = !title || s.has_character ? "Restarting to join the party: your party character continues"
                                              : "Restarting to join the party: choose New Game";
        return p;
    case Action::ContinueParty:
        if (!title) {
            p.error = "Already playing";
            return p;
        }
        if (!s.has_character) {
            p.error = "No party character yet: New party game or Join party game";
            return p;
        }
        if (s.role == "join" && s.host_address.empty()) {
            p.error = "Enter the host's address or party code first";
            return p;
        }
        if (s.role.empty()) s.role = "host";
        PartyEnv(&p, s, s.role == "host", true);
        p.message = s.role == "host" ? "Restarting into the party game (host)" : "Restarting to join the party";
        return p;
    case Action::HostParty:
        if (c.party_process && c.host) {
            if (c.link_active) {
                p.error = "Already hosting";
                return p;
            }
            p.local = Plan::Local::Rejoin;
            p.message = "Hosting again";
            return p;
        }
        if (!c.party_process && !s.has_character) {
            p.error = "No party character yet: New party game";
            return p;
        }
        s.role = "host";
        PartyEnv(&p, s, true, !title || s.has_character);
        p.message = "Restarting to host the party";
        return p;
    case Action::LeaveParty:
        if (!c.party_process) {
            p.error = "Not in a party";
            return p;
        }
        if (!c.link_active) {
            p.error = c.host ? "Not hosting" : "Not in a party";
            return p;
        }
        p.local = Plan::Local::Leave;
        p.message = c.host ? "The party ends; guests return to their worlds" : "Leaving the party";
        return p;
    case Action::SinglePlayer:
        if (!c.party_process) {
            p.error = "Already playing single-player";
            return p;
        }
        p.relaunch = true;
        p.env = {{"BB_PARTY", ""},           {"BB_PARTY_HOST", ""},          {"BB_PARTY_MENU", ""},
                 {"BB_PARTY_AUTOCONTINUE", ""}, {"BB_PARTY_RESTARTED", ""},  {"BB_SKIP_NETWORK_CHOICE", ""}};
        p.message = "Restarting into the single-player game";
        return p;
    }
    p.error = "unknown action";
    return p;
}

bool RelaunchBat(const Plan& p, std::string* out, std::string* error) {
    std::string text = "@rem bbport: the party flow's next start (party_flow.cpp); run.bat calls and deletes it\r\n";
    for (const auto& [k, v] : p.env) {
        const auto ok = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == ':' || c == '-' || c == '@'; };
        if (k.empty() || !std::all_of(k.begin(), k.end(), ok) || !std::all_of(v.begin(), v.end(), ok)) {
            if (error) *error = "unsafe value for " + k;
            return false;
        }
        text += "set \"" + k + "=" + v + "\"\r\n";
    }
    *out = text;
    return true;
}

}  // namespace party::flow
