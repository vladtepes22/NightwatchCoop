using System.IO;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    public static class ProblemReport
    {
        private static readonly string[] LauncherKeys =
        {
            "fps_mode", "fullscreen", "borderless_fullscreen", "present_mode", "draw_pipe", "readbacks",
            "fsr4_balanced", "legacy_shaders", "party_restart", "party_start",
        };

        private static readonly string[] GameKeys =
        {
            "upscaler", "preset", "output_res", "live_resolution", "model_lod", "sharpen", "sharpness",
            "object_motion", "frame_gen", "frame_gen_cap", "dlss_preset", "hdr", "hdr_mode", "hdr_paper_white",
            "hdr_peak_auto", "hdr_peak", "hdr_boost", "hdr_ui_brightness", "hdr_ui_color",
            "party_grant_bells", "party_open_world", "party_guest_respawn", "party_guest_refill",
            "party_chalice_rejoin", "party_guest_mark", "party_full_rewards", "party_items", "party_progress",
            "party_progress_npc", "party_story", "party_story_mirror", "party_guest_lamps", "party_guest_insight",
            "party_guest_revive_inplace", "party_rendezvous", "party_boss_drops", "party_phantom_hp",
        };

        public static string IssueUrl(string version) =>
            "https://github.com/vladtepes22/NightwatchCoop/issues/new?template=bug_report.yml&version=" + Uri.EscapeDataString(version);

        public static string SettingsSummary(LauncherSettings settings, IniFile ini)
        {
            var text = new StringBuilder();
            text.AppendLine("role=" + (settings.Str("party_mode") switch { "host" => "host", "join" => "guest", _ => "solo" }));
            void Add(string key, string value)
            {
                // Settings are editable text: never let a value inject extra report lines.
                if (Regex.IsMatch(value, @"\A[A-Za-z0-9_.#-]{1,64}\z")) text.AppendLine(key + "=" + value);
            }
            foreach (var key in LauncherKeys) Add(key, settings.Str(key));
            foreach (var key in GameKeys) Add(key, ini.Get(key));
            return LogCopy.Redact(text.ToString(), settings);
        }

        public static string BuildText(LauncherSettings settings, IniFile ini)
        {
            var text = new StringBuilder();
            text.AppendLine(CrashReport.VersionLine);
            text.AppendLine("Windows " + Environment.OSVersion.Version);
            var hw = Hardware.Detect();
            if (!string.IsNullOrWhiteSpace(hw.Gpu)) text.AppendLine("GPU: " + hw.Gpu);
            text.AppendLine(SettingsSummary(settings, ini));
            return LogCopy.Redact(text.ToString(), settings);
        }

        public static (string Zip, string Text) Create(LauncherSettings settings, IniFile ini)
        {
            var logDir = Path.GetDirectoryName(GameRunner.SessionLog)!;
            var folder = Path.Combine(logDir, "reports");
            Directory.CreateDirectory(folder);
            var zip = Path.Combine(folder, $"NightwatchCoop-report-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.zip");
            var text = BuildText(settings, ini);
            using (var stream = new FileStream(zip, FileMode.CreateNew))
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Create))
            {
                void Add(string name, string content)
                {
                    using var writer = new StreamWriter(archive.CreateEntry(name).Open(), new UTF8Encoding(false));
                    writer.Write(content);
                }
                Add("report.txt", text);
                foreach (var log in LogCopy.NewestLogs(logDir, 2, archives: false, distinct: false))
                {
                    var content = LogCopy.Read(log) ?? throw new IOException(L.T("A session log could not be read. Try again."));
                    Add("logs/" + Path.GetFileName(log), LogCopy.Redact(content, settings));
                }
            }
            return (zip, text);
        }
    }
}
