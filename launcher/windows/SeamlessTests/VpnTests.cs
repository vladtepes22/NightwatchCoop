using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using BloodborneLauncher.Core;

/// <summary>The Nightwatch VPN client (NightwatchVpn, VpnLobby) with a fake pinned service and a fake lobby on loopback.
/// No real server, no OpenVPN; synthetic names and keys only.</summary>
static class VpnTests
{
    private static void Check(bool ok, string what)
    {
        if (!ok) throw new InvalidOperationException("VPN: " + what);
    }

    private static void Fails(VpnFailure expected, Func<Task> action, string what)
    {
        try { action().GetAwaiter().GetResult(); }
        catch (VpnException e)
        {
            Check(e.Failure == expected, what + ": got " + e.Failure);
            Check(!e.Message.Contains("127.0.0.1"), what + ": the address is in the message");
            return;
        }
        throw new InvalidOperationException("VPN: " + what + ": no failure");
    }

    /// <summary>VpnIdle: an idle tunnel goes after the set minutes, comes back when needed, and never against the player's choice.</summary>
    private static void IdleRules()
    {
        var t0 = new DateTime(2026, 11, 1, 12, 0, 0, DateTimeKind.Utc);
        var idle = new VpnIdleState(Enabled: true, Wanted: true, Owned: true, Up: true, Busy: false, GameRunning: false,
            WindowActive: false, InactiveSince: t0, IdleAfter: TimeSpan.FromMinutes(10), Now: t0.AddMinutes(10));
        var tick = VpnIdleTrigger.Tick;
        Check(VpnIdle.Decide(idle, tick) == VpnIdleAction.Disconnect, "idle 10 min: disconnect");
        Check(VpnIdle.Decide(idle with { Now = t0.AddMinutes(9) }, tick) == VpnIdleAction.None, "idle 9 min: keep");
        Check(VpnIdle.Decide(idle with { GameRunning = true }, tick) == VpnIdleAction.None, "never while the game runs");
        Check(VpnIdle.Decide(idle with { WindowActive = true }, tick) == VpnIdleAction.None, "never while the window is in front");
        Check(VpnIdle.Decide(idle with { InactiveSince = null }, tick) == VpnIdleAction.None, "no idle start: keep");
        Check(VpnIdle.Decide(idle with { Owned = false }, tick) == VpnIdleAction.None, "another launcher's tunnel: keep");
        Check(VpnIdle.Decide(idle with { Busy = true }, tick) == VpnIdleAction.None, "a change under way: wait");
        Check(VpnIdle.Decide(idle with { Enabled = false }, tick) == VpnIdleAction.None, "setting off: keep");
        Check(VpnIdle.Decide(idle with { Wanted = false }, tick) == VpnIdleAction.None, "not this launcher's wish: keep");
        Check(VpnIdle.Decide(idle, VpnIdleTrigger.WindowActivated) == VpnIdleAction.None, "an up tunnel is not dropped on a trigger");
        var down = idle with { Up = false, WindowActive = true, InactiveSince = null };
        foreach (var trigger in new[] { VpnIdleTrigger.WindowActivated, VpnIdleTrigger.CoopShown, VpnIdleTrigger.Play })
            Check(VpnIdle.Decide(down, trigger) == VpnIdleAction.Connect, "back on demand: " + trigger);
        Check(VpnIdle.Decide(down, tick) == VpnIdleAction.None, "a tick never connects");
        Check(VpnIdle.Decide(down with { Wanted = false }, VpnIdleTrigger.Play) == VpnIdleAction.None, "never connects a player who did not");
        Check(VpnIdle.Decide(down with { Enabled = false }, VpnIdleTrigger.Play) == VpnIdleAction.None, "setting off: no auto connect");
        Check(VpnIdle.IdleAfter("25") == TimeSpan.FromMinutes(25) && VpnIdle.IdleAfter("0") == VpnIdle.DefaultIdle &&
              VpnIdle.IdleAfter("x") == VpnIdle.DefaultIdle && VpnIdle.IdleAfter(null) == VpnIdle.DefaultIdle &&
              VpnIdle.IdleAfter("999") == VpnIdle.DefaultIdle, "idle minutes setting");
    }

    private static void SetupAndConnectionRules()
    {
        var args = OpenVpnSetup.Arguments(@"C:\p q\o.msi", @"C:\l.log");
        Check(args.Contains("\"C:\\p q\\o.msi\"") && args.Contains("/qn") && args.Contains("/norestart") && args.Contains("ADDLOCAL="), "msiexec arguments");
        Check(!OpenVpnSetup.Features.Contains("OnLogon") && OpenVpnSetup.Features.Contains("OpenVPN.GUI") && OpenVpnSetup.Features.Contains("OpenVPN.Service"), "Nothing starts with Windows, and the GUI and its service are in");
        foreach (var (code, want) in new[] { (0, OpenVpnSetupResult.Installed), (3010, OpenVpnSetupResult.Installed), (1641, OpenVpnSetupResult.Installed), (1602, OpenVpnSetupResult.Declined), (1223, OpenVpnSetupResult.Declined), (1618, OpenVpnSetupResult.Busy), (1603, OpenVpnSetupResult.Failed) })
            Check(OpenVpnSetup.ResultOf(code) == want, "msiexec exit " + code);
        using (var wrong = new MemoryStream(new byte[] { 1, 2, 3 })) Check(!OpenVpnSetup.HashMatches(wrong), "A changed installer passed the hash");

        var t = new DateTime(2026, 10, 30, 12, 0, 0, DateTimeKind.Utc);
        var c = new VpnConnection();
        c.BeginConnect(t);
        Check(c.Busy && c.Observe(false, t.AddSeconds(30)) == VpnPhase.Connecting, "Connecting holds without a tunnel");
        Check(c.Observe(true, t.AddSeconds(31)) == VpnPhase.On && !c.Busy && !c.TimedOut, "The tunnel confirms Connect");
        c.BeginDisconnect(t);
        Check(c.Observe(true, t.AddSeconds(5)) == VpnPhase.Disconnecting, "Disconnecting holds while the tunnel is up");
        Check(c.Observe(false, t.AddSeconds(6)) == VpnPhase.Off && !c.TimedOut, "The tunnel's end confirms Disconnect");
        c.BeginConnect(t);
        Check(c.Observe(false, t + VpnConnection.ConnectLimit) == VpnPhase.Off && c.TimedOut, "A connect that never comes up times out");
        c.BeginDisconnect(t);
        Check(c.Observe(true, t + VpnConnection.DisconnectLimit) == VpnPhase.On && c.TimedOut, "A disconnect that never ends times out, still on");
        c.BeginConnect(t);
        c.Abort(false);
        Check(c.Phase == VpnPhase.Off && !c.TimedOut, "A failure before the tunnel is no timeout");

        // One owner: a launcher that does not hold the connection never ends it, and ownership frees for the next.
        if (!NightwatchVpn.TryOwn())
        {
            // The lock is per Windows session: a real launcher open on this PC holds it, and must not be disturbed.
            Console.WriteLine("SKIP: VPN one-owner check (a launcher on this PC owns the connection)");
            return;
        }
        Check(NightwatchVpn.OwnedHere, "The first launcher owns the connection");
        var second = true;
        var other = new Thread(() =>
        {
            using var mutex = new Mutex(false, @"Local\NightwatchVpnOwner");
            second = mutex.WaitOne(0);
            if (second) mutex.ReleaseMutex();
        });
        other.Start();
        other.Join();
        Check(!second, "A second holder got the connection");
        NightwatchVpn.ReleaseOwnership();
        Check(!NightwatchVpn.OwnedHere, "Ownership was not released");
    }

    public static void Run()
    {
        IdleRules();
        SetupAndConnectionRules();
        Formats();
        Lobby();
        Store();
        PendingJoin();
        Service();
    }

    private static void Formats()
    {
        const string csr = "-----BEGIN CERTIFICATE REQUEST-----\nabc\n-----END CERTIFICATE REQUEST-----\n";
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(csr))).ToLowerInvariant();
        Check(NightwatchVpn.RenewMessage("id", csr, 1700000000) == "nwsign-renew-v1\nid\n" + hash + "\n1700000000", "renew message");
        var install = new VpnInstall { Profile = "client\ndev tun\n\n", CaBundle = "CA\n", Cert = "\nCERT" };
        Check(NightwatchVpn.BuildProfile(install, "KEY\n", "TLS\n") ==
              "client\ndev tun\n<ca>\nCA\n</ca>\n<cert>\nCERT\n</cert>\n<key>\nKEY\n</key>\n<tls-crypt-v2>\nTLS\n</tls-crypt-v2>\n", "profile blocks");
        Check(NightwatchVpn.Shipped == null || NightwatchVpn.Shipped.Pin.Length == 64, "shipped pin shape");
        Check(new VpnEndpoint("host.invalid", 1, "p").ToString() == "VpnEndpoint", "the endpoint prints its host");
    }

    private static byte[] Entry(byte[] ip, int port, int flags, int players, int max, byte[] name, byte[] build)
    {
        var e = new List<byte>(ip) { (byte)(port >> 8), (byte)port, (byte)flags, (byte)players, (byte)max, (byte)name.Length };
        e.AddRange(name);
        e.Add((byte)build.Length);
        e.AddRange(build);
        return e.ToArray();
    }

    private static byte[] Reply(int total, int offset, params byte[][] entries)
    {
        var r = new List<byte> { 0xfa, (byte)'L', 1, 3, (byte)(total >> 8), (byte)total, (byte)(offset >> 8), (byte)offset, (byte)entries.Length };
        foreach (var e in entries) r.AddRange(e);
        return r.ToArray();
    }

    private static readonly byte[] TunnelHost = { 10, 8, 0, 7 };
    private static readonly byte[] Build = Encoding.ASCII.GetBytes("nightwatch-coop-0.2.6-abc");

    private static void Lobby()
    {
        var name = Encoding.UTF8.GetBytes("Alice");
        var good = VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 4, name, Build)));
        Check(good is { Total: 1, Offset: 0 } && good.Value.Games[0] is { Name: "Alice", Players: 1, Max: 4, Password: true, Own: false, Port: 9307 }, "good reply");
        Check(good!.Value.Games[0].JoinCode == "10.8.0.7:9307", "join address");
        Check(!good.Value.Games[0].ToString().Contains("10.8"), "a game prints its address");
        var utf8Name = VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 4, Encoding.UTF8.GetBytes("Ж\u0007‮星 x"), Build)));
        Check(utf8Name!.Value.Games[0].Name == "Ж星 x", "control and bidi characters stay out of the page");

        // Everything malformed is refused whole.
        Check(VpnLobby.ParseReply(new byte[] { 0xfa, (byte)'L', 1, 3 }) == null, "short");
        Check(VpnLobby.ParseReply(Reply(0, 0).Select((b, i) => i == 2 ? (byte)2 : b).ToArray()) == null, "version");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 4, 1, 4, name, Build))) == null, "unknown flag");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 80, 1, 1, 4, name, Build))) == null, "low port");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 5, 4, name, Build))) == null, "too many players");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 9, name, Build))) == null, "too many slots");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 4, new byte[] { 0xc3, 0x28 }, Build))) == null, "bad UTF-8");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 4, new byte[] { 7 }, Build))) == null, "a name of control characters");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 4, name, new byte[] { 7 }))) == null, "bad build");
        Check(VpnLobby.ParseReply(Reply(1, 0, Entry(TunnelHost, 9307, 1, 1, 4, name, Build)).Append((byte)0).ToArray()) == null, "trailing byte");

        Check(!VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.0.1")) && VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.0.2")) &&
              VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.0.254")) && VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.0.255")) &&
              VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.1.2")) && VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.3.254")) &&
              !VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.0.0")) && !VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.3.255")) &&
              !VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.8.4.2")) && !VpnLobby.IsTunnelAddress(NetworkRoute.Parse("10.9.0.2")),
              "tunnel range (10.8.0.0/22)");
        Check(VpnLobby.TunnelIp(new[] { new NetAdapter("lan", NetworkRoute.Parse("192.168.1.2")) }) == null, "no tunnel, nothing to bind");

        // Your own game, a game without a password and an address outside the tunnel are not rows.
        var parsed = VpnLobby.ParseReply(Reply(4, 0,
            Entry(new byte[] { 10, 8, 0, 9 }, 9307, 1, 1, 4, Encoding.UTF8.GetBytes("zed"), Build),
            Entry(TunnelHost, 9307, 3, 1, 4, Encoding.UTF8.GetBytes("own"), Build),
            Entry(new byte[] { 8, 8, 8, 8 }, 9307, 1, 1, 4, Encoding.UTF8.GetBytes("outside"), Build),
            Entry(new byte[] { 10, 8, 0, 3 }, 9307, 1, 1, 4, Encoding.UTF8.GetBytes("Ann"), Build)))!.Value.Games;
        Check(string.Join(",", VpnLobby.Rows(parsed).Select(g => g.Name)) == "Ann,zed", "rows");
        Check(VpnLobby.VersionOf("nightwatch-coop-0.2.6-abcdef") == "0.2.6" && VpnLobby.VersionOf("0.7.7 (abc)") == "0.7.7 (abc)", "version text");

        // Pages over a real socket, from a fake lobby on loopback.
        using var fake = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        var target = (IPEndPoint)fake.Client.LocalEndPoint!;
        var offsets = new List<int>();
        var server = Task.Run(() =>
        {
            for (var page = 0; page < 2; page++)
            {
                var from = new IPEndPoint(IPAddress.Any, 0);
                var request = fake.Receive(ref from);
                offsets.Add(request[4] << 8 | request[5]);
                var reply = page == 0
                    ? Reply(2, 0, Entry(TunnelHost, 9307, 1, 1, 4, name, Build))
                    : Reply(2, 1, Entry(new byte[] { 10, 8, 0, 8 }, 9307, 1, 2, 4, Encoding.UTF8.GetBytes("Bob"), Build));
                fake.Send(reply, reply.Length, from);
            }
        });
        var games = VpnLobby.Fetch(target, null);
        Check(server.Wait(5000), "the fake lobby was not asked twice");
        Check(games is { Count: 2 } && games[1].Name == "Bob" && offsets.SequenceEqual(new[] { 0, 1 }), "paging");
        using var silent = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        Check(VpnLobby.Fetch((IPEndPoint)silent.Client.LocalEndPoint!, null, 100) == null, "a silent lobby is no list");
    }

    private static void Store()
    {
        var dir = Directory.CreateTempSubdirectory("nwvpn-t-").FullName;
        try
        {
            var profile = Path.Combine(dir, "sub", "x.ovpn");
            NightwatchVpn.WriteUserOnly(profile, "SECRET");
            NightwatchVpn.WriteUserOnly(profile, "SECRET2");
            Check(File.ReadAllText(profile) == "SECRET2", "profile text");
            var security = new FileInfo(profile).GetAccessControl();
            var rules = security.GetAccessRules(true, true, typeof(SecurityIdentifier)).Cast<FileSystemAccessRule>().ToList();
            Check(security.AreAccessRulesProtected && rules.Count == 1 && rules[0].IdentityReference == WindowsIdentity.GetCurrent().User,
                "the profile is readable by someone else");
        }
        finally { Directory.Delete(dir, true); }
    }

    private static void PendingJoin()
    {
        var s = new LauncherSettings();
        s.Set("party_mode", "join");
        s.Set("party_name", "Tester");
        s.Set("party_code", "");
        Check(SeamlessSettings.Validate(s) != null, "a join needs a host");
        SeamlessSettings.PendingJoin = ("10.8.0.7:9307", "pw-lobby");
        try
        {
            Check(SeamlessSettings.Validate(s) == null, "a lobby join is valid: " + SeamlessSettings.Validate(s));
            var env = new Dictionary<string, string>();
            SeamlessSettings.Apply(s, env);
            Check(env["BB_PARTY"] == "join" && env["BB_PARTY_CODE"] == "10.8.0.7:9307" && env["BB_PARTY_PASSWORD"] == "pw-lobby", "lobby join environment");
            Check(s.Str("party_code").Length == 0 && s.JoinPassword("10.8.0.7:9307").Length == 0, "the lobby join reached the settings");
        }
        finally { SeamlessSettings.PendingJoin = null; }
        var after = new Dictionary<string, string>();
        SeamlessSettings.Apply(s, after);
        Check(!after.Values.Any(v => v.Contains("10.8.0.7") || v.Contains("pw-lobby")), "the lobby join outlived its start");
    }

    // ---- a fake nwsign on loopback: one request per connection ----

    private sealed class FakeService : IDisposable
    {
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly X509Certificate2 _cert;
        public readonly List<(string Path, string Body)> Requests = new();
        public Func<string, string, (int Status, string Body, string? RetryAfter)> Handle = (_, _) => (500, "{}", null);
        public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;
        public string Pin => NightwatchVpn.SpkiHex(_cert);

        public FakeService()
        {
            using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            using var self = new CertificateRequest("CN=fake", key, HashAlgorithmName.SHA256).CreateSelfSigned(DateTimeOffset.Now.AddDays(-1), DateTimeOffset.Now.AddDays(1));
            _cert = new X509Certificate2(self.Export(X509ContentType.Pfx));
            _listener.Start();
            _ = Task.Run(Serve);
        }

        private async Task Serve()
        {
            while (true)
            {
                TcpClient client;
                try { client = await _listener.AcceptTcpClientAsync(); }
                catch (Exception e) when (e is SocketException or ObjectDisposedException) { return; }
                using (client)
                {
                    try { await One(client); }
                    catch (Exception e) when (e is IOException or System.Security.Authentication.AuthenticationException) { }
                }
            }
        }

        private async Task One(TcpClient client)
        {
            await using var tls = new SslStream(client.GetStream());
            await tls.AuthenticateAsServerAsync(_cert);
            var bytes = new List<byte>();
            var buffer = new byte[4096];
            int end;
            while ((end = Encoding.ASCII.GetString(bytes.ToArray()).IndexOf("\r\n\r\n", StringComparison.Ordinal)) < 0)
            {
                var n = await tls.ReadAsync(buffer);
                if (n == 0) return;
                bytes.AddRange(buffer.Take(n));
            }
            var head = Encoding.ASCII.GetString(bytes.ToArray(), 0, end);
            var lines = head.Split("\r\n");
            var length = int.Parse(lines.First(l => l.StartsWith("Content-Length:", StringComparison.OrdinalIgnoreCase)).Split(':')[1]);
            while (bytes.Count < end + 4 + length)
            {
                var n = await tls.ReadAsync(buffer);
                if (n == 0) return;
                bytes.AddRange(buffer.Take(n));
            }
            var path = lines[0].Split(' ')[1];
            var body = Encoding.UTF8.GetString(bytes.ToArray(), end + 4, length);
            lock (Requests) Requests.Add((path, body));
            var (status, reply, retry) = Handle(path, body);
            var data = Encoding.UTF8.GetBytes(reply);
            var header = $"HTTP/1.1 {status} X\r\nContent-Type: application/json\r\nContent-Length: {data.Length}\r\nConnection: close\r\n" +
                         (retry != null ? $"Retry-After: {retry}\r\n" : "") + "\r\n";
            await tls.WriteAsync(Encoding.ASCII.GetBytes(header));
            await tls.WriteAsync(data);
        }

        public void Dispose() { _listener.Stop(); _cert.Dispose(); }
    }

    private static string Issued(string id, string cert, long renewAfter, long expires) => JsonSerializer.Serialize(new Dictionary<string, object>
    {
        ["install_id"] = id, ["cert"] = cert, ["ca_bundle"] = "CA", ["tls_crypt_v2_client_key"] = "TLS-SECRET-" + id, ["expires"] = expires,
        ["renew_after"] = renewAfter, ["profile"] = "client\n",
    });

    private static void Service()
    {
        var dir = Directory.CreateTempSubdirectory("nwvpn-t-").FullName;
        using var fake = new FakeService();
        try
        {
            var id1 = new string('a', 32);
            var id2 = new string('b', 32);
            var endpoint = new VpnEndpoint("127.0.0.1", fake.Port, fake.Pin);

            // Another key: nothing is sent, nothing is saved.
            Fails(VpnFailure.PinMismatch, () => NightwatchVpn.EnrollAsync(endpoint with { Pin = new string('0', 64) }, dir), "wrong pin");
            Check(fake.Requests.Count == 0 && NightwatchVpn.Load(dir) == null, "a request crossed a wrong pin");

            // Enroll: a P-256 CSR goes out, the secrets are saved wrapped.
            fake.Handle = (_, _) => (200, Issued(id1, "CERT1", NightwatchVpn.Now - 10, NightwatchVpn.Now + 1000), null);
            var first = NightwatchVpn.EnrollAsync(endpoint, dir).GetAwaiter().GetResult();
            Check(fake.Requests[0].Path == "/v1/enroll", "enroll path");
            var csrPem = JsonDocument.Parse(fake.Requests[0].Body).RootElement.GetProperty("csr").GetString()!;
            var request = CertificateRequest.LoadSigningRequestPem(csrPem, HashAlgorithmName.SHA256);
            Check(request.PublicKey.GetECDsaPublicKey()!.ExportParameters(false).Curve.Oid.FriendlyName is "nistP256" or "ECDSA_P256", "CSR curve");
            var file = File.ReadAllText(Path.Combine(dir, "nightwatch-vpn.json"));
            Check(!file.Contains("PRIVATE KEY") && !file.Contains("TLS-SECRET") && Secret.IsProtected(first.KeyProtected) && Secret.IsProtected(first.TlsProtected), "plain secrets on disk");
            Check(Directory.GetFiles(dir).Length == 1, "a temporary file stayed behind");
            Check(Secret.Unprotect(first.TlsProtected) == "TLS-SECRET-" + id1, "tls key round trip");
            Check(NightwatchVpn.Load(dir) is { InstallId: var loaded } && loaded == id1, "load");
            var firstKey = Secret.Unprotect(first.KeyProtected);

            // Renew: a new key, the exact message signed by the old one (DER), everything replaced at once.
            fake.Handle = (_, _) => (200, Issued(id1, "CERT2", NightwatchVpn.Now + 500, NightwatchVpn.Now + 1000), null);
            var second = NightwatchVpn.RenewAsync(endpoint, dir, first).GetAwaiter().GetResult();
            var sent = JsonDocument.Parse(fake.Requests[^1].Body).RootElement;
            Check(fake.Requests[^1].Path == "/v1/renew" && sent.GetProperty("install_id").GetString() == id1 && sent.GetProperty("cert").GetString() == "CERT1", "renew body");
            using (var old = ECDsa.Create())
            {
                old.ImportFromPem(firstKey);
                var message = NightwatchVpn.RenewMessage(id1, sent.GetProperty("csr").GetString()!, sent.GetProperty("timestamp").GetInt64());
                Check(old.VerifyData(Encoding.UTF8.GetBytes(message), Convert.FromBase64String(sent.GetProperty("signature").GetString()!),
                    HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence), "the old key did not sign the renewal");
            }
            Check(Secret.Unprotect(second.KeyProtected) != firstKey && NightwatchVpn.Load(dir)!.Cert == "CERT2", "renew replaced nothing");

            // Not due: no request. A network failure or a bad reply leaves the old install.
            var before = fake.Requests.Count;
            NightwatchVpn.RenewIfDueAsync(endpoint, dir).GetAwaiter().GetResult();
            Check(fake.Requests.Count == before, "renewed before renew_after");
            fake.Handle = (_, _) => (200, "{\"install_id\":\"x\"}", null);
            Fails(VpnFailure.BadReply, () => NightwatchVpn.RenewAsync(endpoint, dir, second), "bad reply");
            Check(NightwatchVpn.Load(dir)!.Cert == "CERT2", "a bad reply touched the install");
            NightwatchVpn.Save(dir, new VpnInstall
            {
                InstallId = id1, Cert = "CERT2", Profile = "client\n", CaBundle = "CA", KeyProtected = second.KeyProtected, TlsProtected = second.TlsProtected,
                RenewAfter = NightwatchVpn.Now - 1, Expires = NightwatchVpn.Now + 1000,
            });
            var due = NightwatchVpn.Load(dir)!;
            Check(NightwatchVpn.ReadyAsync(new VpnEndpoint("127.0.0.1", 1, fake.Pin), dir).GetAwaiter().GetResult().Cert == "CERT2", "unreachable: the old profile works until it expires");

            // 403: told, and even a still valid install is not used. 401: enroll again, with a new id.
            fake.Handle = (_, _) => (403, "{\"error\":\"banned\"}", null);
            Fails(VpnFailure.Blocked, () => NightwatchVpn.ReadyAsync(endpoint, dir), "blocked");
            Check(NightwatchVpn.Load(dir)!.InstallId == id1, "a block changed the install");
            fake.Handle = (path, _) => path == "/v1/renew" ? (401, "{}", null) : (200, Issued(id2, "CERT3", NightwatchVpn.Now + 500, NightwatchVpn.Now + 1000), null);
            Check(NightwatchVpn.RenewAsync(endpoint, dir, due).GetAwaiter().GetResult().InstallId == id2 && fake.Requests[^1].Path == "/v1/enroll", "401 enrolls again");

            // 429 last (it holds every later request of this process): Retry-After is kept, nothing more is sent.
            fake.Handle = (_, _) => (429, "{}", "120");
            Fails(VpnFailure.RateLimited, () => NightwatchVpn.EnrollAsync(endpoint, dir), "429");
            before = fake.Requests.Count;
            Fails(VpnFailure.RateLimited, () => NightwatchVpn.EnrollAsync(endpoint, dir), "held by Retry-After");
            Check(fake.Requests.Count == before, "a request went out during Retry-After");
            Check(NightwatchVpn.Load(dir)!.InstallId == id2, "429 changed the install");
        }
        finally { Directory.Delete(dir, true); }
    }
}
