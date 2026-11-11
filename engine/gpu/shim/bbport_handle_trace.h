// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_HANDLE_TRACE=1 -- who creates kernel Event / Semaphore handles while the game runs
// (0.8.1 leak hunt: ~7 Events per load were left unexplained). The import tables of every loaded module are
// patched for kernel32!CreateEvent[Ex]W / CreateSemaphore[Ex]W and the ntdll Nt*Event/Semaphore creates that
// kernelbase calls; each creation is attributed to the first stack frame outside kernel32/kernelbase/ntdll
// (module + RVA) and the counts are printed every 30 s with the process's handle count. Diagnostic only, off
// by default.
#pragma once
namespace BbHandleTrace {
/// Call periodically from any thread (cheap when disabled): starts the trace on first use, reports every 30 s.
void Tick();
} // namespace BbHandleTrace
