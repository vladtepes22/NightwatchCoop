using System.ComponentModel;
using System.Diagnostics;
using System.IO;

namespace BloodborneLauncher.Core;

/// <summary>The "Fix firewall issues" action: inbound allow rules, for the game only, for the party port (TCP + UDP) and
/// party discovery (UDP 9308, also for the launcher, whose Join list listens on it) on every network profile, and removal
/// of those programs' inbound block rules (cancelling the Windows prompt leaves those behind, and a block rule wins over
/// any allow rule). The script is shown to the player first; running it needs an administrator
/// prompt, or the player pastes the same lines into an administrator PowerShell.</summary>
public static class FirewallFix
{
    private const string Name = "Nightwatch Co-op";

    /// <summary>The exact lines that run. Existing rules of the same name are replaced, so running it twice is harmless.</summary>
    public static string Script(int port) => Script(port, Path.Combine(Paths.BinariesDir, Paths.Program("bb-probe")), Environment.ProcessPath);

    /// <summary>The same lines for a given game exe (the tests and the elevated QA check pass a dummy path).</summary>
    public static string Script(int port, string exe, string? launcher = null)
    {
        var lines = new List<string> { UnblockLine(exe) };
        if (launcher != null) lines.Add(UnblockLine(launcher));
        lines.Add(Rule("party (TCP)", "TCP", port, exe));
        lines.Add(Rule("party (UDP)", "UDP", port, exe));
        lines.Add(Rule("party discovery (UDP)", "UDP", PartyDiscovery.Port, exe));
        if (launcher != null) lines.Add(Rule("party discovery, launcher (UDP)", "UDP", PartyDiscovery.Port, launcher));
        return string.Join("\n", lines);
    }

    private static string Rule(string what, string protocol, int port, string program)
    {
        var name = $"{Name} {what} {port}";
        return $"Remove-NetFirewallRule -DisplayName '{name}' -ErrorAction SilentlyContinue; " +
               $"New-NetFirewallRule -DisplayName '{name}' -Direction Inbound -Program '{program.Replace("'", "''")}' -Protocol {protocol} -LocalPort {port} -Profile Any -Action Allow | Out-Null";
    }

    /// <summary>Removes every inbound block rule whose program is exe.</summary>
    public static string UnblockLine(string exe)
    {
        exe = exe.Replace("'", "''");
        return $"Get-NetFirewallApplicationFilter | Where-Object Program -eq '{exe}' | Get-NetFirewallRule | " +
               "Where-Object { $_.Direction -eq 'Inbound' -and $_.Action -eq 'Block' } | Remove-NetFirewallRule";
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
