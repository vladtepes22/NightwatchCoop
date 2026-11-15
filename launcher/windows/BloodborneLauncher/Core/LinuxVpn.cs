using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;

namespace BloodborneLauncher.Core;

/// <summary>What the Linux helper script reported (its status.txt, first word).</summary>
public enum LinuxVpnResult { Ok, NoHost, NoNmcli, NoPlugin, NmDown, Denied, NoProfile, NoConnection, Failed }

/// <summary>
/// The Nightwatch VPN under Wine / Proton (Linux, Steam Deck). Wine cannot load the TAP / DCO drivers the Windows OpenVPN
/// installer brings, so the tunnel is NetworkManager's OpenVPN on the Linux side. The launcher still enrolls (the
/// per-install keys and the 7-day renewal are the same) and writes the profile as an .ovpn that REFERENCES separate key
/// files in ~/.local/share/nightwatch-vpn; a renewal rewrites those files in place, so the imported connection keeps
/// working. A small shell script (nightwatch-vpn-linux.sh, built into the launcher and copied beside the files) does the
/// part that needs the host: adds the NetworkManager connection once, brings it up and down. The launcher starts it with
/// Wine's `start /unix`; if that does not work it shows the one command to run by hand. No kernel driver, no administrator
/// rights for the launcher; installing NetworkManager's OpenVPN plugin (once) is the only step that may need sudo.
/// The script answers in status.txt (one line: a code, then free text), because a Wine program cannot read its output.
/// </summary>
public static class LinuxVpn
{
    public const string ScriptName = "nightwatch-vpn-linux.sh";
    public const string ProfileName = "Nightwatch.ovpn";
    public const string ConnectionName = "Nightwatch";
    private const string ScriptResource = "BloodborneLauncher.Assets." + ScriptName;

    /// <summary>~/.local/share/nightwatch-vpn as (Windows, Unix) spellings; null when the home is unknown.</summary>
    public static (string Windows, string Unix)? Dir => WineHost.InHome(".local", "share", "nightwatch-vpn");

    private static string? Win(string name) => Dir is { } d ? Path.Combine(d.Windows, name) : null;
    public static string? ScriptUnixPath => Dir is { } d ? d.Unix + "/" + ScriptName : null;

    /// <summary>The script has added the NetworkManager connection (it writes setup.ok, and removes it when the connection is
    /// gone). This is what "installed" means on Linux.</summary>
    public static bool Ready => Win("setup.ok") is { } marker && File.Exists(marker);

    public static string? ProfilePath => Win(ProfileName);

    /// <summary>OpenVPN's quoting for a file name in a config line: bare when plain, else in double quotes.</summary>
    public static string Quote(string path) =>
        path.Any(c => char.IsWhiteSpace(c) || c is '"' or '\\' or '\'' or ';' or '#')
            ? "\"" + path.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"" : path;

    /// <summary>The server's profile (no keys) plus the four file references, with Unix paths: what NetworkManager and OpenVPN
    /// read on the Linux side. No secret is in it.</summary>
    public static string FileProfile(VpnInstall install, string unixDir) =>
        install.Profile.TrimEnd('\n') + "\n" +
        "ca " + Quote(unixDir + "/ca.crt") + "\n" +
        "cert " + Quote(unixDir + "/client.crt") + "\n" +
        "key " + Quote(unixDir + "/client.key") + "\n" +
        "tls-crypt-v2 " + Quote(unixDir + "/tls-crypt-v2.key") + "\n";

    public static string ReadScript()
    {
        using var stream = typeof(LinuxVpn).Assembly.GetManifestResourceStream(ScriptResource)
            ?? throw new FileNotFoundException(ScriptResource);
        using var reader = new StreamReader(stream, Encoding.UTF8);
        return reader.ReadToEnd().Replace("\r\n", "\n");
    }

    /// <summary>Writes the key, the certificates, the profile and the script into the folder. The secrets go through
    /// <see cref="NightwatchVpn.WriteUserOnly"/> (Wine maps it to a mode only the user's own group can read; the script
    /// tightens it to 600). Key first, so a renewal in the middle never leaves a new certificate with an old key for long.</summary>
    public static void WriteFiles(VpnInstall install, string key, string tls)
    {
        if (Dir is not { } dir) throw new DirectoryNotFoundException("Unix home");
        Directory.CreateDirectory(dir.Windows);
        NightwatchVpn.WriteUserOnly(Path.Combine(dir.Windows, "client.key"), key.Trim() + "\n");
        NightwatchVpn.WriteUserOnly(Path.Combine(dir.Windows, "tls-crypt-v2.key"), tls.Trim() + "\n");
        NightwatchVpn.WriteUserOnly(Path.Combine(dir.Windows, "client.crt"), install.Cert.Trim() + "\n");
        NightwatchVpn.WriteUserOnly(Path.Combine(dir.Windows, "ca.crt"), install.CaBundle.Trim() + "\n");
        NightwatchVpn.WriteUserOnly(Path.Combine(dir.Windows, ProfileName), FileProfile(install, dir.Unix));
        File.WriteAllText(Path.Combine(dir.Windows, ScriptName), ReadScript(), new UTF8Encoding(false));
    }

    /// <summary>After a background renewal: the files are rewritten in place, only when they exist (the player set it up).</summary>
    public static void RefreshFiles(VpnInstall install, string key, string tls)
    {
        if (ProfilePath is { } profile && File.Exists(profile)) WriteFiles(install, key, tls);
    }

    /// <summary>The first word of status.txt.</summary>
    public static LinuxVpnResult Parse(string? status)
    {
        var word = (status ?? "").Trim().Split(new[] { ' ', '\t', '\n' }, 2)[0];
        return word switch
        {
            "ok" => LinuxVpnResult.Ok,
            "no-nmcli" => LinuxVpnResult.NoNmcli,
            "no-plugin" => LinuxVpnResult.NoPlugin,
            "nm-down" => LinuxVpnResult.NmDown,
            "denied" => LinuxVpnResult.Denied,
            "no-profile" => LinuxVpnResult.NoProfile,
            "no-connection" => LinuxVpnResult.NoConnection,
            _ => LinuxVpnResult.Failed,
        };
    }

    /// <summary>Runs the script (setup, up, down, remove) on the Linux side with Wine's `start /unix` and waits for its answer.
    /// NoHost: it could not be started, or never answered.</summary>
    public static async Task<LinuxVpnResult> RunAsync(string verb, int waitSeconds)
    {
        if (Dir is not { } dir || Win("status.txt") is not { } status) return LinuxVpnResult.NoHost;
        try
        {
            File.Delete(status);
            var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "start.exe")) { UseShellExecute = false, CreateNoWindow = true };
            foreach (var arg in new[] { "/unix", "/bin/bash", dir.Unix + "/" + ScriptName, verb }) start.ArgumentList.Add(arg);
            using var process = Process.Start(start);
            if (process == null) return LinuxVpnResult.NoHost;
            var until = DateTime.UtcNow.AddSeconds(waitSeconds);
            while (DateTime.UtcNow < until)
            {
                await Task.Delay(250);
                try
                {
                    if (File.Exists(status) && new FileInfo(status).Length > 0) return Parse(File.ReadAllText(status));
                }
                catch (IOException) { }  // being written
            }
            return LinuxVpnResult.NoHost;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            return LinuxVpnResult.NoHost;
        }
    }

    /// <summary>Removes the connection from NetworkManager and the key files (the enrollment stays in the launcher's settings).</summary>
    public static async Task<bool> RemoveAsync()
    {
        var result = await RunAsync("remove", 20);
        if (result != LinuxVpnResult.Ok && result != LinuxVpnResult.NoConnection) return false;
        if (Dir is { } dir)
        {
            foreach (var name in new[] { "client.key", "tls-crypt-v2.key", "client.crt", "ca.crt", ProfileName, "setup.ok", "status.txt" })
            {
                try { File.Delete(Path.Combine(dir.Windows, name)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
        }
        return true;
    }

    /// <summary>The line to run by hand in a terminal.</summary>
    public static string ManualCommand(string verb) => $"bash \"{ScriptUnixPath ?? "~/.local/share/nightwatch-vpn/" + ScriptName}\" {verb}";

    /// <summary>The commands that install NetworkManager's OpenVPN plugin on SteamOS (Steam Deck; its root is read-only, so it
    /// is unlocked for the install; an OS update may remove the package again), then run the setup.</summary>
    public static string SteamOsCommands(string verb) =>
        "sudo steamos-readonly disable\n" +
        "sudo pacman-key --init && sudo pacman-key --populate\n" +
        "sudo pacman -S --needed networkmanager-openvpn\n" +
        "sudo steamos-readonly enable\n" +
        ManualCommand(verb);
}
