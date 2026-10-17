// SPDX-License-Identifier: GPL-3.0-or-later
#include "upnp_win.h"

#include "party_code.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#endif

namespace party {

#if defined(_WIN32)
// natupnp.h is not part of mingw-w64: the interfaces from natupnp.idl (Windows SDK), as used.
// They must NOT be in an anonymous namespace: GCC then knows every class derived from them (none -
// the objects come from COM) and, with LTO, turned the calls into a call to nowhere (crash at
// RIP 0x100000000 right after CoCreateInstance).
namespace upnp_com {
const GUID kClsidUPnPNAT = {0xAE1E00AA, 0x3FD5, 0x403C, {0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1}};
const GUID kIidIUPnPNAT = {0xB171C812, 0xCC76, 0x485A, {0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99}};

struct IStaticPortMapping : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_ExternalIPAddress(BSTR* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExternalPort(long* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InternalPort(long* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Protocol(BSTR* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InternalClient(BSTR* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Enabled(VARIANT_BOOL* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Description(BSTR* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE EditInternalClient(BSTR bstrInternalClient) = 0;
    virtual HRESULT STDMETHODCALLTYPE Enable(VARIANT_BOOL vb) = 0;
    virtual HRESULT STDMETHODCALLTYPE EditDescription(BSTR bstrDescription) = 0;
    virtual HRESULT STDMETHODCALLTYPE EditInternalPort(long lInternalPort) = 0;
};

struct IStaticPortMappingCollection : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown** pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Item(long lExternalPort, BSTR bstrProtocol, IStaticPortMapping** ppSPM) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Count(long* pVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(long lExternalPort, BSTR bstrProtocol) = 0;
    virtual HRESULT STDMETHODCALLTYPE Add(long lExternalPort, BSTR bstrProtocol, long lInternalPort,
                                          BSTR bstrInternalClient, VARIANT_BOOL bEnabled, BSTR bstrDescription,
                                          IStaticPortMapping** ppSPM) = 0;
};

struct IUPnPNAT : public IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_StaticPortMappingCollection(IStaticPortMappingCollection** ppSPMs) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DynamicPortMappingCollection(IUnknown** ppDPMs) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NATEventManager(IUnknown** ppNEM) = 0;
};

}  // namespace upnp_com
using namespace upnp_com;

namespace {

std::string narrow(BSTR b) {
    if (!b) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, b, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, b, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace
#endif

struct UpnpMapper::State {
    std::mutex mu;
    std::condition_variable cv;
    UpnpResult result;
    bool started = false;
    bool stop_requested = false;
    bool finished = false;  // the thread has removed the mappings and exited
    std::uint16_t port = 0;
    std::string local_ip;
};

UpnpMapper::UpnpMapper() : state_(std::make_shared<State>()) {}
UpnpMapper::~UpnpMapper() { stop(); }

bool UpnpMapper::enabled_by_env() {
    const char* v = std::getenv("BB_PARTY_UPNP");
    return !(v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0));
}

std::string UpnpMapper::manual_hint(std::uint16_t port) {
    return "UPnP unavailable: forward UDP+TCP port " + std::to_string(port) +
           " to this PC in your router (or share the LAN code / use a VPN like Tailscale)";
}

#if defined(_WIN32)
static void upnp_thread(std::shared_ptr<UpnpMapper::State> st);
#endif

void UpnpMapper::start(std::uint16_t port, const std::string& local_ip) {
    std::lock_guard<std::mutex> lk(state_->mu);
    if (state_->started) return;
    state_->started = true;
    state_->port = port;
    state_->local_ip = local_ip;
    if (!enabled_by_env()) {
        state_->result.done = true;
        state_->result.disabled = true;
        state_->result.message = "UPnP disabled (BB_PARTY_UPNP=0); forward UDP+TCP port " + std::to_string(port) +
                                 " to this PC manually";
        state_->finished = true;
        return;
    }
#if defined(_WIN32)
    if (state_->local_ip.empty()) {
        std::array<std::uint8_t, 4> ip{};
        if (best_local_ipv4(&ip)) state_->local_ip = format_ipv4(ip);
    }
    if (state_->local_ip.empty()) {
        state_->result.done = true;
        state_->result.message = "no local IPv4 address for UPnP; " + manual_hint(port);
        state_->finished = true;
        return;
    }
    state_->result.message = "UPnP: still waiting for the router; " + manual_hint(port);
    std::thread(upnp_thread, state_).detach();
#else
    state_->result.done = true;
    state_->result.message = manual_hint(port);
    state_->finished = true;
#endif
}

UpnpResult UpnpMapper::wait(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_->mu);
    state_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return state_->result.done; });
    return state_->result;
}

UpnpResult UpnpMapper::result() const {
    std::lock_guard<std::mutex> lk(state_->mu);
    return state_->result;
}

void UpnpMapper::stop(int timeout_ms) {
    std::unique_lock<std::mutex> lk(state_->mu);
    if (!state_->started || state_->finished) return;
    state_->stop_requested = true;
    state_->cv.notify_all();
    // A hung router call: the detached thread keeps its own reference to the state and exits
    // whenever COM returns.
    state_->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return state_->finished; });
}

#if defined(_WIN32)
static void upnp_thread(std::shared_ptr<UpnpMapper::State> st) {
    std::uint16_t port;
    std::string local_ip;
    {
        std::lock_guard<std::mutex> lk(st->mu);
        port = st->port;
        local_ip = st->local_ip;
    }
    auto finish_result = [&](bool ok, std::string ext, std::string msg) {
        std::lock_guard<std::mutex> lk(st->mu);
        st->result.done = true;
        st->result.ok = ok;
        st->result.external_ip = std::move(ext);
        st->result.message = std::move(msg);
        st->cv.notify_all();
    };

    HRESULT hr_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUPnPNAT* nat = nullptr;
    IStaticPortMappingCollection* coll = nullptr;
    bool mapped_udp = false, mapped_tcp = false;
    BSTR udp = SysAllocString(L"UDP");
    BSTR tcp = SysAllocString(L"TCP");
    BSTR desc = SysAllocString(L"Bloodborne party");
    std::wstring wip(local_ip.begin(), local_ip.end());
    BSTR client = SysAllocString(wip.c_str());

    HRESULT hr = CoCreateInstance(kClsidUPnPNAT, nullptr, CLSCTX_INPROC_SERVER, kIidIUPnPNAT,
                                  reinterpret_cast<void**>(&nat));
    if (FAILED(hr) || !nat) {
        finish_result(false, {}, "UPnP service not available on this PC; " + UpnpMapper::manual_hint(port));
    } else {
        hr = nat->get_StaticPortMappingCollection(&coll);  // discovery: may take seconds
        if (FAILED(hr) || !coll) {
            finish_result(false, {}, "no UPnP router found (or UPnP disabled on it); " + UpnpMapper::manual_hint(port));
        } else {
            std::string ext;
            auto add = [&](BSTR proto, bool* mapped) {
                IStaticPortMapping* m = nullptr;
                HRESULT h = coll->Add(port, proto, port, client, VARIANT_TRUE, desc, &m);
                if (FAILED(h) || !m) {
                    // A stale mapping from an earlier run (or another PC): replace it.
                    coll->Remove(port, proto);
                    h = coll->Add(port, proto, port, client, VARIANT_TRUE, desc, &m);
                }
                if (SUCCEEDED(h) && m) {
                    *mapped = true;
                    if (ext.empty()) {
                        BSTR e = nullptr;
                        if (SUCCEEDED(m->get_ExternalIPAddress(&e)) && e) {
                            ext = narrow(e);
                            SysFreeString(e);
                        }
                    }
                    m->Release();
                }
            };
            add(udp, &mapped_udp);
            add(tcp, &mapped_tcp);
            if (mapped_udp && mapped_tcp)
                finish_result(true, ext,
                              "UPnP: mapped UDP+TCP port " + std::to_string(port) + " to " + local_ip +
                                  (ext.empty() ? std::string() : " (external " + ext + ")"));
            else
                finish_result(false, ext, "the router refused the UPnP mapping; " + UpnpMapper::manual_hint(port));
        }
    }

    {
        std::unique_lock<std::mutex> lk(st->mu);
        st->cv.wait(lk, [&] { return st->stop_requested; });
    }
    if (coll) {
        if (mapped_udp) coll->Remove(port, udp);
        if (mapped_tcp) coll->Remove(port, tcp);
        coll->Release();
    }
    if (nat) nat->Release();
    SysFreeString(udp);
    SysFreeString(tcp);
    SysFreeString(desc);
    SysFreeString(client);
    if (SUCCEEDED(hr_init)) CoUninitialize();
    std::lock_guard<std::mutex> lk(st->mu);
    st->finished = true;
    st->cv.notify_all();
}
#endif

}  // namespace party
