using System.IO;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core;

public static partial class SeamlessSettings
{
    /// <summary>The launcher's own co-op switches (settings.json; bb-start reads them, not the game).</summary>
    public static readonly (string Key, string Environment, string Label)[] Switches =
    {
        ("party_restart", "BB_PARTY_RESTART", L.T("Restart and rejoin after a crash")),
    };

    /// <summary>The game's co-op switches. They live in bbport.ini, where the game's own menu
    /// (System > Party rules / My party) changes them too, and the game reads them from there. Until
    /// 0.2.2 beta 2 the launcher kept them in settings.json and passed them as BB_PARTY_* variables,
    /// which win over the ini: a change made in the game was undone at the next start.</summary>
    public static readonly (string Key, string Label)[] GameSwitches =
    {
        ("party_grant_bells", L.T("Grant co-op bells")),
        ("party_open_world", L.T("Remove multiplayer barriers")),
        ("party_guest_respawn", L.T("Guest respawn")),
        ("party_guest_refill", L.T("Refill guest supplies")),
        ("party_chalice_rejoin", L.T("Stay in Chalice Dungeons")),
        ("party_guest_mark", L.T("Guests can use Hunter's Mark")),
        ("party_full_rewards", L.T("Full Blood Echo rewards")),
        ("party_items", L.T("Items for every player")),
        ("party_progress", L.T("Shared progression")),
        ("party_progress_npc", L.T("Shared NPC quest choices")),
        ("party_story", L.T("Shared story and endings")),
        ("party_story_mirror", L.T("Cutscenes for every player")),
    };

    /// <summary>Carries an older launcher's choices for <see cref="GameSwitches"/>, the DHT rendezvous
    /// and the guest Insight mode from settings.json into bbport.ini, once (only keys the ini doesn't have yet).
    /// True when something moved.</summary>
    public static bool MoveToIni(LauncherSettings s, IniFile ini)
    {
        var moved = false;
        foreach (var (key, _) in GameSwitches)
        {
            if (ini.Raw(key) != null || !s.All.Any(item => item.Key == key)) continue;
            ini.Set(key, s.Bool(key) ? "1" : "0");
            moved = true;
        }
        // The DHT rendezvous (System > Party network "Find via public DHT"), off by default.
        if (ini.Raw("party_rendezvous") == null && s.All.Any(item => item.Key == "party_rendezvous"))
        {
            ini.Set("party_rendezvous", s.Bool("party_rendezvous") ? "1" : "0");
            moved = true;
        }
        if (ini.Raw("party_guest_insight") == null && s.Str("party_guest_insight") is "full" or "0")
        {
            ini.Set("party_guest_insight", s.Str("party_guest_insight"));
            moved = true;
        }
        return moved;
    }

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
            return L.T("Party name must contain 1–16 letters, numbers, underscores or hyphens.");
        if (s.Str("party_port").Length > 0 &&
            (!int.TryParse(s.Str("party_port"), out var port) || port is < 1 or > 65535))
            return L.T("Party port must be between 1 and 65535.");
        if (s.Str("party_mode") == "join" && s.Str("party_code").Trim().Length == 0)
            return L.T("Enter the host's party code or address before joining.");
        var code = s.Str("party_code");
        if (code.Any(char.IsControl) || code.Length > 512)
            return L.T("The party code or address contains invalid characters.");
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
        env["BB_PARTY_STUN"] = s.Str("party_stun");
        env["BB_PARTY_PUBLIC_ADDR"] = s.Str("party_mode") == "host" ? s.Str("party_public_addr") : "";
        env["BB_PARTY_SAVE"] = "separate";
        env["BB_PARTY_START"] = s.Str("party_start") == "immediate" ? "immediate" : "prologue_solo";
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
        try { return File.Exists(path) ? File.ReadAllText(path).Trim() : L.T("Start hosting to generate your party codes."); }
        catch (IOException) { return L.T("Party codes are being updated."); }
    }
}
