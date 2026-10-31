using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;

namespace BloodborneLauncher.Core;

public enum OpenVpnSetupResult { Installed, Declined, Busy, Failed, Missing, Tampered, NotOurs }

/// <summary>First-time setup of online play: the official signed OpenVPN installer, shipped in the package
/// (tools/package-seamless.ps1 reads the name and the hash from here), installed unattended after the player agreed.
/// A PC that already has OpenVPN GUI keeps its own.</summary>
public static class OpenVpnSetup
{
    public const string MsiName = "OpenVPN-2.7.7-I001-amd64.msi";
    public const string MsiSha256 = "9069a48397c4fd4135fb9c9baea4863c74a465651f67e55a423a433e503100bb";
    /// <summary>The GUI runs the tunnel, the service gives it the network rights without a prompt on every Connect. Not
    /// OpenVPN.GUI.OnLogon: nothing starts with Windows.</summary>
    public const string Features = "OpenVPN,OpenVPN.GUI,OpenVPN.Service,Drivers,Drivers.OvpnDco,Drivers.TAPWindows6";

    /// <summary>"Want to know more?" in the install prompt: the README section that says what Nightwatch Online is, how safe
    /// it is, and that it is optional. The section (anchor #nightwatch-online) is written by the 0.2.6 release.</summary>
    public const string LearnMoreUrl = "https://github.com/vladtepes22/NightwatchCoop#nightwatch-online";

    public static string PackagedMsi => Path.Combine(Paths.PortDir, "openvpn", MsiName);

    public static bool HashMatches(Stream msi) =>
        Convert.ToHexString(SHA256.HashData(msi)).Equals(MsiSha256, StringComparison.OrdinalIgnoreCase);

    public static string Arguments(string msi, string log) =>
        $"/i \"{msi}\" /qn /norestart ADDLOCAL={Features} /L*v \"{log}\"";

    public static string UninstallArguments(string msi, string log) => $"/x \"{msi}\" /qn /norestart /L*v \"{log}\"";

    /// <summary>msiexec's exit code: 3010 and 1641 are a success that wants a restart, 1602 cancelled, 1618 another
    /// installer is running, 1605 an uninstall of a product this installer did not put here (the player's own OpenVPN).</summary>
    public static OpenVpnSetupResult ResultOf(int exitCode) => exitCode switch
    {
        0 or 3010 or 1641 => OpenVpnSetupResult.Installed,
        1602 or 1223 => OpenVpnSetupResult.Declined,
        1618 => OpenVpnSetupResult.Busy,
        1605 => OpenVpnSetupResult.NotOurs,
        _ => OpenVpnSetupResult.Failed,
    };

    /// <summary>One UAC prompt, no wizard. The file stays open against writes from the hash check until the installer is
    /// done, so what was checked is what runs.</summary>
    public static Task<OpenVpnSetupResult> InstallAsync() => RunAsync(Arguments, "openvpn-setup.log");

    /// <summary>Removes the OpenVPN this package installs, with the same signed installer (one UAC prompt).</summary>
    public static Task<OpenVpnSetupResult> UninstallAsync() => RunAsync(UninstallArguments, "openvpn-remove.log");

    private static async Task<OpenVpnSetupResult> RunAsync(Func<string, string, string> arguments, string logName)
    {
        try
        {
            await using var msi = new FileStream(PackagedMsi, FileMode.Open, FileAccess.Read, FileShare.Read);
            if (!HashMatches(msi)) return OpenVpnSetupResult.Tampered;
            var start = new ProcessStartInfo("msiexec.exe", arguments(PackagedMsi, Path.Combine(Paths.ConfigDir, logName)))
            {
                UseShellExecute = true,
                Verb = "runas",
            };
            using var installer = Process.Start(start);
            if (installer == null) return OpenVpnSetupResult.Failed;
            await installer.WaitForExitAsync();
            return ResultOf(installer.ExitCode);
        }
        catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return OpenVpnSetupResult.Declined; }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { return OpenVpnSetupResult.Missing; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception) { return OpenVpnSetupResult.Failed; }
    }
}

public enum VpnPhase { Off, Connecting, On, Disconnecting }

/// <summary>The connection as the page shows it: Connecting and Disconnecting hold until the tunnel's adapter confirms
/// them, or the deadline passes.</summary>
public sealed class VpnConnection
{
    public static readonly TimeSpan ConnectLimit = TimeSpan.FromSeconds(60), DisconnectLimit = TimeSpan.FromSeconds(15);
    private DateTime _deadline;

    public VpnPhase Phase { get; private set; } = VpnPhase.Off;
    public bool TimedOut { get; private set; }
    public bool Busy => Phase is VpnPhase.Connecting or VpnPhase.Disconnecting;

    public void BeginConnect(DateTime now) => Begin(VpnPhase.Connecting, now + ConnectLimit);
    public void BeginDisconnect(DateTime now) => Begin(VpnPhase.Disconnecting, now + DisconnectLimit);

    /// <summary>A failure before the tunnel had its chance: back to what the adapter says, no timeout to report.</summary>
    public void Abort(bool tunnelUp)
    {
        Phase = tunnelUp ? VpnPhase.On : VpnPhase.Off;
        TimedOut = false;
    }

    private void Begin(VpnPhase phase, DateTime deadline)
    {
        Phase = phase;
        _deadline = deadline;
        TimedOut = false;
    }

    public VpnPhase Observe(bool tunnelUp, DateTime now)
    {
        var settled = Phase switch
        {
            VpnPhase.Connecting => tunnelUp,
            VpnPhase.Disconnecting => !tunnelUp,
            _ => true,
        };
        if (settled || now >= _deadline)
        {
            TimedOut = !settled;
            Phase = tunnelUp ? VpnPhase.On : VpnPhase.Off;
        }
        return Phase;
    }
}
