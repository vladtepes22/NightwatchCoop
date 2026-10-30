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

    public static bool MatchesSearch(string name, string search) =>
        name.Contains(search.Trim(), StringComparison.OrdinalIgnoreCase);
}
