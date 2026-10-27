using System.IO;
using System.Collections.Specialized;
using System.Net;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The header's log button: last_session.log and previous_session.log as files on the clipboard (pasting into Discord
    /// attaches them) and the last crash report as text. Both have IP addresses hidden the way the connection
    /// doctor's --redactips hides them, and the Windows user folder replaced, since they go to public channels.
    /// </summary>
    public static class LogCopy
    {
        public const int Count = 2;

        /// <summary>Written when the game crashes, so the report can still be copied after the next start.</summary>
        public static string CrashReportFile => Path.Combine(Path.GetDirectoryName(GameRunner.SessionLog)!, "last_crash_report.txt");

        /// <summary>The newest <paramref name="count"/> session logs in <paramref name="logDir"/>: last_session.log,
        /// previous_session.log, then logs\sessions newest first. previous_session.log is normally a copy of the
        /// newest archived one, so a file with the same contents as one already taken is skipped.</summary>
        public static List<string> NewestLogs(string logDir, int count = Count, bool archives = true, bool distinct = true)
        {
            var candidates = new List<string> { Path.Combine(logDir, "last_session.log"), Path.Combine(logDir, "previous_session.log") };
            var archive = Path.Combine(logDir, "sessions");
            if (archives && Directory.Exists(archive))
                candidates.AddRange(new DirectoryInfo(archive).GetFiles("session-*.log")
                    .OrderByDescending(f => f.Name, StringComparer.Ordinal).Select(f => f.FullName));
            var taken = new List<string>();
            var seen = new HashSet<string>();
            foreach (var path in candidates)
            {
                if (taken.Count >= count) break;
                if (Read(path) is not { } text || distinct && !seen.Add(text)) continue;
                taken.Add(path);
            }
            return taken;
        }

        private static readonly Regex Ip = new(@"\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b", RegexOptions.Compiled);
        private static readonly Regex OwnAddress = new(@"(setx BB_PARTY_LOCAL_IP |local address |to `|print `)(\d+\.\d+\.\d+\.\d+)", RegexOptions.Compiled);
        private static readonly Regex UserFolder = new(@"([A-Za-z]:\\Users\\)[^\\\r\n]+(?=\\)", RegexOptions.Compiled | RegexOptions.IgnoreCase);

        /// <summary>net_doctor.py's redact(ips=True): this PC's VPN address named by its kind, a public address
        /// replaced whole, any other keeps only its first number. Plus the user folder (it holds the Windows name).</summary>
        public static string Redact(string text, string player = "", IEnumerable<string>? secrets = null)
        {
            text = RedactAddresses(text);
            var replacements = (secrets ?? Array.Empty<string>()).Where(s => !string.IsNullOrWhiteSpace(s))
                .Select(s => (Value: s, Replacement: "<secret>"))
                // A very short name would also hit ordinary words in the log.
                .Concat(player.Trim().Length < 3 ? Array.Empty<(string Value, string Replacement)>() : new[] { (Value: player, Replacement: "<player>") })
                .SelectMany(s => new[] { s, (Value: RedactAddresses(s.Value), s.Replacement) })
                .OrderByDescending(s => s.Value.Length).ToArray();
            if (replacements.Length > 0)
                text = Regex.Replace(text, string.Join("|", replacements.Select(s => Regex.Escape(s.Value))),
                    m => replacements.First(s => string.Equals(s.Value, m.Value, StringComparison.OrdinalIgnoreCase)).Replacement,
                    RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
            return text;
        }

        private static string RedactAddresses(string text)
        {
            text = UserFolder.Replace(text, m => m.Groups[1].Value + "<user>");
            text = IpV6.Replace(text, m =>
            {
                var candidate = m.Value.TrimEnd('.');
                var addressText = candidate.Split('%')[0];
                return IPAddress.TryParse(addressText, out var address) && address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6
                    ? "<IP>" + m.Value[candidate.Length..] : m.Value;
            });
            text = OwnAddress.Replace(text, m => m.Groups[1].Value + $"<your {VpnKind(m.Groups[2].Value)} IP>");
            return Ip.Replace(text, m => IsPublic(m.Value) ? "<public IP>" : m.Groups[1].Value + ".x.x.x");
        }

        private static readonly Regex IpV6 = new(@"(?<![\w:])(?:[0-9a-f]*:){2,}[0-9a-f:.]*(?:%[\w.-]+)?", RegexOptions.Compiled | RegexOptions.IgnoreCase);

        public static string Redact(string text, LauncherSettings settings)
        {
            var secrets = new List<string> { settings.PartyPassword, settings.OnlinePassword, settings.Str("party_code").Trim(), settings.Str("party_public_addr").Trim() };
            foreach (var host in settings.Map("party_join_passwords").Keys)
            {
                secrets.Add(host);
                secrets.Add(settings.JoinPassword(host));
            }
            return Redact(text, settings.Str("party_name"), secrets);
        }

        private static string VpnKind(string ip) =>
            ip.StartsWith("26.") ? "Radmin" : ip.StartsWith("25.") ? "Hamachi" : ip.StartsWith("100.") ? "Tailscale" : "VPN";

        /// <summary>Python's ip_address(ip).is_global and not a VPN range (Radmin 26/8, Hamachi 25/8, CGNAT/Tailscale 100.64/10).</summary>
        public static bool IsPublic(string ip)
        {
            if (!IPAddress.TryParse(ip, out var address)) return false;
            var b = address.GetAddressBytes();
            if (b.Length != 4) return false;
            return !(b[0] is 0 or 10 or 25 or 26 or 127 or >= 224
                     || b[0] == 100 && (b[1] & 0xC0) == 64
                     || b[0] == 169 && b[1] == 254
                     || b[0] == 172 && (b[1] & 0xF0) == 16
                     || b[0] == 192 && b[1] == 168
                     || b[0] == 192 && b[1] == 0 && b[2] is 0 or 2
                     || b[0] == 198 && (b[1] & 0xFE) == 18
                     || b[0] == 198 && b[1] == 51 && b[2] == 100
                     || b[0] == 203 && b[1] == 0 && b[2] == 113);
        }

        /// <summary>Copies the hidden-address logs to a folder of our own under %TEMP% and puts those files on the
        /// clipboard. Returns how many went, or null when another program holds the clipboard.</summary>
        public static int? CopyLogs()
        {
            var logs = NewestLogs(Path.GetDirectoryName(GameRunner.SessionLog)!, archives: false, distinct: false);
            if (logs.Count == 0) return 0;
            var dir = Path.Combine(Path.GetTempPath(), "NightwatchCoop-logs");
            if (Directory.Exists(dir))
                foreach (var old in Directory.GetFiles(dir))
                    try { File.Delete(old); } catch (IOException) { /* still attached somewhere: overwritten or left */ }
            Directory.CreateDirectory(dir);
            var files = new StringCollection();
            foreach (var log in logs)
            {
                var copy = Path.Combine(dir, Path.GetFileName(log));
                File.WriteAllText(copy, Redact(Read(log) ?? "", Workspace.Current.Settings));
                files.Add(copy);
            }
            return SetClipboard(() => Clipboard.SetFileDropList(files)) ? files.Count : null;
        }

        /// <summary>Keeps the crash's report (addresses hidden) for the log button.</summary>
        public static void SaveCrashReport(string report)
        {
            try { File.WriteAllText(CrashReportFile, Redact(report, Workspace.Current.Settings)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { /* read-only folder: nothing kept */ }
        }

        /// <summary>The last crash report as text on the clipboard: true copied, false clipboard busy, null none kept.</summary>
        public static bool? CopyCrashReport()
        {
            if (Read(CrashReportFile) is not { Length: > 0 } report) return null;
            return ClipboardText.Set(report);
        }

        private static bool SetClipboard(Action set)
        {
            for (int attempt = 0; attempt < 10; attempt++)
            {
                try
                {
                    set();
                    return true;
                }
                catch (ExternalException)
                {
                    Thread.Sleep(50);
                }
            }
            return false;
        }

        internal static string? Read(string path)
        {
            try
            {
                using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream);
                return reader.ReadToEnd();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return null;
            }
        }
    }
}
