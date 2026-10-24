using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;
using PasswordBox = Wpf.Ui.Controls.PasswordBox;

namespace BloodborneLauncher.Views;

public sealed class SeamlessPage : LauncherPage
{
    private readonly TextBlock _codes;
    private readonly TextBlock _status;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    /// Rows and group headers shown only in some modes ("" single player, "host", "join").
    private readonly List<(UIElement Element, Func<string, bool> When)> _byMode = new();
    private static bool InParty(string mode) => mode.Length > 0;
    private static bool Hosting(string mode) => mode == "host";
    private static bool Joining(string mode) => mode == "join";

    public SeamlessPage() : base("Seamless co-op",
        "Play the campaign together with visy's seamless co-op. The host shares a party code; everyone uses the same build, gameplay mods and party size.")
    {
        // Single player shows only Party saves; Join leaves out the host's rows, Host the guest's.
        Group("Party");
        SettingCombo(SymbolRegular.People24, "Mode", "Host starts the party; Join connects to the host.",
            new[] { new Choice("Single player", ""), new Choice("Host", "host"), new Choice("Join", "join") }, "party_mode",
            changed: ShowMode, buttons: true);
        var party = Rows(InParty);
        TextRow(SymbolRegular.Person24, "Your name", "1–16 letters, numbers, underscores or hyphens.", "Hunter",
            () => SeamlessSettings.Name(S), v => S.Set("party_name", v), party);
        TextRow(SymbolRegular.Link24, "Host code or address", "Paste the host's Internet code, LAN code or host:port.", "BBP3-…",
            () => S.Str("party_code"), v => S.Set("party_code", v), Rows(Joining));
        party = Rows(InParty);
        var password = new PasswordBox { Width = 300, Password = S.PartyPassword, PlaceholderText = "Optional party password", PlaceholderEnabled = true };
        password.PasswordChanged += (_, _) => { S.PartyPassword = password.Password; Ws.SaveSoon(); };
        Add(Card(SymbolRegular.Password24, "Password", "Use the same password on every PC. Stored encrypted for your Windows user.", password, out _), party);
        ComboRow(SymbolRegular.People24, "Party size", "Host and guests use the same limit. Five players is experimental.",
            new[] { new Choice("2", "2"), new Choice("3", "3"), new Choice("4", "4"), new Choice("5 (experimental)", "5") },
            () => S.Str("party_max") is { Length: > 0 } n ? n : "3", v => S.Set("party_max", v), party);

        _byMode.Add((Group("Connection"), InParty));
        party = Rows(InParty);
        TextRow(SymbolRegular.Router24, "Party port", "TCP and UDP. Default: 9307.", "9307", () => S.Str("party_port"), v => S.Set("party_port", v), party, width: 120);
        ToggleRow(SymbolRegular.Router24, "UPnP", "Map the host's port automatically.", () => SeamlessSettings.Flag(S, "party_upnp"), v => S.Set("party_upnp", v), Rows(Hosting));
        party = Rows(InParty);
        ToggleRow(SymbolRegular.Globe24, "DHT rendezvous", "Optional NAT traversal through the public BitTorrent DHT. Enable on every PC to use it.",
            () => Ini.Get("party_rendezvous", "0") == "1", v => Ini.Set("party_rendezvous", v ? "1" : "0"), party);
        // Kept in bbport.ini like the game's System > Party network rows, so both show the same value.
        ToggleRow(SymbolRegular.People24, "Relay for party members", "A member that cannot reach the host joins through you (party members only, capped upload).",
            () => Ini.Get("party_member_relay", "1") == "1", v => Ini.Set("party_member_relay", v ? "1" : "0"), party);
        ToggleRow(SymbolRegular.Globe24, "IPv6", "Try IPv6 when both PCs have it. A host without an open port then puts its IPv6 address into the party code.",
            () => Ini.Get("party_ipv6", "0") == "1", v => Ini.Set("party_ipv6", v ? "1" : "0"), party);
        TextRow(SymbolRegular.Server24, "STUN server", "Empty uses stun.l.google.com:19302. Use off to disable.", "Default",
            () => S.Str("party_stun"), v => S.Set("party_stun", v), party);
        var host = Rows(Hosting);
        TextRow(SymbolRegular.Globe24, "Host public address", "Leave on Automatic unless guests reach you another way: your VPN address (Radmin VPN, Tailscale, ZeroTier), or your public IP or DNS name when you forwarded the port yourself (host or host:port). The party code then carries it.", "Automatic",
            () => S.Str("party_public_addr"), v => S.Set("party_public_addr", v), host);
        _codes = Text("", "Faint");
        _codes.TextWrapping = TextWrapping.Wrap;
        host.Children.Add(_codes);
        Add(Card(SymbolRegular.Copy24, "Share party codes", "Available after the host starts. Use the LAN code on the same router.",
            SmallButton("Copy codes", SymbolRegular.Copy24, (_, _) => Clipboard.SetText(SeamlessSettings.Codes(S))), out _), host);

        party = Rows(InParty);
        _status = Text("Start the game to see party status.", "Faint");
        _status.TextWrapping = TextWrapping.Wrap;
        party.Children.Add(_status);
        foreach (var (title, action) in new[] { ("Leave party", "leave"), ("Rejoin party", "rejoin") })
        {
            var command = action;
            Add(Card(SymbolRegular.People24, title, null,
                SmallButton(title, SymbolRegular.People24, (_, _) => {
                    try { SeamlessSession.Command(S, command); }
                    catch (Exception e) { Prompt.Tell("Party", e.Message); }
                }), out _), party);
        }
        host = Rows(Hosting);
        Add(Card(SymbolRegular.People24, "Remove a member", "Host only. Enter the member's exact party name.",
            SmallButton("Choose member", SymbolRegular.People24, (_, _) => {
                var status = SeamlessSession.Read(S);
                if (status?.role != "host") { Prompt.Tell("Party", "Only the host can remove members."); return; }
                var members = status.members.Where(m => !m.local).ToArray();
                if (members.Length == 0) { Prompt.Tell("Party", "No remote members to remove."); return; }
                var choice = Prompt.Choose("Remove member", "Choose a member to disconnect.", members.Select(m => m.name).ToArray());
                if (choice >= 0) {
                    try { SeamlessSession.Command(S, "kick", members[choice].name); }
                    catch (Exception e) { Prompt.Tell("Party", e.Message); }
                }
            }), out _), host);
        Add(Card(SymbolRegular.Link24, "Connect a guest", "Host: copy the guest's BBG1 code first, then add it here.",
            SmallButton("Add copied guest code", SymbolRegular.Link24, (_, _) => {
                try {
                    var code = Clipboard.GetText();
                    if (SeamlessSession.Read(S)?.role != "host" || code.Length > 3500 ||
                        !(code.Contains("BBG1", StringComparison.OrdinalIgnoreCase) || code.Contains("BBG6", StringComparison.OrdinalIgnoreCase)))
                        throw new InvalidOperationException("Host a party and copy the guest's BBG1 connection code first.");
                    SeamlessSession.Command(S, "add_guest_code", code);
                } catch (Exception e) { Prompt.Tell("Party", e.Message); }
            }), out _), host);

        var join = Rows(Joining);
        Add(Card(SymbolRegular.Link24, "Guest connection code", "Guests can share this with a host when NAT prevents a direct connection.",
            SmallButton("Copy guest code", SymbolRegular.Copy24, (_, _) => {
                var code = SeamlessSession.Read(S)?.guest_code;
                if (string.IsNullOrEmpty(code)) Prompt.Tell("Party", "No guest connection code is available yet.");
                else Clipboard.SetText(code);
            }), out _), join);
        Add(Card(SymbolRegular.Link24, "Join through a member", "Guest: when the host cannot be reached but another member can, copy that member's guest code and add it here.",
            SmallButton("Add copied member code", SymbolRegular.Link24, (_, _) => {
                try {
                    var code = Clipboard.GetText();
                    if (SeamlessSession.Read(S)?.role != "guest" || code.Length > 3500 || !code.Contains("BBG1", StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("Join a party and copy another member's BBG1 guest code first.");
                    SeamlessSession.Command(S, "add_relay_code", code);
                } catch (Exception e) { Prompt.Tell("Party", e.Message); }
            }), out _), join);

        // Party settings as in the game's System > Party rules / My party pages (visy 0.7.3): the host's
        // rules for the whole party, then what each player chooses for itself.
        _byMode.Add((Group("Party rules"), Hosting));
        host = Rows(Hosting);
        host.Children.Add(Text("The host's rules for the whole party. When you join, the host's count and yours are kept for when you host (in the game: System > Party rules).", "Faint"));
        IniToggles(host,
            ("party_kill_record", "Enemies stay dead", true), ("party_gather", "Host can gather the party", true),
            ("party_npc_mirror", "NPCs as in the host's world", true),
            ("party_covenant_per_player", "Covenant outcomes per player", true),
            ("party_travel_vote", "Party votes on travel", false),
            ("party_npc_fights", "NPC hunters fight the whole party", true),
            ("party_any_kidnap", "Anyone can be abducted", true),
            ("party_friendly_fire", "Friendly fire / PvP", false), ("party_share_lamps", "Shared lamps", true),
            ("party_guests_bring_lamps", "Guests bring their lamps", false),
            ("party_rot", "Rot on death", false), ("party_rot_cleanse_boss", "Boss kill cleanses rot", true),
            ("party_rot_cleanse_killer", "Killing your killer cleanses its rot", true),
            ("party_rot_cleanse_echoes", "Retrieving Blood Echoes cleanses its rot", true));
        IniNumbers(host,
            ("party_boss_hp", "Boss HP per guest (%)", 150), ("party_boss_damage", "Boss damage per guest (%)", 10),
            ("party_boss_poise", "Boss poise per guest (%)", 100), ("party_enemy_hp", "Enemy HP per guest (%)", 50),
            ("party_enemy_damage", "Enemy damage per guest (%)", 0), ("party_enemy_poise", "Enemy poise per guest (%)", 0));

        _byMode.Add((Group("My party"), InParty));
        party = Rows(InParty);
        party.Children.Add(Text("Your own party settings: your screen, what you get as a guest, story sync and start (in the game: System > My party).", "Faint"));
        IniToggles(party,
            ("party_hud", "Party HUD", true), ("party_name_tags", "Party name tags", true), ("party_pings", "Pings", true),
            ("party_through_walls", "Show party through walls", false),
            ("party_glow_off", "Companions without glow", true), ("party_toasts", "Join, leave and travel notices", true),
            ("party_npc_talk", "Share NPC dialogue", true), ("party_guest_lamps", "Guests use lamps", true),
            ("party_key_view", "Own key items as a guest", true), ("party_key_items_summoned", "Key items at once", true));
        IniNumbers(party, ("party_npc_talk_range", "NPC dialogue range (m)", 25), ("party_overhead", "Name tag detail (0–4)", 3));
        IniToggles(party, SeamlessSettings.GameSwitches.Select(o => (o.Key, o.Label, true)).ToArray());
        foreach (var option in SeamlessSettings.Switches)
        {
            var key = option.Key;
            ToggleRow(SymbolRegular.People24, option.Label, PartyInfo(key), () => SeamlessSettings.Flag(S, key), v => S.Set(key, v), party);
        }
        ComboRow(SymbolRegular.People24, "Insight for bosses", PartyInfo("party_guest_insight"),
            new[] { new Choice("Same as host", "parity"), new Choice("Full bonus", "full"), new Choice("Game's own", "0") },
            () => Ini.Get("party_guest_insight", "parity") is "full" or "0" ? Ini.Get("party_guest_insight") : "parity",
            v => Ini.Set("party_guest_insight", v), party);
        ComboRow(SymbolRegular.Play24, "Party starts", PartyInfo("party_start"),
            new[] { new Choice("After the prologue", "prologue_solo"), new Choice("Right away", "immediate") },
            () => S.Str("party_start") == "immediate" ? "immediate" : "prologue_solo", v => S.Set("party_start", v), party);

        Group("Party saves");
        Body.Children.Add(Text("Seamless play uses its own savedata_party folder. Automatic snapshots keep single-player and party progress recoverable.", "Faint"));
        ComboRow(SymbolRegular.Timer24, "Backup interval", "Minutes; session-start snapshots are always kept.",
            new[] { new Choice("5 min", "5"), new Choice("10 min", "10"), new Choice("30 min", "30"), new Choice("At start", "0") },
            () => S.Str("party_backup_minutes") is { Length: > 0 } n ? n : "10", v => S.Set("party_backup_minutes", v));
        ComboRow(SymbolRegular.Save24, "Backups to keep", null,
            new[] { new Choice("10", "10"), new Choice("20", "20"), new Choice("50", "50"), new Choice("100", "100") },
            () => S.Str("party_backup_keep") is { Length: > 0 } n ? n : "10", v => S.Set("party_backup_keep", v));
        Add(Card(SymbolRegular.Folder24, "Save and backup folder", null,
            SmallButton("Open", SymbolRegular.Folder24, (_, _) => {
                var folder = GameEnvironment.UserDir(S); Directory.CreateDirectory(folder);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", folder) { UseShellExecute = true });
            }), out _));
        Add(Card(SymbolRegular.Copy24, "Copy single-player progress into party saves", "Existing party saves are backed up before replacement.",
            SmallButton("Copy progress", SymbolRegular.Copy24, async (_, _) => {
                if (!Prompt.Confirm("Copy progress", "Copy this profile's single-player saves into its party save folder? Existing party progress will be backed up.", "Copy")) return;
                await SaveAction("copy");
            }), out _));
        Add(Card(SymbolRegular.Save24, "Undo last party session", "Restores both save folders and first backs up their current contents.",
            SmallButton("Review restore", SymbolRegular.Save24, async (_, _) => {
                try {
                    var description = await SeamlessSession.SaveTool(S, "describe");
                    if (Prompt.Confirm("Undo party session", description, "Restore")) await SaveAction("undo");
                } catch (Exception e) { Prompt.Tell("Party saves", e.Message); }
            }), out _));
        Add(Card(SymbolRegular.Save24, "Check party saves", "Checks save structure and compares progress with the last session snapshot.",
            SmallButton("Check", SymbolRegular.Save24, async (_, _) => await SaveAction("check")), out _));
        // visy 0.7.3 (scripts/save_editor.py): a save last written as a guest in another world lacks the own
        // world's state that save editors look for; the export adds it and the import takes it out again.
        Add(Card(SymbolRegular.ArrowExport24, "Export for save editor", "A copy of one character save that save editors can open. Your save is not changed.",
            SmallButton("Export", SymbolRegular.ArrowExport24, async (_, _) => await ExportForEditor()), out _));
        Add(Card(SymbolRegular.ArrowImport24, "Import from save editor", "Puts an edited export back into its slot. Your saves are backed up first.",
            SmallButton("Import", SymbolRegular.ArrowImport24, async (_, _) => await ImportFromEditor()), out _));
        ComboRow(SymbolRegular.Document24, "Party log detail", "Verbose and trace logging add overhead.",
            new[] { new Choice("Normal", ""), new Choice("Verbose", "verbose"), new Choice("Trace", "trace") },
            () => S.Str("party_log"), v => S.Set("party_log", v));

        _timer.Tick += (_, _) => {
            _codes.Text = SeamlessSettings.Codes(S);
            var status = SeamlessSession.Read(S);
            _status.Text = status == null ? (Ws.Game.Running ? "Waiting for party status…" : "Game stopped.") :
                $"{status.role}: {status.state}  {status.detail}\n{status.network}\n" +
                string.Join("\n", status.members.Select(m => $"{m.name}: {(m.in_world ? "in host world" : m.connected ? "connected" : "disconnected")}  {(m.ping_ms < 0 ? "" : m.ping_ms + " ms")}  {m.area}")) +
                $"\n{status.@event}\n{status.alert}";
        };
        Loaded += (_, _) => { _codes.Text = SeamlessSettings.Codes(S); _timer.Start(); };
        Unloaded += (_, _) => _timer.Stop();
        ShowMode();
    }
    /// A panel for the next rows, shown in the modes when accepts (ShowMode).
    private StackPanel Rows(Func<string, bool> when)
    {
        var rows = new StackPanel();
        Body.Children.Add(rows);
        _byMode.Add((rows, when));
        return rows;
    }
    private void ShowMode()
    {
        var mode = S.Str("party_mode");
        foreach (var (element, when) in _byMode)
            element.Visibility = when(mode) ? Visibility.Visible : Visibility.Collapsed;
    }
    // Party rules from engine/gpu/shim/bbport_game_menu_pages.cpp; My party descriptions from
    // engine/docs/party/my_party_settings.md, plus the remaining My party rows.
    private static string? PartyInfo(string key) => key switch
    {
        "party_kill_record" => L.T("A party kill stays dead for everyone, even when maps reload, until a rest."),
        "party_gather" => L.T("Party > Gather the party brings every guest to you; each guest is asked first."),
        "party_npc_mirror" => L.T("An NPC gone from your world is gone for guests too."),
        "party_covenant_per_player" => L.T("Annalise's, Alfred's and Valtr's quest outcomes stay in each player's own save."),
        "party_travel_vote" => L.T("Party travel by lamp, headstone or Hunter's Mark needs every member's approval; one refusal cancels it."),
        "party_npc_fights" => L.T("A hunter NPC that turns hostile is hostile to guests too, so everyone can fight it."),
        "party_any_kidnap" => L.T("A Snatcher grabbing a guest takes the whole party to the Hypogean Gaol, just as when it grabs the host."),
        "party_friendly_fire" => L.T("Party members can hurt each other; being killed by a party member counts as a normal death."),
        "party_share_lamps" => L.T("A lamp any party member lights is lit for everyone, in each save."),
        "party_guests_bring_lamps" => L.T("A joining guest's own lamps light up in your world too. Requires Shared lamps."),
        "party_rot" => L.T("Each death adds a penalty, stacking up to five times; resting at a lamp removes it."),
        "party_rot_cleanse_boss" => L.T("A boss's death removes all rot from everyone in the host's world. Requires Rot on death."),
        "party_rot_cleanse_killer" => L.T("When the enemy that killed you dies, the rot from that death is removed. Requires Rot on death."),
        "party_rot_cleanse_echoes" => L.T("Retrieving your Blood Echoes removes the rot from the death that dropped them. Requires Rot on death."),
        "party_boss_hp" => L.T("Controls how much boss health scales for each additional guest."),
        "party_boss_damage" => L.T("Controls how much boss damage scales for each additional guest."),
        "party_boss_poise" => L.T("Controls how much boss poise scales for each additional guest."),
        "party_enemy_hp" => L.T("Controls how much enemy health scales for each additional guest."),
        "party_enemy_damage" => L.T("Controls how much enemy damage scales for each additional guest."),
        "party_enemy_poise" => L.T("Controls how much enemy poise scales for each additional guest."),
        "party_hud" => L.T("Shows companions' names, health and where they are, in the bottom left corner. Party mode only."),
        "party_name_tags" => L.T("On: our name plates above companions, never hidden by walls. Off: the game's own plate."),
        "party_pings" => L.T("Touchpad + R3, or the Ping key (default B, under Controls), marks where you look. Everyone sees the mark for 10 s."),
        "party_through_walls" => L.T("A small marker with the distance on each companion on your map, also behind walls."),
        "party_glow_off" => L.T("On: other hunters look like normal hunters on your screen. Off: the game's blue phantom glow."),
        "party_toasts" => L.T("A short notice in a top corner when a hunter joins, leaves or travels."),
        "party_npc_talk" => L.T("See and hear the NPC conversations of companions near you (subtitles, choices, gifts)."),
        "party_guest_lamps" => L.T("Guests may use lamps and headstones; the whole party travels with them. Needs a restart."),
        "party_key_view" => L.T("While you are a guest, your own keys and badges show in Key Items; off shows the game's empty list. Takes effect on the next game start."),
        "party_key_items_summoned" => L.T("While you are a guest, a key the host's world gives you is yours at once (needs Own key items as a guest). Off: the key waits until you are back in your own world."),
        "party_npc_talk_range" => L.T("How close a companion has to be for you to see their NPC conversation."),
        "party_overhead" => L.T("What shows above companions in view, only for you: nothing, name, name and level, name and health, name and deaths, or name and ping."),
        "party_grant_bells" => L.T("Gives the Beckoning Bell and Small Resonant Bell after the prologue, for ringing by hand. Takes effect on the next game start."),
        "party_open_world" => L.T("Removes co-op walls on area borders so the party can walk anywhere; keep it the same for everyone. Takes effect on the next game start."),
        "party_guest_respawn" => L.T("While you are a guest, a death brings you back at the host's lamp, not at home. Takes effect on the next game start."),
        "party_guest_refill" => L.T("While you are a guest, vials and bullets refill when the host rests, dies or travels. Takes effect on the next game start."),
        "party_chalice_rejoin" => L.T("As a guest, if a death or forced trip takes you out of the host's dungeon, you are summoned back. Leaving by choice: use Rejoin."),
        "party_guest_mark" => L.T("A guest's Hunter's Mark asks whether to travel alone or with the party; off uses the game's own behavior. Takes effect on the next game start."),
        "party_full_rewards" => L.T("While you are a guest, receive full Blood Echoes and enemy drops like the host; off uses the game's co-op amounts. Takes effect on the next game start."),
        "party_items" => L.T("Boss drops, key items and gifts the host gets are given to each guest too, once. Takes effect on the next game start."),
        "party_progress" => L.T("Bosses, lamps, shortcuts and key events are copied into every guest's save. Applies to host and guests on the next game start."),
        "party_progress_npc" => L.T("The first NPC quest choice anyone makes applies to every save (needs Shared progression). Takes effect on the next game start."),
        "party_story" => L.T("Guests see the host's cutscenes, time of day and ending. Applies to host and guests on the next game start."),
        "party_story_mirror" => L.T("A guest in the host's world watches cutscenes with the host; off lets the guest watch them later in their own world. Takes effect on the next game start."),
        "party_restart" => L.T("Automatically restarts the game after a crash and rejoins the party."),
        "party_guest_insight" => L.T("Controls the Insight you receive as a guest when the host kills a boss. Takes effect on the next game start."),
        "party_start" => L.T("Each new character normally completes its own prologue first. Keep it the same for everyone."),
        _ => null,
    };

    /// Switches kept in bbport.ini like the game's party pages, so both show the same value.
    private void IniToggles(Panel into, params (string Key, string Label, bool Fallback)[] rows)
    {
        foreach (var (key, label, fallback) in rows)
            ToggleRow(SymbolRegular.People24, label, PartyInfo(key), () => Ini.Get(key, fallback ? "1" : "0") == "1", v => Ini.Set(key, v ? "1" : "0"), into);
    }
    private void IniNumbers(Panel into, params (string Key, string Label, int Value)[] rows)
    {
        foreach (var (key, label, value) in rows)
            TextRow(SymbolRegular.Settings24, label, PartyInfo(key), value.ToString(), () => Ini.Get(key, value.ToString()), v => Ini.Set(key, v), into, width: 120);
    }
    private sealed record EditorSave(string kind, int slot, string text, bool guest_written);
    private sealed record EditorPlan(string kind, int slot, string target, string[] notes);

    private async Task ExportForEditor()
    {
        try
        {
            var saves = System.Text.Json.JsonSerializer.Deserialize<EditorSave[]>(await SeamlessSession.SaveTool(S, "editor-list")) ?? [];
            if (saves.Length == 0) { Prompt.Tell("Export for save editor", "There is no character save yet."); return; }
            var choice = Prompt.Choose("Export for save editor", "Which character?", saves.Select(s => s.text).ToArray());
            if (choice < 0) return;
            var folder = new Microsoft.Win32.OpenFolderDialog { Title = "Folder for the save editor's copy (an empty folder is best)" };
            if (folder.ShowDialog() != true) return;
            var save = saves[choice];
            Prompt.Tell("Export for save editor", await SeamlessSession.SaveTool(S, "editor-export", save.kind, save.slot.ToString(), folder.FolderName) +
                (save.guest_written ? " The copy was fixed up for the editor (it was last saved as a guest in another world)." : ""));
        }
        catch (Exception e) { Prompt.Tell("Export for save editor", e.Message); }
    }
    private async Task ImportFromEditor()
    {
        try
        {
            var file = new Microsoft.Win32.OpenFileDialog { Title = "The edited save (exported by the launcher)", Filter = "Character save|userdata000*|All files|*" };
            if (file.ShowDialog() != true) return;
            var plan = System.Text.Json.JsonSerializer.Deserialize<EditorPlan>(await SeamlessSession.SaveTool(S, "editor-check", file.FileName))!;
            var kind = plan.kind == "party" ? "party save" : "single-player save";
            var notes = string.Concat(plan.notes.Select(n => "\n- " + n));
            if (!Prompt.Confirm("Import from save editor",
                    $"Replace slot {plan.slot + 1} of your {kind} ({plan.target}) with the edited file?{notes}\n\nYour current saves are copied to user\\save_backups first.", "Import"))
                return;
            Prompt.Tell("Import from save editor", await SeamlessSession.SaveTool(S, "editor-import", file.FileName));
        }
        catch (Exception e) { Prompt.Tell("Import from save editor", e.Message); }
    }
    private async Task SaveAction(string action)
    {
        try { Prompt.Tell("Party saves", await SeamlessSession.SaveTool(S, action)); }
        catch (Exception e) { Prompt.Tell("Party saves", e.Message); }
    }
}
