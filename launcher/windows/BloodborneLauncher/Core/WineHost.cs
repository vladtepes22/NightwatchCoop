using System.IO;
using System.Runtime.InteropServices;

namespace BloodborneLauncher.Core;

/// <summary>Running under Wine / Proton (Linux, Steam Deck): detection and the few host facts the launcher needs. Wine has no
/// kernel drivers, so the Windows OpenVPN installer (TAP / DCO) cannot work there: the Nightwatch VPN runs on the Linux side
/// instead (<see cref="LinuxVpn"/>).</summary>
public static class WineHost
{
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr GetModuleHandleW(string name);
    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr wine_get_unix_file_name(string dosPath);
    [DllImport("kernel32")] private static extern IntPtr GetProcessHeap();
    [DllImport("kernel32")] private static extern bool HeapFree(IntPtr heap, uint flags, IntPtr mem);

    private static readonly Lazy<bool> Detected = new(Detect);

    /// <summary>ntdll exports wine_get_version only under Wine (the documented way for a Windows program to tell). BB_WINE=0 / 1
    /// forces the answer (tests, and a Windows PC that wants to try the Linux path).</summary>
    public static bool IsWine => Environment.GetEnvironmentVariable("BB_WINE") switch { "1" => true, "0" => false, _ => Detected.Value };

    private static bool Detect()
    {
        try
        {
            var ntdll = GetModuleHandleW("ntdll.dll");
            return ntdll != IntPtr.Zero && GetProcAddress(ntdll, "wine_get_version") != IntPtr.Zero;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { return false; }
    }

    /// <summary>The Unix path of a Windows path (kernel32's wine_get_unix_file_name; the file need not exist), or null.</summary>
    public static string? UnixPath(string windowsPath)
    {
        if (!IsWine) return null;
        try
        {
            var ptr = wine_get_unix_file_name(windowsPath);
            if (ptr == IntPtr.Zero) return null;
            try { return Marshal.PtrToStringUTF8(ptr); }
            finally { HeapFree(GetProcessHeap(), 0, ptr); }
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return null; }
    }

    /// <summary>WINEHOMEDIR is the Unix home as an NT path: "\??\X:\" under Proton, "\??\Z:\home\deck" or "\??\unix\home\deck"
    /// elsewhere. Returned as a Windows path (X:\, Z:\home\deck), or null.</summary>
    public static string? WindowsHome()
    {
        var raw = Environment.GetEnvironmentVariable("WINEHOMEDIR");
        if (string.IsNullOrEmpty(raw)) return null;
        if (raw.StartsWith(@"\??\unix\", StringComparison.OrdinalIgnoreCase)) return @"Z:\" + raw[@"\??\unix\".Length..];
        return raw.StartsWith(@"\??\", StringComparison.Ordinal) ? raw[4..] : raw;
    }

    /// <summary>The Unix home directory (no trailing slash), or null.</summary>
    public static string? UnixHome() => WindowsHome() is { } home && UnixPath(home) is { Length: > 0 } unix ? unix.TrimEnd('/') : null;

    /// <summary>A path inside the Unix home, as both spellings: the Windows one the launcher writes to and the Unix one Linux
    /// programs read. Null when the home is unknown.</summary>
    public static (string Windows, string Unix)? InHome(params string[] parts)
    {
        if (WindowsHome() is not { } winHome || UnixHome() is not { } unixHome) return null;
        return (Path.Combine(new[] { winHome }.Concat(parts).ToArray()), unixHome + "/" + string.Join("/", parts));
    }
}
