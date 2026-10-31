namespace BloodborneLauncher.Core;

/// <summary>The host's visibility and network settings, applied only after a user choice.</summary>
public static class CoopHostMode
{
    public static void Apply(bool isPublic, bool show, string currentVpnPref, Action<string, string> setIni)
    {
        setIni("party_vpn_lobby", isPublic && show ? "1" : "0");
        setIni("party_discovery", show ? "1" : "0");
        setIni("party_open", show ? "1" : "0");
        setIni("party_vpn", isPublic || string.IsNullOrEmpty(currentVpnPref) || currentVpnPref == "off" ? "auto" : currentVpnPref);
    }

    /// <summary>The party type is per user but these settings are per install: a folder the launcher never set them in (a new
    /// install, or an update) shows the party while the game keeps it closed. Show and open are always written together, so
    /// a difference means that; true when it was repaired.</summary>
    public static bool Repair(bool isPublic, Func<string, string, string> get, Action<string, string> setIni)
    {
        var show = get("party_discovery", "1");
        if (show == get("party_open", "0")) return false;
        Apply(isPublic, show == "1", get("party_vpn", "auto"), setIni);
        return true;
    }

    public static bool MatchesSearch(string name, string search) =>
        name.Contains(search.Trim(), StringComparison.OrdinalIgnoreCase);
}
