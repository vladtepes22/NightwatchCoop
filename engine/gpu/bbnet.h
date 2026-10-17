/* SPDX-License-Identifier: GPL-3.0-or-later
 * C interface of the party network library (gpu/shim/net): libSceNet/NetCtl/Np for serverless
 * party co-op (docs/PARTY_COOP_PLAN.md). Inert unless BB_PARTY is set. */
#ifndef BBNET_H
#define BBNET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Directories the library keeps state in (optional; the environment is read on first use). */
void bbnet_configure(const char *app0, const char *user_dir);
/* Start after all game hooks are installed so peers compare the complete hook identity. */
void bbnet_start(void);
/* The loaded eboot image (bbgpu_patch_image passes the loader's pointer and size): the party
 * code reads and writes game data through it (the Matching2 signaling gate, FrpgNetMan). */
void bbnet_set_image(void *image, uint64_t size);
/* 1 when BB_PARTY turns party mode on (gpu/party_env.h: unset, empty, 0, off, false, no = off). */
int bbnet_party_enabled(void);
/* Function for an import, given its symbol name ("sceNetSocket") or scoped NID ("NID#L#M",
 * translated with runtime_symbol), or 0: always 0 without BB_PARTY, for NpTrophy, NpCommerce,
 * NpProfileDialog and NpScore, and for names the library does not implement, which stay on the
 * runtime's stubs. Net, NetCtl, Np (manager, auth, WebApi, lookup), NpMatching2, NpSignaling,
 * Http and Ssl are the library's. */
uintptr_t bbnet_resolve(const char *scoped_nid_or_name);
/* One line describing the party state (role, name, port, counters, simulator). */
const char *bbnet_status_line(void);
/* Ends the party (BYE to the other members, UPnP mappings removed); bounded (< 2.5 s), idempotent,
 * a no-op without BB_PARTY. Every process exit path calls it (gpu/shim/party/party_runtime.cpp). */
void bbnet_shutdown(void);
/* Calls the guest's System V function fn(a0..a5) on the dispatcher thread, which is attached
 * to the runtime and restores the guest FS base before every call; calls run in order. */
void bbnet_post_guest_call(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                           uint64_t a5);
#ifdef __cplusplus
}
#endif
#endif
