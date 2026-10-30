// SPDX-License-Identifier: GPL-3.0-or-later
// "Stay in my world" (CL-adeb50), the game side (party_stayhome.h).
#include "party_stayhome.h"

#include "party_async_io.h"
#include "party_link.h"
#include "party_status.h"
#include "../bbport_settings.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace coop::stayhome {

namespace {

std::mutex g_mu;
Book g_book;
// guest
bool g_sent_on = false, g_ever_sent = false;
double g_sent_at = 0;
bool g_logged_on = false;

double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    coop::asyncio::Print(std::string("Party stay: ") + line + "\n");
}

}  // namespace

bool Active() {
    return BbSettings::Get().party_stay.load();
}

void OnEvent(int slot, const std::string& body) {
    bool on = false;
    if (slot <= 0 || !Decode(body, &on)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    const bool was = g_book.Stays(slot, Now());
    g_book.Set(slot, on, Now());
    if (was != on) Log("guest slot %d %s", slot, on ? "stays in its own world: no bell, no vote, no gather for it" : "joins again");
}

bool SlotStays(int slot) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_book.Stays(slot, Now());
}

void Tick(party::PartyLink* link, bool host) {
    if (host) {
        unsigned mask = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (int slot = 1; slot < 8; ++slot) {
                if (!g_book.Stays(slot, Now())) continue;
                bool here = false;
                if (link) {
                    for (const party::RosterEntry& e : link->roster()) here = here || (e.slot == slot && e.connected);
                }
                if (!here) g_book.Forget(slot);
                else mask |= 1u << slot;
            }
        }
        if (mask != party::status::StayMask()) party::status::SetStayMask(mask);
        return;
    }
    const bool on = Active();
    if (on != g_logged_on) {
        g_logged_on = on;
        Log("%s", on ? "staying in my own world: no automatic joining" : "automatic joining resumes");
    }
    if (!link || link->state() != party::LinkState::Connected) {
        g_ever_sent = false;  // a new connection: say it again
        return;
    }
    const double now = Now();
    if (!ShouldReport(on, g_sent_on, g_ever_sent, now - g_sent_at)) return;
    link->post_event(party::kHostSlot, kEventName, Encode(on));
    g_sent_on = on;
    g_ever_sent = true;
    g_sent_at = now;
}

}  // namespace coop::stayhome
