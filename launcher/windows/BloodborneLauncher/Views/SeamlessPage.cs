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

    public SeamlessPage() : base("Seamless co-op",
        "Play the campaign together with visy's seamless co-op. The host shares a party code; everyone uses the same build, gameplay mods and party size.")
    {
        Group("Party");
        SettingCombo(SymbolRegular.People24, "Mode", "Host starts the party; Join connects to the host.",
            new[] { new Choice("Single player", ""), new Choice("Host", "host"), new Choice("Join", "join") }, "party_mode");
        TextRow(SymbolRegular.Person24, "Your name", "1–16 letters, numbers, underscores or hyphens.", "Hunter",
            () => SeamlessSettings.Name(S), v => S.Set("party_name", v));
        TextRow(SymbolRegular.Link24, "Host code or address", "Paste the host's Internet code, LAN code or host:port.", "BBP3-…",
            () => S.Str("party_code"), v => S.Set("party_code", v));
        var password = new PasswordBox { Width = 300, Password = S.PartyPassword, PlaceholderText = "Optional party password", PlaceholderEnabled = true };
        password.PasswordChanged += (_, _) => { S.PartyPassword = password.Password; Ws.SaveSoon(); };
        Add(Card(SymbolRegular.Password24, "Password", "Use the same password on every PC. Stored encrypted for your Windows user.", password, out _));
        ComboRow(SymbolRegular.People24, "Party size", "Host and guests use the same limit. Five players is experimental.",
            new[] { new Choice("2", "2"), new Choice("3", "3"), new Choice("4", "4"), new Choice("5 (experimental)", "5") },
            () => S.Str("party_max") is { Length: > 0 } n ? n : "3", v => S.Set("party_max", v));

        Group("Connection");
        TextRow(SymbolRegular.Router24, "Party port", "TCP and UDP. Default: 9307.", "9307", () => S.Str("party_port"), v => S.Set("party_port", v), width: 120);
        ToggleRow(SymbolRegular.Router24, "UPnP", "Map the host's port automatically.", () => SeamlessSettings.Flag(S, "party_upnp"), v => S.Set("party_upnp", v));
        ToggleRow(SymbolRegular.Globe24, "DHT rendezvous", "Optional NAT traversal through the public BitTorrent DHT. Enable on every PC to use it.",
            () => SeamlessSettings.Flag(S, "party_rendezvous", false), v => S.Set("party_rendezvous", v));
        TextRow(SymbolRegular.Server24, "STUN server", "Empty uses stun.l.google.com:19302. Use off to disable.", "Default",
            () => S.Str("party_stun"), v => S.Set("party_stun", v));
        TextRow(SymbolRegular.Globe24, "Host public address", "Optional host address override, including a VPN address.", "Automatic",
            () => S.Str("party_public_addr"), v => S.Set("party_public_addr", v));
        _codes = Text("", "Faint");
        _codes.TextWrapping = TextWrapping.Wrap;
        Body.Children.Add(_codes);
        Add(Card(SymbolRegular.Copy24, "Share party codes", "Available after the host starts. Use the LAN code on the same router.",
            SmallButton("Copy codes", SymbolRegular.Copy24, (_, _) => Clipboard.SetText(SeamlessSettings.Codes(S))), out _));

        _status = Text("Start the game to see party status.", "Faint");
        _status.TextWrapping = TextWrapping.Wrap;
        Body.Children.Add(_status);
        foreach (var (title, action) in new[] { ("Leave party", "leave"), ("Rejoin party", "rejoin") })
        {
            var command = action;
            Add(Card(SymbolRegular.People24, title, null,
                SmallButton(title, SymbolRegular.People24, (_, _) => {
                    try { SeamlessSession.Command(S, command); }
                    catch (Exception e) { Prompt.Tell("Party", e.Message); }
                }), out _));
        }
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
            }), out _));

        Add(Card(SymbolRegular.Link24, "Guest connection code", "Guests can share this with a host when NAT prevents a direct connection.",
            SmallButton("Copy guest code", SymbolRegular.Copy24, (_, _) => {
                var code = SeamlessSession.Read(S)?.guest_code;
                if (string.IsNullOrEmpty(code)) Prompt.Tell("Party", "No guest connection code is available yet.");
                else Clipboard.SetText(code);
            }), out _));
        Add(Card(SymbolRegular.Link24, "Connect a guest", "Host: copy the guest's BBG1 code first, then add it here.",
            SmallButton("Add copied guest code", SymbolRegular.Link24, (_, _) => {
                try {
                    var code = Clipboard.GetText();
                    if (SeamlessSession.Read(S)?.role != "host" || code.Length > 3500 || !code.Contains("BBG1", StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException("Host a party and copy the guest's BBG1 connection code first.");
                    SeamlessSession.Command(S, "add_guest_code", code);
                } catch (Exception e) { Prompt.Tell("Party", e.Message); }
            }), out _));

        Group("Campaign rules");
        ComboRow(SymbolRegular.Play24, "First join", "Each new character normally completes its own prologue first.",
            new[] { new Choice("After prologue", "prologue_solo"), new Choice("Immediately", "immediate") },
            () => S.Str("party_start") == "immediate" ? "immediate" : "prologue_solo", v => S.Set("party_start", v));
        ComboRow(SymbolRegular.People24, "Guest Insight", null,
            new[] { new Choice("Same as host", "parity"), new Choice("Full bonus", "full"), new Choice("Game's own", "0") },
            () => S.Str("party_guest_insight") is { Length: > 0 } n ? n : "parity", v => S.Set("party_guest_insight", v));
        foreach (var option in SeamlessSettings.Switches)
        {
            var key = option.Key;
            ToggleRow(SymbolRegular.People24, option.Label, null, () => SeamlessSettings.Flag(S, key), v => S.Set(key, v));
        }
        foreach (var (key, label, fallback) in new[] {
            ("party_rot", "Death rot", true), ("party_share_lamps", "Share lamps", true),
            ("party_guest_lamps", "Guests can use lamps", true), ("party_friendly_fire", "Friendly fire / PvP", false),
            ("party_npc_talk", "Share nearby NPC dialogue", true), ("party_pings", "Party pings", true),
            ("party_hud", "Party health display", true), ("party_name_tags", "Player name tags", true),
            ("party_toasts", "Join, leave and travel notices", true), ("party_through_walls", "Player outlines through walls", false) })
        {
            var field = key;
            ToggleRow(SymbolRegular.People24, label, null, () => Ini.Get(field, fallback ? "1" : "0") == "1", v => Ini.Set(field, v ? "1" : "0"));
        }
        foreach (var (key, label, value) in new[] {
            ("party_boss_hp", "Boss HP per guest (%)", 150), ("party_boss_damage", "Boss damage per guest (%)", 10),
            ("party_boss_poise", "Boss poise per guest (%)", 100), ("party_enemy_hp", "Enemy HP per guest (%)", 50),
            ("party_enemy_damage", "Enemy damage per guest (%)", 0), ("party_enemy_poise", "Enemy poise per guest (%)", 0),
            ("party_npc_talk_range", "NPC dialogue range (m)", 25), ("party_overhead", "Name tag detail (0–4)", 3) })
        {
            var field = key;
            TextRow(SymbolRegular.Settings24, label, null, value.ToString(), () => Ini.Get(field, value.ToString()), v => Ini.Set(field, v), width: 120);
        }

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
    }
    private async Task SaveAction(string action)
    {
        try { Prompt.Tell("Party saves", await SeamlessSession.SaveTool(S, action)); }
        catch (Exception e) { Prompt.Tell("Party saves", e.Message); }
    }
}
