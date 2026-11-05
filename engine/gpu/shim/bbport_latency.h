// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: input latency probe (from visy's 8ee5500e, CL-7ae614). BB_LATENCY_LOG=1 prints once a second
//   latency: n=<frames> pad_age_us avg/p95/max=... read_to_present_ms avg/p95/max=... reads=...
//            read_to_flip_ms avg/p95/max=... flip_to_present_ms avg/p95/max=...
// pad_age: how old the SDL pad state was when the game read it (time since the window thread's last event
// pump, measured before any fresh update); read_to_flip: from that read to the flip request; flip_to_present:
// the flip waiting for its turn; read_to_present: from the game's last pad read before a flip to the CPU call
// of that frame's present (the share of latency this process can shorten; it excludes the display, the
// queued present and the device). tools/windows/analyze_latency.py sums a log. Nothing runs when unset.
#pragma once

#include <cstdint>

namespace BbLatency {

bool Enabled();
/// Window thread, after each event pump.
void NotePump();
/// Pad read (any thread): measures the pump age, remembers the read time. Call before a fresh update.
void NotePadRead();
/// Time of the newest pad read (steady clock, ns); 0 when the probe is off.
std::uint64_t LastReadNs();
/// Steady clock now in ns when the probe is on, else 0.
std::uint64_t NowNs();
/// A real present: `read_ns` is LastReadNs() taken when its flip was queued, `flip_ns` NowNs() then.
void NotePresent(std::uint64_t read_ns, std::uint64_t flip_ns);

} // namespace BbLatency
