using System.IO;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core;

public static partial class SeamlessSettings
{
    public static readonly (string Key, string Environment, string Label)[] Switches =
    {
        ("party_grant_bells", "BB_PARTY_GRANT_BELLS", "Grant co-op bells"),
        ("party_open_world", "BB_PARTY_OPEN_WORLD", "Remove multiplayer barriers"),
        ("party_guest_respawn", "BB_PARTY_GUEST_RESPAWN", "Guest respawn"),
        ("party_guest_refill", "BB_PARTY_GUEST_REFILL", "Refill guest supplies"),
        ("party_guest_mark", "BB_PARTY_GUEST_MARK", "Guests can use Hunter's Mark"),
        ("party_full_rewards", "BB_PARTY_FULL_REWARDS", "Full Blood Echo rewards"),
        ("party_items", "BB_PARTY_ITEMS", "Items for every player"),
        ("party_progress", "BB_PARTY_PROGRESS", "Shared progression"),
        ("party_progress_npc", "BB_PARTY_PROGRESS_NPC", "Shared NPC quest choices"),
        ("party_story", "BB_PARTY_STORY", "Shared story and endings"),
        ("party_story_mirror", "BB_PARTY_STORY_MIRROR", "Cutscenes for every player"),
        ("party_restart", "BB_PARTY_RESTART", "Restart and rejoin after a crash"),
    };

    public static bool Enabled(LauncherSettings s) => s.Str("party_mode") is "host" or "join";
    public static bool Flag(LauncherSettings s, string key, bool fallback = true) =>
        s.All.Any(item => item.Key == key) ? s.Bool(key) : fallback;

    public static string Name(LauncherSettings s)
    {
        var name = s.Str("party_name");
        if (name.Length > 0) return name;
        name = Regex.Replace(Environment.UserName, "[^A-Za-z0-9_-]", "_");
        if (name.Length == 0) name = "Hunter";
        name = name[..Math.Min(11, name.Length)] + "-" + Random.Shared.Next(0x10000).ToString("x4");
        s.Set("party_name", name);
        return name;
    }

    public static string? Validate(LauncherSettings s)
    {
        if (!Enabled(s)) return null;
        if (!Regex.IsMatch(Name(s), "^[A-Za-z0-9_-]{1,16}$"))
            return "Party name must contain 1–16 letters, numbers, underscores or hyphens.";
        if (s.Str("party_port").Length > 0 &&
            (!int.TryParse(s.Str("party_port"), out var port) || port is < 1 or > 65535))
            return "Party port must be between 1 and 65535.";
        if (s.Str("party_mode") == "join" && s.Str("party_code").Trim().Length == 0)
            return "Enter the host's party code or address before joining.";
        var code = s.Str("party_code");
        if (code.Any(char.IsControl) || code.Length > 512)
            return "The party code or address contains invalid characters.";
        return null;
    }

    public static void Apply(LauncherSettings s, Dictionary<string, string> env)
    {
        env["BB_PARTY"] = Enabled(s) ? s.Str("party_mode") : "off";
        env["BB_PARTY_AUTOCONTINUE"] = "0";
        env["BB_PARTY_RESTARTED"] = "0";
        if (!Enabled(s)) return;
        env["BB_ONLINE"] = "0";
        env["BB_PARTY_NAME"] = Name(s);
        env["BB_PARTY_PORT"] = s.Str("party_port") is { Length: > 0 } port ? port : "9307";
        env["BB_PARTY_CODE"] = s.Str("party_code").Trim();
        env["BB_PARTY_PASSWORD"] = s.PartyPassword;
        env["BB_PARTY_MAX"] = s.Str("party_max") is "2" or "3" or "4" or "5" ? s.Str("party_max") : "3";
        env["BB_PARTY_UPNP"] = Flag(s, "party_upnp") ? "1" : "0";
        env["BB_PARTY_RENDEZVOUS"] = Flag(s, "party_rendezvous", false) ? "1" : "0";
        env["BB_PARTY_STUN"] = s.Str("party_stun");
        env["BB_PARTY_PUBLIC_ADDR"] = s.Str("party_mode") == "host" ? s.Str("party_public_addr") : "";
        env["BB_PARTY_SAVE"] = "separate";
        env["BB_PARTY_START"] = s.Str("party_start") == "immediate" ? "immediate" : "prologue_solo";
        env["BB_PARTY_GUEST_INSIGHT"] = s.Str("party_guest_insight") is "full" or "0" ? s.Str("party_guest_insight") : "parity";
        env["BB_SAVE_BACKUP_MINUTES"] = s.Str("party_backup_minutes") is { Length: > 0 } minutes ? minutes : "10";
        env["BB_SAVE_BACKUP_KEEP"] = s.Str("party_backup_keep") is { Length: > 0 } keep ? keep : "10";
        env["BB_PARTY_SEAMLESS"] = "1";
        env["BB_PARTY_DIRECT_SUMMON"] = "1";
        env["BB_PARTY_AUTO"] = "1";
        env["BB_RELAUNCH_LOOP"] = "1";
        env["BB_PARTY_LOG"] = s.Str("party_log");
        foreach (var option in Switches) env[option.Environment] = Flag(s, option.Key) ? "1" : "0";
    }

    public static string Codes(LauncherSettings s)
    {
        var path = Path.Combine(GameEnvironment.UserDir(s), "party_code.txt");
        try { return File.Exists(path) ? File.ReadAllText(path).Trim() : "Start hosting to generate your party codes."; }
        catch (IOException) { return "Party codes are being updated."; }
    }
}
