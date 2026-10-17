// SPDX-License-Identifier: GPL-3.0-or-later
// Party co-op main-thread cost probes (BB_PARTY_PERF=1). Every zone keeps a per-call histogram
// (all threads) and a per-frame histogram (the sum of its calls on the coop tick's thread between
// two SprjFlipper::Update entries). A background thread logs p50 / p99 / max every
// BB_PARTY_PERF_SECONDS (default 10) and swaps the histograms; nothing is printed on the game's
// threads. Off (the default): one predictable branch per probe.
//
//   coop::perf::Scope z(coop::perf::Zone::Travel);   // RAII: time until the end of the scope
//   coop::perf::FrameBoundary();                      // the tick's entry, main thread
#pragma once

#include <atomic>
#include <cstdint>

namespace coop::perf {

enum class Zone : int {
    Tick,           ///< CoopTick (everything below on the tick)
    LuaEvents,
    Director,       ///< PartyDirector::Tick
    DirReadState,   ///< ReadGameState in the director
    DirNpcTest,
    DirStart,       ///< UpdateStart / GrantBells (start / readiness reads)
    Progress,       ///< progress::DirectorTick
    ProgressScan,   ///< the host's safe-point diff (HostTick)
    ProgressGuest,  ///< the guest's apply (GuestTick)
    ProgressBridge, ///< HostSide / GuestSide (roster, sends, snapshots)
    ProgressDump,   ///< BBPF dumps (BB_PARTY_FLAG_DUMP)
    DirTitle,       ///< FROM client description / AutoContinue
    DirSend,        ///< roster entry, travel / story / items sends
    StatusBridge,   ///< party::bridge::Tick
    Phantom,
    DirRing,
    Travel,
    Story,
    StorySidecar,   ///< party_story.json write (queued)
    Items,
    Seamless,
    SeamlessParams, ///< ApplyParamRules
    Fourp,
    HookSetFlag,       ///< 0x13CFCC0
    HookSetFlagValue,  ///< 0x13D0060
    HookBroadcastFlag, ///< 0x132AAD0
    HookEmkEnd,        ///< 0x12ED100
    HookEmevd,         ///< 0x17B93A0 (our part, not the game's dispatch)
    HookAward,         ///< 0x17DDC50
    HookGiveList,      ///< 0x17D89F0
    HookFunnel,        ///< 0x13CDE30 travel funnel (our part)
    HookLampWarp,
    HookRemo,          ///< story bank-2002 handler
    HookFourpSlot,     ///< H1 SessionSlotSummary (our part)
    HookFourpSpfx,     ///< H4 ApplySpEffect 7501 check
    Log,               ///< party log lines (printf + fflush)
    B2State,
    B2Work,
    B2Travel,
    HudMenu,
    HudPlate,
    HudVitals,
    Balance,
    Pvp,
    HookCollision,
    CollisionScan,
    kCount
};
constexpr int kZoneCount = static_cast<int>(Zone::kCount);

const char* ZoneName(Zone z);

/// Reads BB_PARTY_PERF once (any thread); starts the reporter when on.
bool Init();
extern bool g_on; // set once by Init before the hooks go in; read-only afterwards

inline bool On() { return g_on; }

std::uint64_t Now(); // QPC ticks
void Record(Zone z, std::uint64_t ticks);
/// The coop tick's entry on the game's main thread: closes the last frame's per-zone sums.
void FrameBoundary();

class Scope {
public:
    explicit Scope(Zone z) : zone_(z), t0_(g_on ? Now() : 0) {}
    ~Scope() {
        if (t0_) {
            Record(zone_, Now() - t0_);
        }
    }
    /// Ends this zone's time here (if it runs) and starts `z`'s: consecutive sections of one
    /// function, or a restart after End().
    void Next(Zone z) {
        if (!g_on) {
            return;
        }
        const std::uint64_t t = Now();
        if (t0_) {
            Record(zone_, t - t0_);
        }
        t0_ = t;
        zone_ = z;
    }
    /// Ends the zone early (nothing is recorded at the end of the scope).
    void End() {
        if (t0_) {
            Record(zone_, Now() - t0_);
            t0_ = 0;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone_;
    std::uint64_t t0_;
};

} // namespace coop::perf
