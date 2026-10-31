using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core;

/// <summary>Where the enrollment service is: host, port and the SHA-256 (hex) of its certificate's public key.</summary>
public sealed record VpnEndpoint(string Host, int Port, string Pin)
{
    // Records print their fields: this one must not end up in a message or a log with the host in it.
    public override string ToString() => "VpnEndpoint";
}

public enum VpnFailure { Unreachable, PinMismatch, Blocked, RateLimited, Refused, BadReply, Protect }

/// <summary>A failure the page can say in words. Holds no address, key or server text.</summary>
public sealed class VpnException : Exception
{
    public VpnFailure Failure { get; }
    public int RetryAfterSeconds { get; }
    public VpnException(VpnFailure failure, int retryAfterSeconds = 0) : base("Nightwatch VPN: " + failure)
    {
        Failure = failure;
        RetryAfterSeconds = retryAfterSeconds;
    }
}

/// <summary>One enrolled install, as saved. The private key and the tls-crypt-v2 key are DPAPI-wrapped (Secret).</summary>
public sealed class VpnInstall
{
    public string InstallId { get; set; } = "";
    public string Cert { get; set; } = "";
    public string CaBundle { get; set; } = "";
    public string Profile { get; set; } = "";
    public long Expires { get; set; }
    public long RenewAfter { get; set; }
    public string KeyProtected { get; set; } = "";
    public string TlsProtected { get; set; } = "";
}

/// <summary>
/// The Nightwatch VPN's per-install keys (engine/docs/party/nightwatch_vpn.md). The launcher enrolls once with a key made
/// here that never leaves this PC, renews in the background and hands the finished profile to OpenVPN GUI, which runs the
/// tunnel through OpenVPN's interactive service (no administrator rights here). No request is made before the player's
/// first Connect. The host and the pin come from the build (BloodborneLauncher.csproj, not in the repo).
/// </summary>
public static class NightwatchVpn
{
    public const string ConfigName = "nightwatch";
    private const int MaxReply = 1 << 20;
    private static readonly Regex InstallIdShape = new("^[0-9a-f]{32}$", RegexOptions.Compiled);
    private static long _waitUntil;  // a 429's Retry-After: nothing is sent before it (three enrollments a day are the budget)

    /// <summary>The service this build ships with; null when it was built without one (the page then hides the VPN).</summary>
    public static VpnEndpoint? Shipped { get; } = ReadShipped();

    private static VpnEndpoint? ReadShipped()
    {
        string? host = null, pin = null;
        foreach (var meta in typeof(NightwatchVpn).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>())
        {
            if (meta.Key == "NwVpnHost") host = meta.Value;
            if (meta.Key == "NwVpnPin") pin = meta.Value;
        }
        return string.IsNullOrWhiteSpace(host) || pin is null || !Regex.IsMatch(pin, "^[0-9a-fA-F]{64}$")
            ? null : new VpnEndpoint(host, 8443, pin.ToLowerInvariant());
    }

    private static string InstallFile(string dir) => Path.Combine(dir, "nightwatch-vpn.json");

    public static long Now => DateTimeOffset.UtcNow.ToUnixTimeSeconds();

    public static VpnInstall? Load(string dir)
    {
        try
        {
            var install = JsonSerializer.Deserialize<VpnInstall>(File.ReadAllText(InstallFile(dir)));
            return install is { InstallId.Length: > 0 } ? install : null;
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>One file replaced in one step, so a crash never mixes an old key with a new certificate.</summary>
    public static void Save(string dir, VpnInstall install)
    {
        Directory.CreateDirectory(dir);
        var temp = InstallFile(dir) + ".new";
        File.WriteAllText(temp, JsonSerializer.Serialize(install));
        File.Move(temp, InstallFile(dir), overwrite: true);
    }

    private static string Protect(string plain) =>
        Secret.Protect(plain) is { Length: > 0 } wrapped ? wrapped : throw new VpnException(VpnFailure.Protect);

    private static string Unprotect(string wrapped) =>
        Secret.IsProtected(wrapped) && Secret.Unprotect(wrapped) is { Length: > 0 } plain ? plain : throw new VpnException(VpnFailure.Protect);

    private static bool CanRead(VpnInstall install) =>
        Secret.IsProtected(install.KeyProtected) && Secret.IsProtected(install.TlsProtected) &&
        Secret.Unprotect(install.KeyProtected).Length > 0 && Secret.Unprotect(install.TlsProtected).Length > 0;

    /// <summary>The bytes of a certificate's SubjectPublicKeyInfo, hashed: the value the pin is compared with.</summary>
    public static string SpkiHex(X509Certificate2 cert) =>
        Convert.ToHexString(SHA256.HashData(cert.PublicKey.ExportSubjectPublicKeyInfo())).ToLowerInvariant();

    public static bool PinMatches(X509Certificate2? cert, string pin) =>
        cert != null && CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(SpkiHex(cert)), Encoding.ASCII.GetBytes(pin));

    /// <summary>What the old key signs to renew: UTF-8, \n between the lines, no trailing newline.</summary>
    public static string RenewMessage(string installId, string csr, long timestamp) =>
        "nwsign-renew-v1\n" + installId + "\n" + Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(csr))).ToLowerInvariant() + "\n" + timestamp;

    /// <summary>The finished .ovpn: holds both secrets, so it is only ever written by <see cref="WriteUserOnly"/>.</summary>
    public static string BuildProfile(VpnInstall install, string key, string tls) =>
        install.Profile.TrimEnd('\n') + "\n<ca>\n" + install.CaBundle.Trim() + "\n</ca>\n<cert>\n" + install.Cert.Trim() +
        "\n</cert>\n<key>\n" + key.Trim() + "\n</key>\n<tls-crypt-v2>\n" + tls.Trim() + "\n</tls-crypt-v2>\n";

    private static (ECDsa Key, string Csr) NewKey()
    {
        var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        return (key, new CertificateRequest("CN=nwvpn", key, HashAlgorithmName.SHA256).CreateSigningRequestPem());
    }

    /// <summary>POST over TLS, trusting only the pin: the handshake fails on another key, before anything is sent.
    /// No proxy and no redirects. Every failure becomes a VpnException without the address in its text.</summary>
    /// <summary>This PC's clock against the server's, from its last reply this run.</summary>
    public static TimeSpan? ClockSkew { get; private set; }

    private static async Task<JsonElement> PostAsync(VpnEndpoint endpoint, string path, object body)
    {
        if (Now < Interlocked.Read(ref _waitUntil)) throw new VpnException(VpnFailure.RateLimited, (int)(_waitUntil - Now));
        var pinMismatch = false;
        using var handler = new HttpClientHandler
        {
            UseProxy = false,
            AllowAutoRedirect = false,
            CheckCertificateRevocationList = false,
            SslProtocols = System.Security.Authentication.SslProtocols.Tls12 | System.Security.Authentication.SslProtocols.Tls13,
            ServerCertificateCustomValidationCallback = (_, cert, _, _) =>
            {
                if (PinMatches(cert, endpoint.Pin)) return true;
                pinMismatch = true;
                return false;
            },
        };
        using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(20) };
        var content = new StringContent(JsonSerializer.Serialize(body), Encoding.UTF8, "application/json");
        try
        {
            using var reply = await http.PostAsync($"https://{endpoint.Host}:{endpoint.Port}{path}", content);
            if (reply.Headers.Date is { } serverTime) ClockSkew = DateTimeOffset.UtcNow - serverTime;
            var status = (int)reply.StatusCode;
            if (status == 429)
            {
                var wait = (int?)reply.Headers.RetryAfter?.Delta?.TotalSeconds ?? 3600;
                Interlocked.Exchange(ref _waitUntil, Now + wait);
                throw new VpnException(VpnFailure.RateLimited, wait);
            }
            if (status == 401) throw new VpnException(VpnFailure.Refused);
            if (status == 403) throw new VpnException(VpnFailure.Blocked);
            if (status != 200 || reply.Content.Headers.ContentLength > MaxReply) throw new VpnException(VpnFailure.BadReply);
            var text = await reply.Content.ReadAsStringAsync();
            if (text.Length > MaxReply) throw new VpnException(VpnFailure.BadReply);
            using var parsed = JsonDocument.Parse(text);
            return parsed.RootElement.Clone();
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException)
        {
            throw new VpnException(pinMismatch ? VpnFailure.PinMismatch : VpnFailure.Unreachable);
        }
        catch (JsonException) { throw new VpnException(VpnFailure.BadReply); }
    }

    private static VpnInstall FromReply(JsonElement reply, ECDsa key)
    {
        try
        {
            var install = new VpnInstall
            {
                InstallId = reply.GetProperty("install_id").GetString() ?? "",
                Cert = reply.GetProperty("cert").GetString() ?? "",
                CaBundle = reply.GetProperty("ca_bundle").GetString() ?? "",
                Profile = reply.GetProperty("profile").GetString() ?? "",
                Expires = reply.GetProperty("expires").GetInt64(),
                RenewAfter = reply.GetProperty("renew_after").GetInt64(),
            };
            var tls = reply.GetProperty("tls_crypt_v2_client_key").GetString() ?? "";
            if (!InstallIdShape.IsMatch(install.InstallId) || tls.Length == 0 || install.Cert.Length == 0 ||
                install.CaBundle.Length == 0 || install.Profile.Length == 0)
                throw new VpnException(VpnFailure.BadReply);
            install.KeyProtected = Protect(key.ExportECPrivateKeyPem());
            install.TlsProtected = Protect(tls);
            return install;
        }
        catch (Exception e) when (e is KeyNotFoundException or InvalidOperationException or FormatException)
        {
            throw new VpnException(VpnFailure.BadReply);
        }
    }

    public static async Task<VpnInstall> EnrollAsync(VpnEndpoint endpoint, string dir)
    {
        var (key, csr) = NewKey();
        using (key)
        {
            var install = FromReply(await PostAsync(endpoint, "/v1/enroll", new Dictionary<string, object> { ["csr"] = csr }), key);
            Save(dir, install);
            return install;
        }
    }

    /// <summary>A new key, signed for by the old one. A 401 means the server no longer knows the install: enroll again,
    /// once. Any other failure leaves the saved install alone (it works until it expires).</summary>
    public static async Task<VpnInstall> RenewAsync(VpnEndpoint endpoint, string dir, VpnInstall old)
    {
        var (key, csr) = NewKey();
        using (key)
        using (var oldKey = ECDsa.Create())
        {
            oldKey.ImportFromPem(Unprotect(old.KeyProtected));
            var timestamp = Now;
            var signature = oldKey.SignData(Encoding.UTF8.GetBytes(RenewMessage(old.InstallId, csr, timestamp)),
                HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence);
            try
            {
                var reply = await PostAsync(endpoint, "/v1/renew", new Dictionary<string, object>
                {
                    ["install_id"] = old.InstallId, ["csr"] = csr, ["timestamp"] = timestamp,
                    ["signature"] = Convert.ToBase64String(signature), ["cert"] = old.Cert,
                });
                var install = FromReply(reply, key);
                Save(dir, install);
                return install;
            }
            catch (VpnException e) when (e.Failure == VpnFailure.Refused)
            {
                return await EnrollAsync(endpoint, dir);
            }
        }
    }

    /// <summary>At launcher start, and only for an install that exists: renews once its renew time has passed. Quiet:
    /// a failure keeps the old profile; Connect reports it if it still matters.</summary>
    public static async Task RenewIfDueAsync(VpnEndpoint endpoint, string dir)
    {
        if (Load(dir) is not { } install || Now < install.RenewAfter) return;
        try { await RenewAsync(endpoint, dir, install); }
        catch (Exception e) when (e is VpnException or CryptographicException or IOException or UnauthorizedAccessException) { }
    }

    /// <summary>The install to connect with: enrolled on first use, renewed when due. A failed renewal is only fatal once
    /// the certificate has expired; a blocked install is always told.</summary>
    public static async Task<VpnInstall> ReadyAsync(VpnEndpoint endpoint, string dir)
    {
        // Keys another Windows user wrapped (a settings folder copied over) cannot be used here.
        if (Load(dir) is not { } install || !CanRead(install)) return await EnrollAsync(endpoint, dir);
        if (Now < install.RenewAfter) return install;
        try { return await RenewAsync(endpoint, dir, install); }
        catch (VpnException e) when (e.Failure != VpnFailure.Blocked && Now < install.Expires) { return install; }
    }

    // ---- OpenVPN GUI: it runs the tunnel; its --command interface is documented in its README ----

    public static string? GuiExe()
    {
        var roots = new List<string?>();
        try
        {
            using var key = Microsoft.Win32.RegistryKey.OpenBaseKey(Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Registry64)
                .OpenSubKey(@"SOFTWARE\OpenVPN");
            roots.Add(key?.GetValue("") as string);
        }
        catch (Exception e) when (e is System.Security.SecurityException or IOException or UnauthorizedAccessException) { }
        roots.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "OpenVPN"));
        return roots.Where(r => !string.IsNullOrEmpty(r)).Select(r => Path.Combine(r!, "bin", "openvpn-gui.exe")).FirstOrDefault(File.Exists);
    }

    /// <summary>The folder OpenVPN GUI reads a user's profiles from (its registry value, else its default).</summary>
    public static string GuiConfigDir()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\OpenVPN-GUI");
            if (key?.GetValue("config_dir") is string { Length: > 0 } dir) return Environment.ExpandEnvironmentVariables(dir);
        }
        catch (Exception e) when (e is System.Security.SecurityException or IOException or UnauthorizedAccessException) { }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "OpenVPN", "config");
    }

    public static string ProfilePath => Path.Combine(GuiConfigDir(), ConfigName + ".ovpn");

    /// <summary>A new file only this Windows user can read: the permissions are set when it is created, before the text.</summary>
    public static void WriteUserOnly(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.Delete(path);
        var security = new FileSecurity();
        security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        security.AddAccessRule(new FileSystemAccessRule(WindowsIdentity.GetCurrent().User!, FileSystemRights.FullControl, AccessControlType.Allow));
        using var stream = new FileInfo(path).Create(FileMode.CreateNew, FileSystemRights.FullControl, FileShare.None, 4096, FileOptions.None, security);
        stream.Write(new UTF8Encoding(false).GetBytes(text));
    }

    /// <summary>Writes the profile, then asks OpenVPN GUI to connect. The file stays (user-only) while the GUI needs it for
    /// a reconnect; <see cref="Disconnect"/> and the launcher's next start without a tunnel delete it.</summary>
    public static void Connect(VpnInstall install, string guiExe)
    {
        lock (GuiLock)
        {
            if (_closing) return;
            WriteUserOnly(ProfilePath, BuildProfile(install, Unprotect(install.KeyProtected), Unprotect(install.TlsProtected)));
            Gui(guiExe, "--command", "rescan").WaitForExit(3000);  // a running GUI has to see the file before connect
            if (!_closing) Gui(guiExe, "--command", "connect", ConfigName);
        }
    }

    public static void Disconnect(string guiExe)
    {
        Gui(guiExe, "--command", "disconnect", ConfigName);
        DeleteProfile();
    }

    /// <summary>Closes OpenVPN GUI, so the uninstaller finds no running program.</summary>
    public static void ExitGui(string guiExe) => Gui(guiExe, "--command", "exit");

    public static void DeleteProfile()
    {
        try { File.Delete(ProfilePath); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    // ---- Who owns the connection: one launcher at a time, and it ends with that launcher ----

    private static readonly object GuiLock = new();
    private static volatile bool _closing;
    private static Mutex? _owner;
    private const string OwnerMutex = @"Local\NightwatchVpnOwner";

    public static bool OwnedHere => _owner != null;

    /// <summary>Takes the connection for this launcher; false while another launcher holds it. One a crashed launcher left
    /// is free to take. Call on the UI thread: a mutex belongs to the thread that took it.</summary>
    public static bool TryOwn()
    {
        if (_owner != null) return true;
        var mutex = new Mutex(false, OwnerMutex);
        try
        {
            if (!mutex.WaitOne(0))
            {
                mutex.Dispose();
                return false;
            }
        }
        catch (AbandonedMutexException) { }
        _owner = mutex;
        return true;
    }

    public static void ReleaseOwnership()
    {
        _owner?.ReleaseMutex();
        _owner?.Dispose();
        _owner = null;
    }

    /// <summary>The launcher is closing (the window, an update's restart, Windows shutting down): ends this launcher's
    /// connection, and one still being made, and waits a bounded time for the tunnel to go. Only the Nightwatch profile, and
    /// the GUI keeps running: it may hold the player's other VPNs. Returns whether the tunnel is confirmed gone.</summary>
    public static bool DisconnectOnExit()
    {
        _closing = true;
        if (!OwnedHere) return true;
        var locked = Monitor.TryEnter(GuiLock, TimeSpan.FromSeconds(4));
        try
        {
            if (GuiExe() is { } gui)
            {
                Gui(gui, "--command", "disconnect", ConfigName);
                var until = DateTime.UtcNow.AddSeconds(6);
                while (VpnLobby.TunnelIp(NetworkRoute.Enumerate()) != null)
                {
                    if (DateTime.UtcNow >= until) return false;
                    Thread.Sleep(250);
                }
            }
            DeleteProfile();
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException or System.ComponentModel.Win32Exception) { return false; }
        finally { if (locked) Monitor.Exit(GuiLock); }
    }

    private static Process Gui(string exe, params string[] args)
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true };
        foreach (var arg in args) start.ArgumentList.Add(arg);
        return Process.Start(start)!;
    }
}
