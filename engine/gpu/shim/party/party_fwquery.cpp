// SPDX-License-Identifier: GPL-3.0-or-later
// The real Windows Firewall query behind connwarn::QueryFirewall (INetFwPolicy2 / INetFwRules: read
// only, no extra process, never changes a rule). Runs in its own COM apartment on the calling thread.
#include "party_connwarn.h"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>

namespace party::connwarn {
namespace {

std::string Narrow(BSTR b) {
    if (!b) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string ExpandEnv(const std::string& s) {
    if (s.find('%') == std::string::npos) return s;
    char buf[MAX_PATH * 2] = {};
    const DWORD n = ExpandEnvironmentStringsA(s.c_str(), buf, sizeof buf);
    return n > 0 && n < sizeof buf ? std::string(buf) : s;
}

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

}  // namespace

FwReport QueryFirewallWindows(const std::string& exe, int port) {
    FwReport out;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        out.detail = "COM unavailable";
        return out;
    }
    INetFwPolicy2* pol = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2),
                                  reinterpret_cast<void**>(&pol));
    if (FAILED(hr) || !pol) {
        out.detail = "the Windows Firewall policy is not available";
        if (uninit) CoUninitialize();
        return out;
    }
    std::vector<FwProfile> active;
    std::vector<FwRule> rules;
    long cur = 0;
    if (SUCCEEDED(pol->get_CurrentProfileTypes(&cur))) {
        for (const int bit : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
            if (!(cur & bit)) continue;
            FwProfile p;
            p.bit = bit;
            VARIANT_BOOL on = VARIANT_TRUE;
            NET_FW_ACTION act = NET_FW_ACTION_BLOCK;
            if (SUCCEEDED(pol->get_FirewallEnabled(static_cast<NET_FW_PROFILE_TYPE2>(bit), &on))) p.enabled = on != VARIANT_FALSE;
            if (SUCCEEDED(pol->get_DefaultInboundAction(static_cast<NET_FW_PROFILE_TYPE2>(bit), &act)))
                p.inbound_blocked = act == NET_FW_ACTION_BLOCK;
            active.push_back(p);
        }
    }
    INetFwRules* list = nullptr;
    if (SUCCEEDED(pol->get_Rules(&list)) && list) {
        IUnknown* en_unk = nullptr;
        IEnumVARIANT* en = nullptr;
        if (SUCCEEDED(list->get__NewEnum(&en_unk)) && en_unk) en_unk->QueryInterface(IID_PPV_ARGS(&en));
        Release(en_unk);
        if (en) {
            VARIANT v;
            VariantInit(&v);
            ULONG got = 0;
            while (en->Next(1, &v, &got) == S_OK && got == 1) {
                INetFwRule* r = nullptr;
                if (v.vt == VT_DISPATCH && v.pdispVal &&
                    SUCCEEDED(v.pdispVal->QueryInterface(__uuidof(INetFwRule), reinterpret_cast<void**>(&r))) && r) {
                    FwRule fr;
                    VARIANT_BOOL enabled = VARIANT_FALSE;
                    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
                    NET_FW_ACTION act = NET_FW_ACTION_BLOCK;
                    r->get_Enabled(&enabled);
                    r->get_Direction(&dir);
                    r->get_Action(&act);
                    fr.enabled = enabled != VARIANT_FALSE;
                    fr.inbound = dir == NET_FW_RULE_DIR_IN;
                    fr.allow = act == NET_FW_ACTION_ALLOW;
                    if (fr.enabled && fr.inbound) {  // the other rules never matter: skip their string reads
                        long proto = 256, prof = 0x7fffffff;
                        BSTR app = nullptr, ports = nullptr;
                        r->get_Protocol(&proto);
                        r->get_Profiles(&prof);
                        r->get_ApplicationName(&app);
                        r->get_LocalPorts(&ports);
                        fr.protocol = static_cast<int>(proto);
                        fr.profiles = static_cast<int>(prof);
                        fr.app = ExpandEnv(Narrow(app));
                        fr.local_ports = Narrow(ports);
                        SysFreeString(app);
                        SysFreeString(ports);
                        rules.push_back(std::move(fr));
                    }
                    r->Release();
                }
                VariantClear(&v);
            }
            en->Release();
        }
        list->Release();
    } else {
        out.detail = "the firewall rules cannot be read";
        pol->Release();
        if (uninit) CoUninitialize();
        return out;
    }
    pol->Release();
    if (uninit) CoUninitialize();
    return Classify(rules, active, exe, port);
}

}  // namespace party::connwarn
#else
namespace party::connwarn {
FwReport QueryFirewallWindows(const std::string&, int) {
    FwReport r;
    r.detail = "not Windows";
    return r;
}
}  // namespace party::connwarn
#endif
