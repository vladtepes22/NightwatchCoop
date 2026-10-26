using System.ComponentModel;
using System.Diagnostics;
using System.IO;

namespace BloodborneLauncher.Core;

/// <summary>The "Fix firewall issues" action: inbound allow rules for the party port (TCP + UDP) and party discovery
/// (UDP 9308) on every network profile. The script is shown to the player first; running it needs an administrator
/// prompt, or the player pastes the same lines into an administrator PowerShell.</summary>
public static class FirewallFix
{
    private const string Name = "Nightwatch Co-op";

    /// <summary>The exact lines that run. Existing rules of the same name are replaced, so running it twice is harmless.</summary>
    public static string Script(int port) => string.Join("\n", new[]
    {
        Rule("party (TCP)", "TCP", port),
        Rule("party (UDP)", "UDP", port),
        Rule("party discovery (UDP)", "UDP", PartyDiscovery.Port),
    });

    private static string Rule(string what, string protocol, int port)
    {
        var name = $"{Name} {what} {port}";
        return $"Remove-NetFirewallRule -DisplayName '{name}' -ErrorAction SilentlyContinue; " +
               $"New-NetFirewallRule -DisplayName '{name}' -Direction Inbound -Protocol {protocol} -LocalPort {port} -Profile Any -Action Allow | Out-Null";
    }

    /// <summary>Runs the script in an elevated PowerShell that stays open to show the result. False when the player
    /// declined the administrator prompt.</summary>
    public static bool RunElevated(int port)
    {
        var file = Path.Combine(Path.GetTempPath(), "nightwatch-firewall.ps1");
        File.WriteAllText(file, Script(port) + "\nWrite-Host 'Done. You can close this window.'\n");
        var info = new ProcessStartInfo("powershell.exe",
            $"-NoProfile -ExecutionPolicy Bypass -NoExit -File \"{file}\"") { UseShellExecute = true, Verb = "runas" };
        try { Process.Start(info)?.Dispose(); return true; }
        catch (Win32Exception) { return false; }   // 1223: the administrator prompt was declined
    }
}
