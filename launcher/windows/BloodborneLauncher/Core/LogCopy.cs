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
    /// The header's log button: the newest session logs as files on the clipboard (pasting into Discord
    /// attaches them) and the last crash report as text. Both have IP addresses hidden the way the connection
    /// doctor's --redactips hides them, and the Windows user folder replaced, since they go to public channels.
    /// </summary>
    public static class LogCopy
    {
        public const int Count = 5;

        /// <summary>Written when the game crashes, so the report can still be copied after the next start.</summary>
        public static string CrashReportFile => Path.Combine(Path.GetDirectoryName(GameRunner.SessionLog)!, "last_crash_report.txt");

        /// <summary>The newest <paramref name="count"/> session logs in <paramref name="logDir"/>: last_session.log,
        /// previous_session.log, then logs\sessions newest first. previous_session.log is normally a copy of the
        /// newest archived one, so a file with the same contents as one already taken is skipped.</summary>
        public static List<string> NewestLogs(string logDir, int count = Count)
        {
            var candidates = new List<string> { Path.Combine(logDir, "last_session.log"), Path.Combine(logDir, "previous_session.log") };
            var archive = Path.Combine(logDir, "sessions");
            if (Directory.Exists(archive))
                candidates.AddRange(new DirectoryInfo(archive).GetFiles("session-*.log")
                    .OrderByDescending(f => f.Name, StringComparer.Ordinal).Select(f => f.FullName));
            var taken = new List<string>();
            var seen = new HashSet<string>();
            foreach (var path in candidates)
            {
                if (taken.Count >= count) break;
                if (Read(path) is not { } text || !seen.Add(text)) continue;
                taken.Add(path);
            }
            return taken;
        }

        private static readonly Regex Ip = new(@"\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b", RegexOptions.Compiled);
        private static readonly Regex OwnAddress = new(@"(setx BB_PARTY_LOCAL_IP |local address |to `|print `)(\d+\.\d+\.\d+\.\d+)", RegexOptions.Compiled);
        private static readonly Regex UserFolder = new(@"([A-Za-z]:\\Users\\)[^\\\s]+", RegexOptions.Compiled);

        /// <summary>net_doctor.py's redact(ips=True): this PC's VPN address named by its kind, a public address
        /// replaced whole, any other keeps only its first number. Plus the user folder (it holds the Windows name).</summary>
        public static string Redact(string text)
        {
            text = UserFolder.Replace(text, m => m.Groups[1].Value + "<user>");
            text = OwnAddress.Replace(text, m => m.Groups[1].Value + $"<your {VpnKind(m.Groups[2].Value)} IP>");
            return Ip.Replace(text, m => IsPublic(m.Value) ? "<public IP>" : m.Groups[1].Value + ".x.x.x");
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
            var logs = NewestLogs(Path.GetDirectoryName(GameRunner.SessionLog)!);
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
                File.WriteAllText(copy, Redact(Read(log) ?? ""));
                files.Add(copy);
            }
            return SetClipboard(() => Clipboard.SetFileDropList(files)) ? files.Count : null;
        }

        /// <summary>Keeps the crash's report (addresses hidden) for the log button.</summary>
        public static void SaveCrashReport(string report)
        {
            try { File.WriteAllText(CrashReportFile, Redact(report)); }
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

        private static string? Read(string path)
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
