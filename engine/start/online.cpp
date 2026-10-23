// SPDX-License-Identifier: GPL-3.0-or-later
#include "online.h"

namespace start {
namespace {

const char* const kOnlineEnv[] = {"BB_ONLINE", "BB_SHADNET_SERVER", "BB_SHADNET_WEBAPI", "BB_SHADNET_NPID",
                                  "BB_SHADNET_PASSWORD", "BB_SHADNET_TOKEN", "BB_UPNP", "BB_P2P_PORT",
                                  "SHADPS4_HTTP_HOST_OVERRIDES_JSON", "BB_SIGN_TIMEOUT", "BB_NET_BACKEND",
                                  "BB_THD_SERVER", "BB_THD_AUTH", "BB_THD_TOKEN", "BB_THD_ONLINE_ID", "BB_THD_RULESET"};

} // namespace

bool ConfigureOnline(const std::map<std::string, std::string>&, const fs::path&) {
    // Nightwatch Co-op has no public online play (no shadNet, no The Hunter's Dream): co-op goes
    // through the party's own network layer. Whatever bbport.ini or the environment says, the game
    // starts with the online stack off.
    for (const char* key : kOnlineEnv) UnsetEnv(key);
    SetEnv("BB_ONLINE", "0");
    return false;
}

} // namespace start
