namespace BloodborneLauncher.Core;

public enum VpnIdleAction { None, Disconnect, Connect }

/// <summary>Why the launcher looks now: a timer tick, or a moment the player may need the tunnel.</summary>
public enum VpnIdleTrigger { Tick, WindowActivated, CoopShown, Play }

/// <summary>What the launcher knows when it looks (visy 0.7.8 handoff, docs/nightwatch/INTEGRATION.md item 2).</summary>
public readonly record struct VpnIdleState(
    bool Enabled,          // setting vpn_idle_disconnect (on by default)
    bool Wanted,           // the player connected in this launcher and has not pressed Disconnect since
    bool Owned,            // NightwatchVpn.OwnedHere
    bool Up,               // a tunnel address is on this PC
    bool Busy,             // a connect / disconnect is under way, or OpenVPN is being set up
    bool GameRunning,
    bool WindowActive,     // the launcher window is in front and not minimized
    DateTime? InactiveSince,
    TimeSpan IdleAfter,    // setting vpn_idle_minutes (default 10)
    DateTime Now);

/// <summary>
/// Drops a tunnel nobody uses and brings it back when needed, so a launcher left open does not hold a VPN address and
/// server capacity (on 2026-10-31, 18 of 30 connected players were in no game). Only for a connection the player made in
/// this launcher and still wants: a manual Disconnect keeps it off, and nothing here ever connects a player who never
/// pressed Connect. Never while the game runs, while a connect or disconnect is under way, or for another launcher's tunnel.
/// </summary>
public static class VpnIdle
{
    public static readonly TimeSpan DefaultIdle = TimeSpan.FromMinutes(10);
    public static readonly TimeSpan PlayWait = TimeSpan.FromSeconds(15);

    public static VpnIdleAction Decide(in VpnIdleState s, VpnIdleTrigger trigger)
    {
        if (!s.Enabled || !s.Wanted || s.Busy) return VpnIdleAction.None;
        if (!s.Up)
            return trigger == VpnIdleTrigger.Tick ? VpnIdleAction.None : VpnIdleAction.Connect;
        if (trigger != VpnIdleTrigger.Tick || !s.Owned || s.GameRunning || s.WindowActive || s.InactiveSince is not { } since)
            return VpnIdleAction.None;
        return s.Now - since >= s.IdleAfter ? VpnIdleAction.Disconnect : VpnIdleAction.None;
    }

    /// <summary>The idle time from the setting: whole minutes 1..240, anything else the default.</summary>
    public static TimeSpan IdleAfter(string? minutes) =>
        int.TryParse(minutes, out var m) && m is >= 1 and <= 240 ? TimeSpan.FromMinutes(m) : DefaultIdle;
}
