// SPDX-License-Identifier: GPL-3.0-or-later
// UPnP port mapping for the party host through Windows' own IUPnPNAT (COM; ole32/oleaut32):
// TCP + UDP mappings of the party port to this PC, description "Bloodborne party", removed again
// on stop(). UPnP discovery can hang for a long time on some routers, so all COM work runs on a
// background thread and callers only ever wait with a timeout. BB_PARTY_UPNP=0 disables it.
// Non-Windows: always reports "not available".
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace party {

struct UpnpResult {
    bool done = false;         // the attempt finished (successfully or not)
    bool ok = false;           // both mappings are in place
    bool disabled = false;     // BB_PARTY_UPNP=0
    std::string external_ip;   // when the router reports it (may be empty even when ok)
    std::string message;       // human-readable outcome, e.g. the manual port-forward hint
};

class UpnpMapper {
public:
    UpnpMapper();
    ~UpnpMapper();  // stop()
    UpnpMapper(const UpnpMapper&) = delete;
    UpnpMapper& operator=(const UpnpMapper&) = delete;

    // Starts mapping `port` (TCP and UDP, external = internal) to `local_ip` (dotted IPv4; empty
    // = best_local_ipv4()). Returns immediately.
    void start(std::uint16_t port, const std::string& local_ip = {});
    // Waits up to timeout_ms for the attempt; returns the state at that point (done = false on
    // timeout: the message then already carries the manual port-forward hint).
    UpnpResult wait(int timeout_ms);
    UpnpResult result() const;
    // Removes the mappings (waits up to timeout_ms for the router; a hung thread is abandoned).
    void stop(int timeout_ms = 3000);

    static bool enabled_by_env();  // BB_PARTY_UPNP != "0"
    static std::string manual_hint(std::uint16_t port);  // "forward UDP+TCP port N to this PC"

    struct State;  // shared with the (possibly abandoned) COM thread

private:
    std::shared_ptr<State> state_;
};

}  // namespace party
