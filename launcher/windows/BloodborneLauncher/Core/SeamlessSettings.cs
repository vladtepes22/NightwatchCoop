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
        ("party_items", L.T("Key items for every player")),
        ("party_progress", L.T("Shared progression")),
        ("party_progress_npc", L.T("Shared NPC quest choices")),
        ("party_story", L.T("Shared story and endings")),
        ("party_story_mirror", L.T("Cutscenes for every player")),
    };

    /// <summary>The game's own switches that stay each player's own choice (bbport.ini): what reaches your own
    /// save or items. The rest of <see cref="GameSwitches"/> are the host's rules (session rules the host decides
    /// for the party, 0.7.5); a guest follows the host's values, sent on connect.</summary>
    public static readonly string[] OwnSwitches =
    {
        "party_grant_bells", "party_items", "party_progress", "party_progress_npc", "party_story",
    };

    /// <summary>The start-up rules the host compares with every guest's: the game reads them once at start, and the
    /// host refuses a guest whose values differ ("set X like the host"); a refused guest takes the host's (TakeHostStartupRules).
    /// Key, default; party_start is the launcher's own variable (BB_PARTY_START), the others live in bbport.ini.</summary>
    public static readonly (string Key, string Default)[] StartupRules =
    {
        ("party_guest_lamps", "1"), ("party_guest_mark", "1"), ("party_full_rewards", "1"),
        ("party_guest_insight", "parity"), ("party_start", "prologue_solo"),
    };

    /// <summary>The host's start-up rules named in its refusal text (identity_mismatch in party_link.cpp, names and values
    /// from JoinRulesTag in party_balance_rules.cpp: change both together): key -> value as stored. Empty when none.</summary>
    public static Dictionary<string, string> StartupRulesFromRefusal(string? text)
    {
        var rules = new Dictionary<string, string>();
        foreach (Match match in Regex.Matches(text ?? "", "set \"([^\"]+)\" like the host \\(host: (.+?), yours: "))
        {
            var key = match.Groups[1].Value switch
            {
                "Guests use lamps" => "party_guest_lamps",
                "Guests can use Hunter's Mark" => "party_guest_mark",
                "Full rewards for guests" => "party_full_rewards",
                "Insight for bosses" => "party_guest_insight",
                "Party starts" => "party_start",
                _ => "",
            };
            var value = (key, match.Groups[2].Value) switch
            {
                ("party_guest_lamps" or "party_guest_mark" or "party_full_rewards", "on") => "1",
                ("party_guest_lamps" or "party_guest_mark" or "party_full_rewards", "off") => "0",
                ("party_guest_insight", "Same as the host") => "parity",
                ("party_guest_insight", "Full bonus") => "full",
                ("party_guest_insight", "Game's own (+1)") => "0",
                ("party_start", "Right away") => "immediate",
                ("party_start", "After the prologue") => "prologue_solo",
                _ => "",
            };
            if (value.Length > 0) rules[key] = value;
        }
        return rules;
    }

    /// <summary>A guest the host refused for its start-up rules: stores the host's values named in the refusal (party
    /// status detail and alert). True when one changed, so the game has to restart to join.</summary>
    public static bool TakeHostStartupRules(LauncherSettings s, IniFile ini, SeamlessSession.Status? status)
    {
        if (status is not { role: "guest" } || s.Str("party_mode") != "join") return false;
        var changed = false;
        foreach (var (key, value) in StartupRulesFromRefusal(status.detail + "\n" + status.alert))
        {
            if (StartupValue(key, key == "party_start" ? s.Str(key) : ini.Get(key, "")) == value) continue;
            if (key == "party_start") s.Set(key, value);
            else ini.Set(key, value);
            changed = true;
        }
        return changed;
    }

    /// <summary>One start-up rule's value as the host compares it: a switch is "1" or "0", Insight "parity", "full" or
    /// "0", the party start "prologue_solo" or "immediate"; anything else counts as the default.</summary>
    public static string StartupValue(string key, string? raw)
    {
        var value = (raw ?? "").Trim();
        return key switch
        {
            "party_guest_insight" => value is "full" or "0" ? value : "parity",
            "party_start" => value == "immediate" ? "immediate" : "prologue_solo",
            _ => value is "0" or "false" or "off" ? "0" : "1",
        };
    }

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
        if (Password(s).Trim().Length == 0)
            return s.Str("party_mode") == "join" ? L.T("Enter the host's party password before joining.")
                                                 : L.T("Set a party password first: the host and every guest use the same one.");
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

    /// <summary>The password the game gets: a guest's is the one remembered for the host it joins.</summary>
    public static string Password(LauncherSettings s) =>
        s.Str("party_mode") == "join" ? s.JoinPassword(s.Str("party_code")) : s.PartyPassword;

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
        env["BB_PARTY_PASSWORD"] = Password(s);
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
}
