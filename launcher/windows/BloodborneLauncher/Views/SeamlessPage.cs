using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using TextBlock = System.Windows.Controls.TextBlock;
using PasswordBox = Wpf.Ui.Controls.PasswordBox;
using MessageBoxButton = System.Windows.MessageBoxButton;
using MessageBoxResult = System.Windows.MessageBoxResult;

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

    public SeamlessPage() : base(L.T("Co-op"),
        L.T("Play the campaign together with visy's seamless co-op. The host shares a party code; everyone uses the same build, gameplay mods and party size."))
    {
        // Single player shows only Party saves. Each mode shows what it needs first and folds the rest: a guest sees
        // the mode, a name, the party list and its own settings; network troubleshooting and the host's finer rules
        // are folded (the settings search still finds and opens them).
        Group(L.T("Party"));
        SettingCombo(SymbolRegular.People24, L.T("Mode"), L.T("Host starts the party; Join connects to the host."),
            new[] { new Choice(L.T("Single player"), ""), new Choice(L.T("Host"), "host"), new Choice(L.T("Join"), "join") }, "party_mode",
            changed: ShowMode, buttons: true);
        var party = Rows(InParty);
        TextRow(SymbolRegular.Person24, L.T("Your name"), L.T("1–16 letters, numbers, underscores or hyphens."), L.T("Hunter"),
            () => SeamlessSettings.Name(S), v => S.Set("party_name", v), party);
        // The connection card (Host only): the route the party uses, its state and the address to give friends. The
        // adapters are read off the UI thread (RefreshRoute); the address is shown on this screen only. Join mode
        // says which networks the party list hears instead (ListenLine).
        var host = Rows(Hosting);
        _copyAddress = SmallButton(L.T("Copy"), SymbolRegular.Copy24, (_, _) => { if (_route.Address.Length > 0) Copy(_route.Address); });
        Add(Card(SymbolRegular.Router24, L.T("Your connection"),
            L.T("How the party reaches you: your VPN (Radmin VPN, Hamachi, ZeroTier, Tailscale) when one is found, else your local network. Friends on the same VPN or network can join with the address shown here. It is only shown on this screen."),
            _copyAddress, out _routeHint, wide: true), host);
        _hostPassword = PasswordRow(host, () => S.PartyPassword, v => S.PartyPassword = v,
            L.T("Use the same password on every PC. Stored encrypted for your Windows user."));
        _hostSize = PartySizeRow(host, L.T("Host and guests use the same limit. Five players is experimental."));
        // party_vpn in bbport.ini (auto / an adapter name / off), the same value as the game's System > Party network row.
        ComboRow(SymbolRegular.Router24, L.T("Host over"), L.T("Automatic takes your Radmin VPN, Hamachi, Tailscale or ZeroTier when one is up, else your local network. VPN: friends in that VPN join with the address above. Internet: friends join with the Internet party code. Read when the game starts."),
            new[] { new Choice(L.T("Automatic"), "auto"), new Choice(L.T("VPN"), "vpn"), new Choice(L.T("Internet (party code)"), "off") },
            () => NetworkRoute.HostOverOf(Ini.Get("party_vpn", "auto")) switch { HostOver.Automatic => "auto", HostOver.Internet => "off", _ => "vpn" },
            v => Ini.Set("party_vpn", NetworkRoute.PrefFor(v switch { "auto" => HostOver.Automatic, "off" => HostOver.Internet, _ => HostOver.Vpn },
                _adapters, Ini.Get("party_vpn", "auto"))),
            host, changed: UpdateRoute, buttons: true);
        _hostOverNote = Text("", "Faint");
        _hostOverNote.TextWrapping = TextWrapping.Wrap;
        host.Children.Add(_hostOverNote);
        // The VPN to host over, shown only when more than one is up (with one, VPN takes it).
        _vpnPick = new System.Windows.Controls.ComboBox { Width = 280, VerticalAlignment = VerticalAlignment.Center };
        _vpnPick.SelectionChanged += (_, _) => {
            if (_syncingVpn || _vpnPick.SelectedItem is not string label) return;
            Ini.Set("party_vpn", label);
            Ws.SaveSoon();
            UpdateRoute();
        };
        _vpnPickRow = Add(Card(SymbolRegular.Router24, L.T("VPN"), L.T("More than one VPN is up: the one friends are in."), _vpnPick, out _), host);
        ToggleRow(SymbolRegular.People24, L.T("Show my party to friends"), L.T("While you host, the game announces the party's name, level and players on your VPN and local network, so friends see it under Parties on your network when they join. Never the password or your address. Takes effect at once."),
            () => Ini.Get("party_discovery", "1") == "1", v => Ini.Set("party_discovery", v ? "1" : "0"), host);
        Add(Card(SymbolRegular.Copy24, L.T("Share party codes"), L.T("Available after the host starts. Use the LAN code on the same router."),
            SmallButton(L.T("Copy codes"), SymbolRegular.Copy24, (_, _) => Copy(SeamlessSettings.Codes(S))), out _codes), host);
        Add(Card(SymbolRegular.Info24, L.T("Check my connection"),
            L.T("Explains this PC's last session problems from its logs. Names and IPs are hidden; the reply is copied to the clipboard."),
            SmallButton(L.T("Check"), SymbolRegular.Info24, (_, _) => CheckConnection()), out _), host);
        Add(Card(SymbolRegular.Router24, L.T("Fix firewall issues"),
            L.T("Allows the party port (TCP and UDP) and party discovery through the Windows Firewall on every network type. Shows you the commands first."),
            SmallButton(L.T("Fix"), SymbolRegular.Wrench24, (_, _) => FixFirewall()), out _), host);
        // The Join list: parties announced on the VPN or local network (party discovery, UDP 9308). The listener runs
        // on its own thread while this tab is open in Join mode; the timer only reads its snapshot.
        var found = Rows(Joining);
        Add(Card(SymbolRegular.People24, L.T("Parties on your network"),
            L.T("Hosts on your VPN (Radmin VPN, Hamachi, ZeroTier) or local network that left Show my party to friends on. Join fills in the host's address and party size. The name, level and players come from the host's announcement. Nothing is listed while this tab is closed."),
            SmallButton(L.T("Refresh"), SymbolRegular.ArrowClockwise24, (_, _) => { _book.Clear(); RefreshRoute(); StartListener(restart: true); RefreshParties(); }),
            out _partiesHint), found);
        _partyList = new StackPanel();
        found.Children.Add(_partyList);
        // Joining by hand, folded under the list. Its password is the one remembered for the code or address typed
        // here (Join in the list asks for it); there is no party size here: the host's applies.
        _manual = new AdvancedSection(L.T("Join by code or address"), L.T("For a party that is not listed: paste the host's code or address."));
        found.Children.Add(_manual);
        _codeBox = TextRow(SymbolRegular.Link24, L.T("Host code or address"), L.T("Paste the host's Internet code, LAN code or host:port."), "BBP3-…",
            () => S.Str("party_code"), v => { S.Set("party_code", v); SyncSharedRows(); }, _manual.Rows);
        _joinPassword = PasswordRow(_manual.Rows, () => S.JoinPassword(S.Str("party_code")), v => S.SetJoinPassword(S.Str("party_code"), v),
            L.T("The host's party password. Remembered for this code or address, encrypted for your Windows user."));

        // Network settings only matter when the party does not connect: folded, with the mode's own rows.
        var network = Fold(L.T("Network and troubleshooting"), InParty, L.T("Ports, NAT traversal and connection codes: only needed when the party does not connect."));
        party = Rows(InParty, network);
        TextRow(SymbolRegular.Router24, L.T("Party port"), L.T("TCP and UDP. Default: 9307."), "9307", () => S.Str("party_port"), v => S.Set("party_port", v), party, width: 120);
        ToggleRow(SymbolRegular.Router24, L.T("UPnP"), L.T("Map the host's port automatically."), () => SeamlessSettings.Flag(S, "party_upnp"), v => S.Set("party_upnp", v), Rows(Hosting, network));
        party = Rows(InParty, network);
        ToggleRow(SymbolRegular.Globe24, L.T("DHT rendezvous"), L.T("Optional NAT traversal through the public BitTorrent DHT. Enable on every PC to use it."),
            () => Ini.Get("party_rendezvous", "0") == "1", v => Ini.Set("party_rendezvous", v ? "1" : "0"), party);
        // Kept in bbport.ini like the game's System > Party network rows, so both show the same value.
        ToggleRow(SymbolRegular.People24, L.T("Relay for party members"), L.T("A member that cannot reach the host joins through you (party members only, capped upload)."),
            () => Ini.Get("party_member_relay", "1") == "1", v => Ini.Set("party_member_relay", v ? "1" : "0"), party);
        ToggleRow(SymbolRegular.Globe24, L.T("IPv6"), L.T("Try IPv6 when both PCs have it. A host without an open port then puts its IPv6 address into the party code."),
            () => Ini.Get("party_ipv6", "0") == "1", v => Ini.Set("party_ipv6", v ? "1" : "0"), party);
        TextRow(SymbolRegular.Server24, L.T("STUN server"), L.T("Empty uses stun.l.google.com:19302. Use off to disable."), L.T("Default"),
            () => S.Str("party_stun"), v => S.Set("party_stun", v), party);
        host = Rows(Hosting, network);
        TextRow(SymbolRegular.Globe24, L.T("Host public address"), L.T("Leave on Automatic unless guests reach you another way: your VPN address (Radmin VPN, Tailscale, ZeroTier), or your public IP or DNS name when you forwarded the port yourself (host or host:port). The party code then carries it."), L.T("Automatic"),
            () => S.Str("party_public_addr"), v => S.Set("party_public_addr", v), host);
        Add(Card(SymbolRegular.Link24, L.T("Connect a guest"), L.T("Host: copy the guest's BBG1 code first, then add it here."),
            SmallButton(L.T("Add copied guest code"), SymbolRegular.Link24, (_, _) => {
                try {
                    var code = Clipboard.GetText();
                    if (SeamlessSession.Read(S)?.role != "host" || code.Length > 3500 ||
                        !(code.Contains("BBG1", StringComparison.OrdinalIgnoreCase) || code.Contains("BBG6", StringComparison.OrdinalIgnoreCase)))
                        throw new InvalidOperationException(L.T("Host a party and copy the guest's BBG1 connection code first."));
                    SeamlessSession.Command(S, "add_guest_code", code);
                } catch (Exception e) { Prompt.Tell(L.T("Party"), e.Message); }
            }), out _), host);
        var join = Rows(Joining, network);
        Add(Card(SymbolRegular.Link24, L.T("Guest connection code"), L.T("Guests can share this with a host when NAT prevents a direct connection."),
            SmallButton(L.T("Copy guest code"), SymbolRegular.Copy24, (_, _) => {
                var code = SeamlessSession.Read(S)?.guest_code;
                if (string.IsNullOrEmpty(code)) Prompt.Tell(L.T("Party"), L.T("No guest connection code is available yet."));
                else Copy(code);
            }), out _), join);
        Add(Card(SymbolRegular.Link24, L.T("Join through a member"), L.T("Guest: when the host cannot be reached but another member can, copy that member's guest code and add it here."),
            SmallButton(L.T("Add copied member code"), SymbolRegular.Link24, (_, _) => {
                try {
                    var code = Clipboard.GetText();
                    if (SeamlessSession.Read(S)?.role != "guest" || code.Length > 3500 || !code.Contains("BBG1", StringComparison.OrdinalIgnoreCase))
                        throw new InvalidOperationException(L.T("Join a party and copy another member's BBG1 guest code first."));
                    SeamlessSession.Command(S, "add_relay_code", code);
                } catch (Exception e) { Prompt.Tell(L.T("Party"), e.Message); }
            }), out _), join);

        // The running session: its state, then Leave / Rejoin side by side, and the host's Remove.
        _byMode.Add((Group(L.T("Session")), InParty));
        party = Rows(InParty);
        _status = Text(L.T("Start the game to see party status."), "Faint");
        _status.TextWrapping = TextWrapping.Wrap;
        party.Children.Add(_status);
        var commands = new List<UIElement>();
        foreach (var (title, action, icon) in new[] { (L.T("Leave party"), "leave", SymbolRegular.ArrowExit20), (L.T("Rejoin party"), "rejoin", SymbolRegular.ArrowSync24) })
        {
            var command = action;
            var button = SmallButton(title, icon, (_, _) => {
                try { SeamlessSession.Command(S, command); }
                catch (Exception e) { Prompt.Tell(L.T("Party"), e.Message); }
            });
            button.Margin = new Thickness(commands.Count > 0 ? 8 : 0, 0, 0, 0);
            commands.Add(button);
        }
        Add(Card(SymbolRegular.People24, L.T("Party"), L.T("Leave goes back to your own world; Rejoin connects to the party again."), Row(commands.ToArray()), out _), party);
        Add(Card(SymbolRegular.People24, L.T("Remove a member"), L.T("Host only. Enter the member's exact party name."),
            SmallButton(L.T("Choose member"), SymbolRegular.People24, (_, _) => {
                var status = SeamlessSession.Read(S);
                if (status?.role != "host") { Prompt.Tell(L.T("Party"), L.T("Only the host can remove members.")); return; }
                var members = status.members.Where(m => !m.local).ToArray();
                if (members.Length == 0) { Prompt.Tell(L.T("Party"), L.T("No remote members to remove.")); return; }
                var choice = Prompt.Choose(L.T("Remove member"), L.T("Choose a member to disconnect."), members.Select(m => m.name).ToArray());
                if (choice >= 0) {
                    try { SeamlessSession.Command(S, "kick", members[choice].name); }
                    catch (Exception e) { Prompt.Tell(L.T("Party"), e.Message); }
                }
            }), out _), Rows(Hosting));

        // Party settings as in the game's System > Party rules / My party pages (visy 0.7.3): the host's
        // rules for the whole party (the ones most hosts change shown, the rest folded by topic), then what each
        // player chooses for itself. Joining: the rules are the host's and reach you on connect; the start-up rules
        // a guest must match are taken from the host's refusal by the main window (SeamlessSettings.TakeHostStartupRules).
        _byMode.Add((Group(L.T("Party rules")), Hosting));
        host = Rows(Hosting);
        host.Children.Add(Text(L.T("The host's rules for the whole party. When you join, the host's count and yours are kept for when you host (in the game: System > Party rules)."), "Faint"));
        host.Children.Add(Card(SymbolRegular.Settings24, L.T("Co-op setup"),
            L.T("A few questions that set the difficulty rules below: guest health, dying, supplies, scaling and rot."),
            SmallButton(L.T("Run Setup Wizard"), SymbolRegular.Play24, (_, _) => CoopWizard.Open()), out _));
        IniToggles(host,
            ("party_kill_record", L.T("Enemies stay dead"), true), ("party_open_world", L.T("Remove multiplayer barriers"), true),
            ("party_share_lamps", L.T("Shared lamps"), true), ("party_gather", L.T("Host can gather the party"), true),
            ("party_guest_respawn", L.T("Guest respawn"), true), ("party_level_sync", L.T("Level sync"), true),
            ("party_friendly_fire", L.T("Friendly fire / PvP"), false), ("party_pvp_full_hp", L.T("Full health in PvP"), true));
        IniToggles(Fold(L.T("World and story"), Hosting),
            ("party_story_mirror", L.T("Cutscenes for every player"), true), ("party_npc_mirror", L.T("NPCs as in the host's world"), true),
            ("party_covenant_per_player", L.T("Covenant outcomes per player"), true),
            ("party_npc_fights", L.T("NPC hunters fight the whole party"), true),
            ("party_boss_share", L.T("Bosses share attention"), true),
            ("party_any_kidnap", L.T("Anyone can be abducted"), true),
            ("party_travel_vote", L.T("Party votes on travel"), false),
            ("party_guests_bring_lamps", L.T("Guests bring their lamps"), false));
        IniToggles(Fold(L.T("Guests"), Hosting),
            ("party_guest_revive_inplace", L.T("Get up without a loading screen"), true),
            ("party_guest_refill", L.T("Refill guest supplies"), true), ("party_chalice_rejoin", L.T("Stay in Chalice Dungeons"), true),
            ("party_phantom_hp", L.T("Phantom health cut"), true), ("party_boss_drops", L.T("Boss drops for guests"), true));
        IniNumbers(Fold(L.T("Enemy and boss scaling"), Hosting, L.T("How much tougher enemies and bosses get for each guest.")),
            ("party_boss_hp", L.T("Boss HP per guest (%)"), 150), ("party_boss_damage", L.T("Boss damage per guest (%)"), 10),
            ("party_boss_poise", L.T("Boss poise per guest (%)"), 100), ("party_enemy_hp", L.T("Enemy HP per guest (%)"), 50),
            ("party_enemy_damage", L.T("Enemy damage per guest (%)"), 0), ("party_enemy_poise", L.T("Enemy poise per guest (%)"), 0));
        IniToggles(Fold(L.T("Rot on death"), Hosting),
            ("party_rot", L.T("Rot on death"), false), ("party_rot_cleanse_boss", L.T("Boss kill cleanses rot"), true),
            ("party_rot_cleanse_killer", L.T("Killing your killer cleanses its rot"), true),
            ("party_rot_cleanse_echoes", L.T("Retrieving Blood Echoes cleanses its rot"), true));
        host = Fold(L.T("Start-up rules"), Hosting, L.T("Read when the game starts. A guest with other values is refused and takes the host's."));
        StartupRows(host);
        IniToggles(host, ("party_skip_prologue", L.T("Skip the prologue (new characters)"), false));

        _byMode.Add((Group(L.T("My party")), InParty));
        party = Rows(InParty);
        party.Children.Add(Text(L.T("Your own party settings: your screen, what you get as a guest, story sync (in the game: System > My party)."), "Faint"));
        IniToggles(party,
            ("party_hud", L.T("Party HUD"), true), ("party_name_tags", L.T("Party name tags"), true), ("party_pings", L.T("Pings"), true),
            ("party_toasts", L.T("Join, leave and travel notices"), true), ("party_item_notices", L.T("Shared item notices"), true), ("party_npc_talk", L.T("Share NPC dialogue"), true));
        foreach (var option in SeamlessSettings.Switches)
        {
            var key = option.Key;
            ToggleRow(SymbolRegular.People24, option.Label, PartyInfo(key), () => SeamlessSettings.Flag(S, key), v => S.Set(key, v), party);
        }
        party = Fold(L.T("Display"), InParty);
        IniToggles(party,
            ("party_hud_ping", L.T("Ping in the party list"), true),
            ("party_conn_warnings", L.T("Connection warnings"), true), ("party_through_walls", L.T("Show party through walls"), false),
            ("party_glow_off", L.T("Companions without glow"), true));
        IniNumbers(party, ("party_overhead", L.T("Name tag detail (0–4)"), 3), ("party_npc_talk_range", L.T("NPC dialogue range (m)"), 25));
        party = Fold(L.T("Progress and items"), InParty, L.T("What reaches your own save. Takes effect on the next game start."));
        IniToggles(party, SeamlessSettings.GameSwitches.Where(o => SeamlessSettings.OwnSwitches.Contains(o.Key)).Select(o => (o.Key, o.Label, true)).ToArray());
        IniToggles(party,
            ("party_key_view", L.T("Own key items as a guest"), true), ("party_key_items_summoned", L.T("Key items at once"), true));

        Group(L.T("Party saves"));
        Body.Children.Add(Text(L.T("Seamless play uses its own savedata_party folder. Automatic snapshots keep single-player and party progress recoverable."), "Faint"));
        ComboRow(SymbolRegular.Timer24, L.T("Backup interval"), L.T("Minutes; session-start snapshots are always kept."),
            new[] { new Choice(L.T("5 min"), "5"), new Choice(L.T("10 min"), "10"), new Choice(L.T("30 min"), "30"), new Choice(L.T("At start"), "0") },
            () => S.Str("party_backup_minutes") is { Length: > 0 } n ? n : "10", v => S.Set("party_backup_minutes", v));
        ComboRow(SymbolRegular.Save24, L.T("Backups to keep"), null,
            new[] { new Choice("10", "10"), new Choice("20", "20"), new Choice("50", "50"), new Choice("100", "100") },
            () => S.Str("party_backup_keep") is { Length: > 0 } n ? n : "10", v => S.Set("party_backup_keep", v));
        Add(Card(SymbolRegular.Folder24, L.T("Save and backup folder"), null,
            SmallButton(L.T("Open"), SymbolRegular.Folder24, (_, _) => {
                var folder = GameEnvironment.UserDir(S); Directory.CreateDirectory(folder);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", folder) { UseShellExecute = true });
            }), out _));
        Add(Card(SymbolRegular.Copy24, L.T("Copy single-player progress into party saves"), L.T("Existing party saves are backed up before replacement."),
            SmallButton(L.T("Copy progress"), SymbolRegular.Copy24, async (_, _) => {
                if (!Prompt.Confirm(L.T("Copy progress"), L.T("Copy this profile's single-player saves into its party save folder? Existing party progress will be backed up."), L.T("Copy"))) return;
                await SaveAction("copy");
            }), out _));
        Add(Card(SymbolRegular.Save24, L.T("Undo last party session"), L.T("Restores both save folders and first backs up their current contents."),
            SmallButton(L.T("Review restore"), SymbolRegular.Save24, async (_, _) => {
                try {
                    var description = await SeamlessSession.SaveTool(S, "describe");
                    if (Prompt.Confirm(L.T("Undo party session"), description, L.T("Restore"))) await SaveAction("undo");
                } catch (Exception e) { Prompt.Tell(L.T("Party saves"), e.Message); }
            }), out _));
        Add(Card(SymbolRegular.Save24, L.T("Check party saves"), L.T("Checks save structure and compares progress with the last session snapshot."),
            SmallButton(L.T("Check"), SymbolRegular.Save24, async (_, _) => await SaveAction("check")), out _));
        // visy 0.7.3 (scripts/save_editor.py): a save last written as a guest in another world lacks the own
        // world's state that save editors look for; the export adds it and the import takes it out again.
        Add(Card(SymbolRegular.ArrowExportUp24, L.T("Export for save editor"), L.T("A copy of one character save that save editors can open. Your save is not changed."),
            SmallButton(L.T("Export"), SymbolRegular.ArrowExportUp24, async (_, _) => await ExportForEditor()), out _));
        Add(Card(SymbolRegular.ArrowImport24, L.T("Import from save editor"), L.T("Puts an edited export back into its slot. Your saves are backed up first."),
            SmallButton(L.T("Import"), SymbolRegular.ArrowImport24, async (_, _) => await ImportFromEditor()), out _));
        Add(Card(SymbolRegular.Save24, L.T("Save editor"), L.T("Saves are backed up before every change. Close the game first."), SmallButton(L.T("Save editor"), SymbolRegular.Save24, (_, _) => new SaveEditorWindow(S).ShowDialog()), out _));
        ComboRow(SymbolRegular.Document24, L.T("Party log detail"), L.T("Verbose and trace logging add overhead."),
            new[] { new Choice(L.T("Normal"), ""), new Choice(L.T("Verbose"), "verbose"), new Choice(L.T("Trace"), "trace") },
            () => S.Str("party_log"), v => S.Set("party_log", v));

        _timer.Tick += (_, _) => {
            SetHint(_codes, SeamlessSettings.Codes(S));
            if (++_ticks % 5 == 0) RefreshRoute();
            RefreshParties();
            var status = SeamlessSession.Read(S);
            _status.Text = status == null ? (Ws.Game.Running ? L.T("Waiting for party status…") : L.T("Game stopped.")) :
                $"{PartyRole(status.role)}：{PartyState(status.state)}  {status.detail}\n{status.network}\n" +
                string.Join("\n", status.members.Select(m => $"{m.name}：{(m.in_world ? L.T("in host world") : m.connected ? L.T("connected") : L.T("disconnected"))}  {(m.ping_ms < 0 ? "" : m.ping_ms + " ms")}  {m.area}")) +
                $"\n{status.@event}\n{status.alert}";
        };
        Loaded += (_, _) => { SetHint(_codes, SeamlessSettings.Codes(S)); _timer.Start(); RefreshRoute(); StartListener(); };
        Unloaded += (_, _) => { _timer.Stop(); _listener?.Dispose(); _listener = null; };
        ShowMode();
    }

    /// <summary>The first time this tab is shown, the co-op setup wizard (after the page is on screen).</summary>
    public override void OnShown()
    {
        if (S.Bool(CoopPresets.DoneKey)) return;
        S.Set(CoopPresets.DoneKey, true);
        Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, CoopWizard.Open);
    }
    private PartyDiscovery.Book _book = new();
    private PartyDiscovery.Listener? _listener;
    private StackPanel _partyList = null!;
    private TextBlock _partiesHint = null!;
    private Wpf.Ui.Controls.TextBox _codeBox = null!;
    private AdvancedSection _manual = null!;
    private PasswordBox _joinPassword = null!, _hostPassword = null!;
    private System.Windows.Controls.ComboBox _hostSize = null!;
    private string _partySignature = "\0";
    private string _joinNote = "";  // the list's status line after Join: "Restart the game to join ..."
    private long _listenSinceMs;
    private const int ListenGraceMs = 4000;  // a host announces every 1.5 s: an empty list before this is not yet news

    private PasswordBox PasswordRow(Panel into, Func<string> get, Action<string> set, string hint)
    {
        var box = new PasswordBox { Width = 300, Password = get(), PlaceholderText = L.T("Party password"), PlaceholderEnabled = true };
        box.PasswordChanged += (_, _) => { if (box.Password != get()) { set(box.Password); Ws.SaveSoon(); } };
        Add(Card(SymbolRegular.Password24, L.T("Password"), hint, box, out _), into);
        return box;
    }

    private System.Windows.Controls.ComboBox PartySizeRow(Panel into, string hint) =>
        (System.Windows.Controls.ComboBox)ComboRow(SymbolRegular.People24, L.T("Party size"), hint, PartySizes(), PartySize, v => S.Set("party_max", v), into);

    private static Choice[] PartySizes() =>
        new[] { new Choice("2", "2"), new Choice("3", "3"), new Choice("4", "4"), new Choice(L.T("5 (experimental)"), "5") };
    private static string PartySize() => S.Str("party_max") is { Length: > 0 } n ? n : "3";

    /// <summary>Copies text; a clipboard another program holds open is reported instead of crashing the launcher.</summary>
    private static void Copy(string text)
    {
        if (!ClipboardText.Set(text))
            Prompt.Tell(L.T("Party"), L.T("Could not copy: another program is using the clipboard. Try again."));
    }

    /// <summary>Shows the firewall commands. Yes runs them in an administrator PowerShell, No copies them to paste by hand.</summary>
    private static void FixFirewall()
    {
        var port = int.TryParse(S.Str("party_port"), out var p) && p is >= 1 and <= 65535 ? p : NetworkRoute.DefaultPort;
        var script = FirewallFix.Script(port);
        var answer = System.Windows.MessageBox.Show(
            L.T("These lines add Windows Firewall rules that let friends reach your party and remove rules that block the game. Nothing else is changed:") + "\n\n" + script + "\n\n" +
            L.T("Yes: run them now (Windows asks for administrator permission).") + "\n" +
            L.T("No: copy them, then paste them into PowerShell opened as administrator yourself."),
            L.T("Fix firewall issues"), MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
        if (answer == MessageBoxResult.No) Copy(script);
        else if (answer == MessageBoxResult.Yes) FirewallFix.RunElevated(port);
    }

    /// <summary>Runs the connection doctor on this PC's last and previous session logs (names and IPs hidden).
    /// With no log yet the script prints its own usage.</summary>
    private static void CheckConnection()
    {
        var bat = Paths.NetDoctorBat;
        var logs = new[] { GameRunner.SessionLog, Path.Combine(Path.GetDirectoryName(GameRunner.SessionLog)!, "previous_session.log") }
            .Where(File.Exists);
        var args = string.Join(" ", new[] { "--redactnames", "--redactips" }.Concat(logs.Select(log => $"\"{log}\"")));
        var info = new System.Diagnostics.ProcessStartInfo(bat, args) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(bat)! };
        System.Diagnostics.Process.Start(info)?.Dispose();
    }

    /// <summary>The password has a Host row and a Join row: show the stored values (the Join row's password is the
    /// one remembered for the code or address typed there). The party size is the host's alone: a guest takes the host's.</summary>
    private void SyncSharedRows()
    {
        foreach (var (box, password) in new[] { (_joinPassword, S.JoinPassword(S.Str("party_code"))), (_hostPassword, S.PartyPassword) })
            if (box.Password != password) box.Password = password;
        var index = Choices.IndexOf(PartySizes(), PartySize());
        if (_hostSize.SelectedIndex != index) _hostSize.SelectedIndex = index;
    }

    /// <summary>Join in the list: asks for the host's password (filled with the one last used for that host), then
    /// sets the host's address and party size and starts the game as Play starts it. A running game is never stopped.</summary>
    private async void JoinParty(FoundParty party)
    {
        _joinNote = "";
        if (Ws.Game.Running)
        {
            _joinNote = L.T("Restart the game to join ") + party.Info.Name + ".";
            _partySignature = "\0";
            RefreshParties();
            return;
        }
        var password = Prompt.Secret(L.T("Join ") + party.Info.Name, L.T("Type the host's party password."),
            new Prompt.Field(L.T("Password"), S.JoinPassword(party.Address), L.T("Party password")), L.T("Join"));
        if (password == null) return;
        if (password.Trim().Length == 0)
        {
            Prompt.Tell(L.T("No password"), L.T("Every party has a password: ask the host for it."));
            return;
        }
        S.SetJoinPassword(party.Address, password);
        _codeBox.Text = party.Address;  // saved by the row
        if (PartyDiscovery.PartySize(party.Info.Max) is { } size) S.Set("party_max", size);
        Ws.SaveSoon();
        SyncSharedRows();
        _partySignature = "\0";
        RefreshParties();
        await MainWindow.Instance!.PlayOrStopAsync();
    }

    /// <summary>The networks the list hears parties on, for the list's status line.</summary>
    private string ListenLine()
    {
        var (nets, tailscale) = PartyDiscovery.Networks(_adapters);
        var line = nets.Count > 0 ? L.T("Listening on ") + string.Join(", ", nets.Select(n => n == "LAN" ? L.T("Local network") : n))
                                  : L.T("No VPN or local network to listen on.");
        return tailscale ? line + "\n" + L.T("Parties on Tailscale are not listed: paste the host's address under Join by code or address.") : line;
    }

    /// <summary>Listens while the page is shown in Join mode; stops otherwise (the thread ends within half a second).</summary>
    private void StartListener(bool restart = false)
    {
        var wanted = IsLoaded && S.Str("party_mode") == "join";
        if (_listener != null && (!wanted || restart)) { _listener.Dispose(); _listener = null; }
        if (!wanted || _listener != null) return;
        var own = _adapters.Select(a => a.IpText);
        _book = new PartyDiscovery.Book(Environment.GetEnvironmentVariable("BB_PARTY_DISCOVERY_SELF") == "1" ? null : own);
        _listener = new PartyDiscovery.Listener(_book, () => _adapters);
        _listenSinceMs = PartyDiscovery.NowMs;
        _partySignature = "\0";
    }

    /// <summary>Redraws the list from the listener's snapshot, only when it changed.</summary>
    private void RefreshParties()
    {
        if (_listener == null) return;
        var now = PartyDiscovery.NowMs;
        var parties = _book.Snapshot(now);
        var prefix = PartyDiscovery.BuildPrefix(Updater.Current);
        var waited = now - _listenSinceMs > ListenGraceMs;
        var saved = S.Str("party_code");
        var listen = ListenLine();
        var signature = _joinNote + "|" + _listener.Error + "|" + waited + "|" + saved + "|" + listen + "|" +
                        string.Join(";", parties.Select(p => $"{p.Info.Id}/{p.Ip}/{p.Info.Players}/{p.Info.Max}/{p.Info.Level}/{p.Info.Area}/{p.Info.Name}/{p.Info.Password}"));
        if (signature == _partySignature) return;
        _partySignature = signature;
        _partyList.Children.Clear();
        SetHint(_partiesHint, listen + (_joinNote.Length > 0 ? "\n" + _joinNote : "") + "\n" + (
            _listener.Error.Length > 0 ? L.T("Could not listen for parties: ") + _listener.Error + L.T(". Paste the host's code below instead.") :
            parties.Count > 0 ? "" :
            !waited ? L.T("Looking for parties…") :
            L.T("No party found yet. Check that:") + "\n• " + L.T("the host has Show my party to friends on and has started the game") +
            "\n• " + L.T("you are both on the same VPN or local network") + "\n• " + L.T("Windows Firewall allows Nightwatch Co-op on Public networks too (VPN adapters often count as Public)")).TrimEnd('\n'));
        foreach (var party in parties)
        {
            var info = party.Info;
            var state = PartyDiscovery.StateOf(info, prefix);
            var selected = PartyDiscovery.Selected(party, saved);
            var text = (selected ? L.T("Selected") + " · " : "") + L.T("Level ") + (info.Level > 0 ? info.Level.ToString() : "?") + " · " +
                       info.Players + " / " + info.Max + (info.Area.Length > 0 ? " · " + info.Area : "");
            if (state == PartyDiscovery.RowState.OtherBuild) text += "\n" + L.T("This host runs another version of the co-op: update the launcher or ask the host to.");
            var picked = party;
            var join = SmallButton(state switch
                {
                    PartyDiscovery.RowState.Join => L.T("Join"),
                    PartyDiscovery.RowState.Full => L.T("Full"),
                    _ => L.T("Update needed"),
                }, selected ? SymbolRegular.Checkmark24 : SymbolRegular.Link24, (_, _) => JoinParty(picked),
                selected ? ControlAppearance.Primary : ControlAppearance.Secondary);
            join.IsEnabled = state == PartyDiscovery.RowState.Join;
            var row = Card(SymbolRegular.People24, info.Name, null, join, out var hint);
            if (state != PartyDiscovery.RowState.Join) row.Opacity = 0.6;
            SetHint(hint, text);
            _partyList.Children.Add(row);
        }
    }

    private readonly Wpf.Ui.Controls.Button _copyAddress;
    private readonly TextBlock _hostOverNote;
    private readonly System.Windows.Controls.ComboBox _vpnPick;
    private readonly FrameworkElement _vpnPickRow;
    private bool _syncingVpn;
    private readonly TextBlock _routeHint;
    private IReadOnlyList<NetAdapter> _adapters = Array.Empty<NetAdapter>();
    private Route _route = Route.None;
    private bool _adaptersBusy;
    private int _ticks;

    /// <summary>Reads the adapters off the UI thread (the system call can take a moment), then redraws the card.</summary>
    private async void RefreshRoute()
    {
        if (_adaptersBusy) return;
        _adaptersBusy = true;
        try
        {
            _adapters = await Task.Run(NetworkRoute.Enumerate).ConfigureAwait(false);
            await Dispatcher.InvokeAsync(UpdateRoute);
        }
        catch (Exception e) when (e is InvalidOperationException or TaskCanceledException) { }
        finally { _adaptersBusy = false; }
    }

    private void UpdateRoute()
    {
        var port = int.TryParse(S.Str("party_port"), out var p) ? p : NetworkRoute.DefaultPort;
        _route = NetworkRoute.Choose(_adapters, Ini.Get("party_vpn", "auto"), port);
        var label = _route.Label switch { "LAN" => L.T("Local network"), "Internet" => L.T("Internet"), var other => other };
        var lines = new List<string>
        {
            _route.Connected ? label + " · " + L.T("Connected") : L.T("Not connected"),
        };
        if (_route.Address.Length > 0) lines.Add(L.T("Address to share: ") + _route.Address);
        else if (_route.Connected) lines.Add(L.T("No local address to share: use the party code from the Share party codes row."));
        SetHint(_routeHint, string.Join("\n", lines));
        _copyAddress.IsEnabled = _route.Address.Length > 0;

        var pref = Ini.Get("party_vpn", "auto");
        var over = NetworkRoute.HostOverOf(pref);
        var vpns = NetworkRoute.VpnLabels(_adapters);
        _hostOverNote.Text = over switch
        {
            HostOver.Automatic => L.T("Automatic: ") + (!_route.Connected ? L.T("Not connected") : _route.Vpn ? _route.Label :
                _route.Label == "LAN" ? L.T("local network") : L.T("Internet (party code)")),
            HostOver.Internet => L.T("Friends join with the Internet code under Share party codes. Port forwarding (TCP and UDP, the party port) is only needed for a strict NAT."),
            _ => vpns.Count == 0 ? L.T("No VPN is up: start Radmin VPN, Hamachi, ZeroTier or Tailscale.") : L.T("VPN: ") + (_route.Vpn ? _route.Label : pref),
        };
        var offline = NetworkRoute.OfflineVpnLabels(_adapters, pref);
        if (!_route.Vpn && offline.Count > 0)
            _hostOverNote.Text = string.Join(", ", offline) + L.T(" installed but offline (connect it in its app): the usual address is used.");
        _vpnPickRow.Visibility = over == HostOver.Vpn && vpns.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
        _syncingVpn = true;
        if (!_vpnPick.Items.Cast<string>().SequenceEqual(vpns))
        {
            _vpnPick.Items.Clear();
            foreach (var vpn in vpns) _vpnPick.Items.Add(vpn);
        }
        _vpnPick.SelectedItem = vpns.FirstOrDefault(l => l.Contains(pref, StringComparison.OrdinalIgnoreCase) || pref.Contains(l, StringComparison.OrdinalIgnoreCase));
        _syncingVpn = false;
    }

    /// <summary>The host's start-up rules (the game reads them once; a guest must have the host's values).</summary>
    private void StartupRows(Panel into)
    {
        IniToggles(into,
            ("party_guest_lamps", L.T("Guests use lamps"), true), ("party_guest_mark", L.T("Guests can use Hunter's Mark"), true),
            ("party_full_rewards", L.T("Full Blood Echo rewards"), true));
        ComboRow(SymbolRegular.People24, L.T("Insight for bosses"), PartyInfo("party_guest_insight"),
            new[] { new Choice(L.T("Same as host"), "parity"), new Choice(L.T("Full bonus"), "full"), new Choice(L.T("Game's own"), "0") },
            () => SeamlessSettings.StartupValue("party_guest_insight", Ini.Get("party_guest_insight", "parity")),
            v => Ini.Set("party_guest_insight", v), into);
        ComboRow(SymbolRegular.Play24, L.T("Party starts"), PartyInfo("party_start"),
            new[] { new Choice(L.T("After the prologue"), "prologue_solo"), new Choice(L.T("Right away"), "immediate") },
            () => SeamlessSettings.StartupValue("party_start", S.Str("party_start")), v => S.Set("party_start", v), into);
    }

    /// A panel for the next rows (in the page, or in a fold), shown in the modes when accepts (ShowMode).
    private StackPanel Rows(Func<string, bool> when, Panel? into = null)
    {
        var rows = new StackPanel();
        (into ?? Body).Children.Add(rows);
        _byMode.Add((rows, when));
        return rows;
    }
    /// A folded part of the current group, shown in the modes when accepts; its rows go in the panel returned.
    private StackPanel Fold(string title, Func<string, bool> when, string? tip = null)
    {
        var section = new AdvancedSection(title, tip);
        Rows(when).Children.Add(section);
        return section.Rows;
    }
    private void ShowMode()
    {
        var mode = S.Str("party_mode");
        foreach (var (element, when) in _byMode)
            element.Visibility = when(mode) ? Visibility.Visible : Visibility.Collapsed;
        SyncSharedRows();
        UpdateRoute();
        StartListener();
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
        "party_boss_share" => L.T("A boss in a party fight turns on each hunter in reach in turn instead of keeping to the nearest one. The host's rule."),
        "party_any_kidnap" => L.T("A Snatcher grabbing a guest takes the whole party to the Hypogean Gaol, just as when it grabs the host."),
        "party_friendly_fire" => L.T("Party members can hurt each other; being killed by a party member counts as a normal death."),
        "party_boss_drops" => L.T("The item a world boss drops for you also goes to every guest who is there when it dies, once, in their own save. Key and unique items already reach every guest and are not given twice."),
        "party_pvp_full_hp" => L.T("While friendly fire is on, hunters who joined keep their full maximum health (no phantom health cut), so a duel is fair. Switching keeps each hunter's health at the same fraction. Off: the phantom health cut applies in PvP too. The host's rule: guests follow it."),
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
        "party_conn_warnings" => L.T("A notice with the fix when the Windows Firewall, the router or a wrong address keeps the party from working. Each at most once per session. Your own setting, not a host rule."),
        "party_name_tags" => L.T("On: our name plates above companions, never hidden by walls. Off: the game's own plate."),
        "party_pings" => L.T("Touchpad + R3, or the Ping key (default B, under Controls), marks where you look. Everyone sees the mark for 10 s."),
        "party_through_walls" => L.T("A small marker with the distance on each companion on your map, also behind walls."),
        "party_glow_off" => L.T("On: other hunters look like normal hunters on your screen. Off: the game's blue phantom glow."),
        "party_toasts" => L.T("A short notice in a top corner when a hunter joins, leaves or travels."),
        "party_item_notices" => L.T("A short notice when a key or unique item, a weapon or a rune is shared with the party. Never for ordinary pickups. Shows only while join, leave and travel notices are on."),
        "party_npc_talk" => L.T("See and hear the NPC conversations of companions near you (subtitles, choices, gifts)."),
        "party_guest_lamps" => L.T("Guests may use lamps and headstones; the whole party travels with them. Needs a restart."),
        "party_key_view" => L.T("While you are a guest, your own keys and badges show in Key Items; off shows the game's empty list. Takes effect on the next game start."),
        "party_key_items_summoned" => L.T("While you are a guest, a key the host's world gives you is yours at once (needs Own key items as a guest). Off: the key waits until you are back in your own world."),
        "party_npc_talk_range" => L.T("How close a companion has to be for you to see their NPC conversation."),
        "party_overhead" => L.T("What shows above companions in view, only for you: nothing, name, name and level, name and health, name and deaths, or name and ping."),
        "party_grant_bells" => L.T("Gives the Beckoning Bell and Small Resonant Bell after the prologue, for ringing by hand. Takes effect on the next game start."),
        "party_open_world" => L.T("Removes co-op walls on area borders so the party can walk anywhere. The host's rule: guests follow it."),
        "party_guest_respawn" => L.T("A guest who dies comes back at the host's last lamp and is summoned again, instead of being sent home. The host's rule: guests follow it."),
        "party_guest_refill" => L.T("Guests' Blood Vials and Quicksilver Bullets refill when the host rests, dies or travels. The host's rule: guests follow it."),
        "party_chalice_rejoin" => L.T("If a death or forced trip takes a guest out of the host's dungeon, the guest is summoned back. Leaving by choice: use Rejoin. The host's rule."),
        "party_guest_mark" => L.T("A guest's Hunter's Mark asks whether to travel alone or with the party; off uses the game's own behavior. Takes effect on the next game start."),
        "party_full_rewards" => L.T("While you are a guest, receive full Blood Echoes and enemy drops like the host; off uses the game's co-op amounts. Takes effect on the next game start."),
        "party_items" => L.T("Key items and unique items the host picks up or is given (a key, a badge, a gift that exists only once) are given to each guest too, once, in their own world. Ordinary pickups such as vials, bullets and materials are not copied. Takes effect on the next game start."),
        "party_progress" => L.T("Bosses, lamps, shortcuts and key events are copied into every guest's save. Applies to host and guests on the next game start."),
        "party_progress_npc" => L.T("The first NPC quest choice anyone makes applies to every save (needs Shared progression). Takes effect on the next game start."),
        "party_story" => L.T("Guests see the host's cutscenes, time of day and ending. Applies to host and guests on the next game start."),
        "party_story_mirror" => L.T("A guest in the host's world watches cutscenes with the host; off lets the guest watch them later in their own world. The host's rule: guests follow it."),
        "party_guest_revive_inplace" => L.T("A guest who dies gets up at the host's lamp right away, with no loading screen (after a fall: the respawn load). Off: every death takes the respawn loading screen. The host's rule: guests follow it."),
        "party_level_sync" => L.T("As in the game's own co-op: a hunter whose level is above the host's range is scaled down while it plays in the host's world (less maximum health and stamina, less defence and resistance) and gets a blue icon by its name. Anyone can still join at any level. Off: everyone keeps full stats. The host's rule: guests follow it."),
        "party_phantom_hp" => L.T("As in the game's own co-op: hunters who join a party have 30 % less maximum health. Off: full maximum health for everyone. The host's rule: guests follow it."),
        "party_skip_prologue" => L.T("A new character (one that has not finished the prologue) skips the clinic and the first dream visit: the prologue's story flags are set, the starting weapons are given and the hunter appears at the first lamp in the city, ready for the party. Characters past the prologue are never touched. Off: the prologue is played as in the game. The host's rule, read when the character starts."),
        "party_restart" => L.T("Automatically restarts the game after a crash and rejoins the party."),
        "party_guest_insight" => L.T("Controls the Insight you receive as a guest when the host kills a boss. Takes effect on the next game start."),
        "party_start" => L.T("Each new character normally completes its own prologue first; Right away starts the party at once. The host's rule, read at game start: a guest must have the same value, or the host refuses the join."),
        _ => null,
    };

    /// The party role and state the game writes to party_status.json (party_status.cpp RoleName /
    /// StateName, stored for logs): their labels. The free-text detail, network and event lines stay
    /// as the game writes them, which is English.
    private static string PartyRole(string role) => role switch
    {
        "host" => L.T("Host"),
        "guest" => L.T("Guest"),
        _ => L.T("Not in a party"),
    };

    private static string PartyState(string state) => state switch
    {
        "starting" => L.T("Starting"),
        "hosting" => L.T("Waiting for hunters"),
        "connecting" => L.T("Connecting"),
        "waiting_for_world" => L.T("Loading the world"),
        "ringing_bell" => L.T("Ringing the bell"),
        "joined" => L.T("Together"),
        "travelling" => L.T("Travelling"),
        "reconnecting" => L.T("Reconnecting"),
        "error" => L.T("Error"),
        _ => L.T("Not in a party"),
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
            if (saves.Length == 0) { Prompt.Tell(L.T("Export for save editor"), L.T("There is no character save yet.")); return; }
            var choice = Prompt.Choose(L.T("Export for save editor"), L.T("Which character?"), saves.Select(s => s.text).ToArray());
            if (choice < 0) return;
            var folder = new Microsoft.Win32.OpenFolderDialog { Title = L.T("A folder for the save editor's copy (an empty folder is best)") };
            if (folder.ShowDialog() != true) return;
            var save = saves[choice];
            Prompt.Tell(L.T("Export for save editor"), await SeamlessSession.SaveTool(S, "editor-export", save.kind, save.slot.ToString(), folder.FolderName) +
                (save.guest_written ? L.T(" The copy was fixed up for the editor (it was last saved as a guest in another world).") : ""));
        }
        catch (Exception e) { Prompt.Tell(L.T("Export for save editor"), e.Message); }
    }
    private async Task ImportFromEditor()
    {
        try
        {
            var file = new Microsoft.Win32.OpenFileDialog { Title = L.T("The edited save (exported by the launcher)"), Filter = L.T("Character save|userdata000*|All files|*") };
            if (file.ShowDialog() != true) return;
            var plan = System.Text.Json.JsonSerializer.Deserialize<EditorPlan>(await SeamlessSession.SaveTool(S, "editor-check", file.FileName))!;
            var kind = plan.kind == "party" ? L.T("party save") : L.T("single-player save");
            var notes = string.Concat(plan.notes.Select(n => "\n- " + n));
            if (!Prompt.Confirm(L.T("Import from save editor"),
                    L.F($"Replace slot {plan.slot + 1} of your {kind} ({plan.target}) with the edited file?{notes}\n\nYour current saves are copied to user\\save_backups first."), L.T("Import")))
                return;
            Prompt.Tell(L.T("Import from save editor"), await SeamlessSession.SaveTool(S, "editor-import", file.FileName));
        }
        catch (Exception e) { Prompt.Tell(L.T("Import from save editor"), e.Message); }
    }
    private async Task SaveAction(string action)
    {
        try { Prompt.Tell(L.T("Party saves"), await SeamlessSession.SaveTool(S, action)); }
        catch (Exception e) { Prompt.Tell(L.T("Party saves"), e.Message); }
    }
}
