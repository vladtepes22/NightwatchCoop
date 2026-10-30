using System.IO;
using System.Net;
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
    private readonly TextBlock _status;
    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    /// Rows and group headers shown only in some modes ("" single player, "host", "join").
    private readonly List<(UIElement Element, Func<string, bool> When)> _byMode = new();
    private static bool InParty(string mode) => mode.Length > 0;
    private static bool Hosting(string mode) => mode == "host";
    private static bool Joining(string mode) => mode == "join";

    public SeamlessPage() : this(null) { }

    internal SeamlessPage(bool? previewPublic) : base(L.T("Co-op"),
        L.T("Play the campaign together with visy's seamless co-op. The host shares a party code; everyone uses the same build, gameplay mods and party size."))
    {
        _previewPublic = previewPublic;
        Group(L.T("Party"));
        var mode = (Segmented)SettingCombo(SymbolRegular.People24, L.T("Mode"), L.T("Host starts the party; Join connects to the host."),
            new[] { new Choice(L.T("Single player"), ""), new Choice(L.T("Host"), "host"), new Choice(L.T("Join"), "join") }, "party_mode",
            changed: () => { ShowMode(); RunCheck(); }, buttons: true);
        var modeGrid = (Grid)mode.Parent;
        Grid.SetColumn(_modeVpnStatus, 2);
        _modeVpnStatus.HorizontalAlignment = HorizontalAlignment.Right;
        modeGrid.Children.Add(_modeVpnStatus);
        var party = Rows(InParty);
        TextRow(SymbolRegular.Person24, L.T("Your name"), L.T("1–16 letters, numbers, underscores or hyphens."), L.T("Hunter"),
            () => SeamlessSettings.Name(S), v => S.Set("party_name", v), party);
        var checks = Rows(InParty);
        _checkBanner = CheckBanner();
        checks.Children.Add(_checkBanner);
        _checkLine = Text("", "Faint");
        checks.Children.Add(_checkLine);
        var host = Rows(Hosting);
        _hostMode = (Segmented)ComboRow(SymbolRegular.People24, L.T("Party visibility"), null,
            new[] { new Choice(L.T("Public party"), "1"), new Choice(L.T("Private party"), "0") },
            () => IsPublic ? "1" : "0", ChangeHostMode, host,
            enabled: c => c.Value != "1" || NightwatchVpn.Shipped != null || _previewPublic != null, buttons: true);
        _hostConnect = SmallButton(L.T("Connect"), SymbolRegular.Globe24, async (_, _) => await ToggleVpn());
        Add(Card(SymbolRegular.Globe24, L.T("Party connection"), null, Row(_hostRing, _hostConnect), out _hostConnection), host);
        _hostPassword = PasswordRow(host, () => S.PartyPassword, v => { S.PartyPassword = v; UpdateInvite(); },
            L.T("Optional. Without a password, anyone who sees the party can join."));
        var hostOptions = new Grid();
        hostOptions.ColumnDefinitions.Add(new ColumnDefinition());
        hostOptions.ColumnDefinitions.Add(new ColumnDefinition());
        host.Children.Add(hostOptions);
        var sizeRows = new StackPanel();
        var showRows = new StackPanel();
        Grid.SetColumn(showRows, 1);
        hostOptions.Children.Add(sizeRows); hostOptions.Children.Add(showRows);
        _hostSize = (System.Windows.Controls.ComboBox)ComboRow(SymbolRegular.People24, L.T("Player limit"), null,
            PartySizes(), PartySize, v => S.Set("party_max", v), sizeRows, width: 90);
        ToggleRow(SymbolRegular.People24, L.T("Show my party to friends"), null,
            () => Ini.Get("party_discovery", "1") == "1", v => ApplyHostMode(IsPublic, v), showRows);
        _privateCodes = Rows(m => Hosting(m) && !IsPublic);
        _internetButton = SmallButton(L.T("Copy Internet code"), SymbolRegular.Copy24, (_, _) => { if (_internetCode.Length > 0) Copy(_internetCode); });
        _lanButton = SmallButton(L.T("Copy LAN / VPN code"), SymbolRegular.Copy24, (_, _) => { if (_lanCode.Length > 0) Copy(_lanCode); });
        Add(Card(SymbolRegular.Link24, L.T("Private party codes"), L.T("VPNs (Radmin VPN, Hamachi, Tailscale, ZeroTier) use the LAN code."),
            Row(_internetButton, _lanButton), out _inviteHint, wide: true), _privateCodes);

        var found = Rows(Joining);
        _joinConnect = SmallButton(L.T("Connect now"), SymbolRegular.Globe24, async (_, _) => await ToggleVpn());
        _joinBanner = JoinBanner();
        found.Children.Add(_joinBanner);
        _searchBox = new Wpf.Ui.Controls.TextBox { PlaceholderText = L.T("Search parties"), Margin = new Thickness(0, 8, 0, 8) };
        found.Children.Add(_searchBox);
        _searchBox.TextChanged += (_, _) => { RefreshLobby(); RefreshParties(); };
        _lobbyRow = Add(Card(SymbolRegular.Globe24, L.T("Games on the Nightwatch VPN"), null,
            SmallButton(L.T("Refresh"), SymbolRegular.ArrowClockwise24, (_, _) => RefreshLists()), out _lobbyHint), found);
        _lobbyList = new StackPanel(); found.Children.Add(_lobbyList);
        Add(Card(SymbolRegular.People24, L.T("Parties on your network"), null,
            SmallButton(L.T("Refresh"), SymbolRegular.ArrowClockwise24, (_, _) => RefreshLists()), out _partiesHint), found);
        _partyList = new StackPanel(); found.Children.Add(_partyList);
        _codeBox = TextRow(SymbolRegular.Link24, L.T("Host code or address"), L.T("Paste the host's Internet code, LAN code or host:port."), "BBP3-…",
            () => S.Str("party_code"), v => { S.Set("party_code", v); SyncSharedRows(); }, found);
        _joinPassword = PasswordRow(found, () => S.JoinPassword(S.Str("party_code")), v => S.SetJoinPassword(S.Str("party_code"), v),
            L.T("Optional. Enter the host's party password if needed."));
        Add(Card(SymbolRegular.Link24, L.T("Join by code or address"), null,
            SmallButton(L.T("Join party"), SymbolRegular.Link24, async (_, _) => await StartJoin()), out _), found);

        var network = Fold(L.T("Advanced networking and troubleshooting"), InParty);
        _vpnPick = new System.Windows.Controls.ComboBox { Width = 220, VerticalAlignment = VerticalAlignment.Center };
        _vpnPick.SelectionChanged += (_, _) => {
            if (_syncingVpn || _vpnPick.SelectedItem is not string label) return;
            Ini.Set("party_vpn", label); Ws.SaveSoon(); UpdateRoute();
        };
        _vpnPickRow = Add(Card(SymbolRegular.Router24, L.T("VPN"), L.T("More than one VPN is up: the one friends are in."), _vpnPick, out _), Rows(Hosting, network));
        if (NightwatchVpn.Shipped != null)
        {
            _vpnButton = SmallButton(L.T("Connect"), SymbolRegular.Globe24, async (_, _) => await ToggleVpn());
            Add(Card(SymbolRegular.Globe24, L.T("Connection"),
                L.T("Needs OpenVPN. The first Connect makes a personal key that stays on this PC, encrypted for your Windows user, and asks the Nightwatch VPN server for a certificate. Nobody registers. Closing the launcher ends the connection."),
                VpnControl(), out _vpnHint), network);
        }
        ToggleRow(SymbolRegular.Globe24, L.T("Nightwatch VPN lobby"), L.T("Controls whether your hosted party is listed. Join always browses while connected."),
            () => Ini.Get("party_vpn_lobby", "0") == "1", v => Ini.Set("party_vpn_lobby", v ? "1" : "0"), Rows(Hosting, network));
        Add(Card(SymbolRegular.Info24, L.T("Last session and firewall"), null,
            Row(SmallButton(L.T("Check"), SymbolRegular.Info24, (_, _) => CheckConnection()),
                SmallButton(L.T("Fix firewall"), SymbolRegular.Wrench24, (_, _) => FixFirewall())), out _), network);
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
            ("party_guest_gather", L.T("Guests can gather the party"), false), ("party_layer_travel", L.T("Chalice layer travel"), false),
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
            ("party_toasts", L.T("Join, leave and travel notices"), true), ("party_item_notices", L.T("Shared item notices"), true), ("party_npc_talk", L.T("Share NPC dialogue"), true),
            ("party_stay", L.T("Stay in my world"), false));
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
        // visy 0.7.7 (scripts/party_offline.py): the party-only state is cleaned out of the copy.
        Add(Card(SymbolRegular.Copy24, L.T("Copy party character to offline"), L.T("A party character goes into the same single-player slot, cleaned for offline play. The party save is not changed."),
            SmallButton(L.T("Copy to offline"), SymbolRegular.Copy24, async (_, _) => await CopyPartyToOffline()), out _));
        ComboRow(SymbolRegular.Document24, L.T("Party log detail"), L.T("Verbose and trace logging add overhead."),
            new[] { new Choice(L.T("Normal"), ""), new Choice(L.T("Verbose"), "verbose"), new Choice(L.T("Trace"), "trace") },
            () => S.Str("party_log"), v => S.Set("party_log", v));

        _timer.Tick += (_, _) => {
            RefreshPublic();
            UpdateInvite();
            if (++_ticks % 5 == 0) RefreshRoute();
            RefreshParties();
            RefreshLobby();
            UpdateVpn();
            var status = SeamlessSession.Read(S);
            _status.Text = status == null ? (Ws.Game.Running ? L.T("Waiting for party status…") : L.T("Game stopped.")) :
                $"{PartyRole(status.role)}：{PartyState(status.state)}  {status.detail}\n{status.network}\n" +
                string.Join("\n", status.members.Select(m => $"{m.name}：{(m.in_world ? L.T("in host world") : m.connected ? L.T("connected") : L.T("disconnected"))}  {(m.ping_ms < 0 ? "" : m.ping_ms + " ms")}  {m.area}")) +
                $"\n{status.@event}\n{status.alert}";
        };
        Loaded += (_, _) => { _timer.Start(); RefreshPublic(); RefreshRoute(); StartListener(); RunCheck(); };
        Unloaded += (_, _) => { _timer.Stop(); _listener?.Dispose(); _listener = null; _lobby?.Dispose(); _lobby = null; };
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
    private VpnLobby.Poller? _lobby;
    private volatile bool _lobbyOn;
    private string _lobbySignature = "\0";
    private readonly FrameworkElement _lobbyRow;
    private readonly TextBlock _lobbyHint;
    private readonly StackPanel _lobbyList;
    private readonly Wpf.Ui.Controls.Button? _vpnButton;
    private readonly TextBlock? _vpnHint;
    private readonly VpnConnection _vpn = new();
    private readonly Wpf.Ui.Controls.ProgressRing _vpnRing = new() { IsIndeterminate = true, Width = 18, Height = 18, Margin = new Thickness(0, 0, 10, 0), Visibility = Visibility.Collapsed };
    private bool _vpnSetup;
    private string _vpnNote = "";
    private StackPanel _partyList = null!;
    private TextBlock _partiesHint = null!;
    private Wpf.Ui.Controls.TextBox _codeBox = null!;
    private readonly TextBlock _modeVpnStatus = Text("", "Faint");
    private readonly Segmented _hostMode;
    private readonly TextBlock _hostConnection;
    private readonly FrameworkElement _joinBanner;
    private readonly StackPanel _privateCodes;
    private readonly Wpf.Ui.Controls.Button _hostConnect, _joinConnect;
    private readonly Wpf.Ui.Controls.TextBox _searchBox;
    private readonly Wpf.Ui.Controls.ProgressRing _hostRing = new() { Width = 18, Height = 18, IsIndeterminate = true, Visibility = Visibility.Collapsed };
    private readonly Wpf.Ui.Controls.ProgressRing _joinRing = new() { Width = 18, Height = 18, IsIndeterminate = true, Visibility = Visibility.Collapsed };
    private bool _changingHost;
    private readonly bool? _previewPublic;
    private bool IsPublic => _previewPublic ?? (NightwatchVpn.Shipped != null && S.Str("party_host_public") == "1");

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

    /// <summary>The host's password for a join, or null when cancelled or left empty (a party with a password needs it).</summary>
    private static string? AskPartyPassword(string host, string remembered)
    {
        var password = Prompt.Secret(L.T("Join ") + host, L.T("Type the host's party password."),
            new Prompt.Field(L.T("Password"), remembered, L.T("Party password")), L.T("Join"));
        if (password == null) return null;
        if (password.Trim().Length > 0) return password;
        Prompt.Tell(L.T("No password"), L.T("This party has a password: ask the host for it."));
        return null;
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
        if (!party.Info.Password && !party.Info.Open)
        {
            Prompt.Tell(L.T("Join ") + party.Info.Name, L.T("This host has no password and shares a party code: ask the host for it."));
            return;
        }
        var password = "";
        if (party.Info.Password)
        {
            if (AskPartyPassword(party.Info.Name, S.JoinPassword(party.Address)) is not { } typed) return;
            password = typed;
        }
        S.SetJoinPassword(party.Address, password);
        _codeBox.Text = party.Address;  // saved by the row
        if (PartyDiscovery.PartySize(party.Info.Max) is { } size) S.Set("party_max", size);
        Ws.SaveSoon();
        SyncSharedRows();
        _partySignature = "\0";
        RefreshParties();
        await StartJoin();
    }

    private StackPanel VpnControl()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        row.Children.Add(_vpnRing);
        row.Children.Add(_vpnButton!);
        return row;
    }

    /// <summary>First use on a PC without OpenVPN: the player agrees, then the shipped installer runs (one UAC prompt).
    /// Cancel installs nothing. Null: no OpenVPN to connect with, and the note says why.</summary>
    private async Task<string?> SetUpOpenVpn()
    {
        if (!File.Exists(OpenVpnSetup.PackagedMsi))
        {
            Prompt.Tell(L.T("Nightwatch VPN"), L.T("OpenVPN is not installed. Install OpenVPN Community from openvpn.net, then press Connect again."));
            return null;
        }
        if (!Prompt.Confirm(L.T("Set up online play"),
                L.T("Online play uses the Nightwatch VPN network. Setup will install OpenVPN GUI and the required networking components. Windows may ask for administrator permission."),
                L.T("Install and continue")))
            return null;
        _vpnSetup = true;
        _vpnNote = L.T("Setting up online play…");
        UpdateVpn();
        var result = await OpenVpnSetup.InstallAsync();
        _vpnSetup = false;
        _vpnNote = result switch
        {
            OpenVpnSetupResult.Installed when NightwatchVpn.GuiExe() != null => "",
            OpenVpnSetupResult.Declined => L.T("Setup was cancelled, so nothing was installed. You can still play with a Private party."),
            OpenVpnSetupResult.Busy => L.T("Another installer is running. Wait for it to finish and press Connect again."),
            OpenVpnSetupResult.Tampered or OpenVpnSetupResult.Missing => L.T("The OpenVPN installer in this package is damaged or missing. Reinstall Nightwatch Co-op, or play with a Private party."),
            _ => L.T("OpenVPN could not be installed. You can still play with a Private party."),
        };
        UpdateVpn();
        return NightwatchVpn.GuiExe();
    }

    /// <summary>Connect / Disconnect: sets up OpenVPN and enrolls on first use, renews when due, then OpenVPN GUI runs the
    /// tunnel. The state holds until the tunnel's adapter confirms it (<see cref="WatchVpn"/>).</summary>
    private async Task ToggleVpn()
    {
        if (NightwatchVpn.Shipped is not { } endpoint || _vpn.Busy || _vpnSetup) return;
        _adapters = await Task.Run(NetworkRoute.Enumerate);
        var up = VpnLobby.TunnelIp(_adapters) != null;
        if (up && !NightwatchVpn.OwnedHere) return;
        if (up && NightwatchVpn.GuiExe() == null) { _vpnNote = L.T("Could not start the connection through OpenVPN."); UpdateVpn(); return; }
        if (NightwatchVpn.GuiExe() is not { } gui)
        {
            if (await SetUpOpenVpn() is not { } installed) return;
            gui = installed;
        }
        _vpnNote = "";
        try
        {
            if (up)
            {
                _vpn.BeginDisconnect(DateTime.UtcNow);
                UpdateVpn();
                await Task.Run(() => NightwatchVpn.Disconnect(gui));
            }
            else
            {
                if (!NightwatchVpn.TryOwn())
                {
                    _vpnNote = L.T("Another launcher window holds the connection.");
                    UpdateVpn();
                    return;
                }
                _vpn.BeginConnect(DateTime.UtcNow);
                UpdateVpn();
                var install = await NightwatchVpn.ReadyAsync(endpoint, Paths.ConfigDir);
                await Task.Run(() => NightwatchVpn.Connect(install, gui));
            }
            await WatchVpn(gui);
            return;
        }
        catch (VpnException e) { _vpnNote = VpnMessage(e); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or System.Security.Cryptography.CryptographicException)
        {
            _vpnNote = L.T("Could not start the connection through OpenVPN.");
        }
        _vpn.Abort(VpnLobby.TunnelIp(_adapters) != null);
        if (_vpn.Phase == VpnPhase.Off) NightwatchVpn.ReleaseOwnership();
        RefreshRoute();
        UpdateVpn();
    }

    /// <summary>Until the adapter confirms the change or the time is up. A connection that timed out is called off, so it
    /// cannot come up later unasked.</summary>
    private async Task WatchVpn(string gui)
    {
        var connecting = _vpn.Phase == VpnPhase.Connecting;
        while (_vpn.Busy)
        {
            await Task.Delay(500);
            _adapters = await Task.Run(NetworkRoute.Enumerate);
            _vpn.Observe(VpnLobby.TunnelIp(_adapters) != null, DateTime.UtcNow);
        }
        if (_vpn.TimedOut)
        {
            _vpnNote = connecting
                ? L.T("The connection did not come up: the VPN server may be full, or a firewall blocks it. You can still play with a Private party.")
                : L.T("OpenVPN did not confirm the disconnect. Check its tray icon.");
            if (connecting) await Task.Run(() => NightwatchVpn.Disconnect(gui));
        }
        if (_vpn.Phase == VpnPhase.Off) NightwatchVpn.ReleaseOwnership();
        RefreshRoute();
        UpdateVpn();
    }

    private static string VpnMessage(VpnException e) => e.Failure switch
    {
        VpnFailure.Blocked => L.T("This install is blocked."),
        VpnFailure.RateLimited => L.T("The VPN server asks to wait before trying again. Minutes left: ") + Math.Max(1, e.RetryAfterSeconds / 60),
        VpnFailure.PinMismatch => L.T("The VPN server could not prove who it is, so nothing was sent."),
        VpnFailure.Unreachable => L.T("Could not reach the VPN server. Check your connection and try again."),
        VpnFailure.Protect => L.T("Could not read the VPN keys for this Windows user."),
        _ => L.T("The VPN server refused the request."),
    };

    private void UpdateVpn()
    {
        var up = VpnLobby.TunnelIp(_adapters) != null;
        _modeVpnStatus.Text = up ? L.T("VPN connected") : L.T("VPN disconnected");
        _joinBanner.Visibility = !up && (NightwatchVpn.Shipped != null || _previewPublic != null) ? Visibility.Visible : Visibility.Collapsed;
        SetHint(_hostConnection, !IsPublic ? L.T("Friends use a code") : !up ? L.T("Nightwatch VPN not connected")
            : Ini.Get("party_vpn_lobby", "0") == "1" ? L.T("Listed in the lobby as ") + SeamlessSettings.Name(S) : L.T("Not listed: Show my party to friends is off"));
        _hostConnect.Visibility = IsPublic && !up ? Visibility.Visible : Visibility.Collapsed;
        _hostConnect.IsEnabled = _joinConnect.IsEnabled = !_vpn.Busy && !_vpnSetup && !_changingHost;
        _hostRing.Visibility = _joinRing.Visibility = _vpn.Busy || _vpnSetup ? Visibility.Visible : Visibility.Collapsed;
        if (_vpnNote.Length > 0 || _vpn.Busy || _vpnSetup)
            SetHint(_hostConnection, _hostConnection.Text + "\n" + (_vpnNote.Length > 0 ? _vpnNote : L.T("Connecting…")));
        if (_vpnButton == null || _vpnHint == null) return;
        // A tunnel of ours from before this launcher started (a crash) belongs to whoever closes it next.
        if (up && !NightwatchVpn.OwnedHere && !_vpn.Busy && File.Exists(NightwatchVpn.ProfilePath)) NightwatchVpn.TryOwn();
        var other = up && !NightwatchVpn.OwnedHere;
        var busy = _vpn.Busy || _vpnSetup;
        if (up && !busy) _vpnNote = "";
        _vpnRing.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        _vpnButton.Content = _vpn.Phase == VpnPhase.Connecting ? L.T("Connect") : up ? L.T("Disconnect") : L.T("Connect");
        _vpnButton.IsEnabled = !busy && !other;
        SetHint(_vpnHint, _vpn.Phase == VpnPhase.Connecting ? L.T("Connecting…")
            : _vpn.Phase == VpnPhase.Disconnecting ? L.T("Disconnecting…")
            : _vpnNote.Length > 0 ? _vpnNote
            : other ? L.T("Another launcher window or VPN program holds this connection.")
            : up ? L.T("Connected. Online play ends when you close the launcher.") : L.T("Not connected"));
    }

    /// <summary>Join in the VPN lobby's list: asks for the password, then starts the game as Play starts it. The address
    /// and the password stay in memory for this start (SeamlessSettings.PendingJoin).</summary>
    private async void JoinLobbyGame(LobbyGame game)
    {
        if (Ws.Game.Running)
        {
            Prompt.Tell(L.T("Join ") + game.Name, L.T("Restart the game to join ") + game.Name + ".");
            return;
        }
        var password = "";
        if (game.Password)
        {
            if (AskPartyPassword(game.Name, "") is not { } typed) return;
            password = typed;
        }
        SeamlessSettings.PendingJoin = (game.JoinCode, password);
        try { await MainWindow.Instance!.PlayOrStopAsync(); }
        finally { SeamlessSettings.PendingJoin = null; }
    }

    /// <summary>Where the poller asks: the lobby on the tunnel from this PC's tunnel address, or a test lobby; null sends nothing.</summary>
    private (IPEndPoint Target, string? Bind)? LobbySource()
    {
        if (!_lobbyOn) return null;
        if (VpnLobby.TestTarget() is { } test) return (test, null);
        return VpnLobby.TunnelIp(_adapters) is { } ip ? (new IPEndPoint(IPAddress.Parse(VpnLobby.Address), VpnLobby.Port), ip) : null;
    }

    /// <summary>Redraws the VPN games from the poller's list, only when it changed. Shows the name, players, password and
    /// version; another version is greyed out.</summary>
    private void RefreshLobby()
    {
        _lobbyRow.Visibility = _lobbyOn && S.Str("party_mode") == "join" ? Visibility.Visible : Visibility.Collapsed;
        _lobbyList.Visibility = _lobbyRow.Visibility;
        if (!_lobbyOn) return;
        var test = VpnLobby.TestTarget() != null;
        var up = test || VpnLobby.TunnelIp(_adapters) != null;
        var games = VpnLobby.Rows(_lobby?.Games ?? new List<LobbyGame>(), test).Where(g => CoopHostMode.MatchesSearch(g.Name, _searchBox.Text)).ToList();
        var prefix = PartyDiscovery.BuildPrefix(Updater.Current);
        var signature = _searchBox.Text + "|" + up + "|" + (_lobby?.Answered == true) + "|" + string.Join(";", games.Select(g => $"{g.Name}/{g.Ip}/{g.Port}/{g.Players}/{g.Max}/{g.Build}"));
        if (signature == _lobbySignature) return;
        _lobbySignature = signature;
        SetHint(_lobbyHint, !up ? L.T("Connect to the Nightwatch VPN to see its games.") : !(_lobby?.Answered == true) ? L.T("Waiting for the lobby…") :
            games.Count == 0 ? (_searchBox.Text.Trim().Length > 0 ? L.T("No matching parties.") : L.T("No games on the Nightwatch VPN right now.")) : "");
        _lobbyList.Children.Clear();
        foreach (var game in games)
        {
            var sameVersion = PartyDiscovery.BuildMatches(game.Build, prefix);
            var open = sameVersion && game.Players < game.Max;
            var picked = game;
            var join = SmallButton(!sameVersion ? L.T("Update needed") : open ? L.T("Join") : L.T("Full"), SymbolRegular.Link24, (_, _) => JoinLobbyGame(picked));
            join.IsEnabled = open;
            var row = Card(SymbolRegular.People24, game.Name, null, join, out var hint);
            if (!open) row.Opacity = 0.6;
            SetHint(hint, game.Players + " / " + game.Max + " · " + (game.Password ? L.T("Password: yes") : L.T("Open")) + " · " + VpnLobby.VersionOf(game.Build));
            _lobbyList.Children.Add(row);
        }
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
        _lobbyOn = S.Str("party_mode") == "join";
        if (!wanted) { _lobby?.Dispose(); _lobby = null; }
        else _lobby ??= new VpnLobby.Poller(LobbySource);
        RefreshLobby();
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
        if (S.Str("party_mode") != "join") return;
        var now = PartyDiscovery.NowMs;
        var parties = _book.Snapshot(now).Where(p => CoopHostMode.MatchesSearch(p.Info.Name, _searchBox.Text)).ToList();
        var prefix = PartyDiscovery.BuildPrefix(Updater.Current);
        var waited = now - _listenSinceMs > ListenGraceMs;
        var saved = S.Str("party_code");
        var listen = ListenLine();
        var signature = _searchBox.Text + "|" + _joinNote + "|" + _listener?.Error + "|" + waited + "|" + saved + "|" + listen + "|" +
                        string.Join(";", parties.Select(p => $"{p.Info.Id}/{p.Ip}/{p.Info.Players}/{p.Info.Max}/{p.Info.Level}/{p.Info.Area}/{p.Info.Name}/{p.Info.Password}"));
        if (signature == _partySignature) return;
        _partySignature = signature;
        _partyList.Children.Clear();
        SetHint(_partiesHint, listen + (_joinNote.Length > 0 ? "\n" + _joinNote : "") + "\n" + (
            _listener?.Error.Length > 0 ? L.T("Could not listen for parties: ") + _listener?.Error + L.T(". Paste the host's code below instead.") :
            parties.Count > 0 ? "" : _searchBox.Text.Trim().Length > 0 ? L.T("No matching parties.") :
            !waited ? L.T("Looking for parties…") :
            L.T("No party found yet. Check that:") + "\n• " + L.T("the host has Show my party to friends on and has started the game") +
            "\n• " + L.T("you are both on the same VPN or local network") + "\n• " + L.T("Windows Firewall allows Nightwatch Co-op on Public networks too (VPN adapters often count as Public)")).TrimEnd('\n'));
        foreach (var party in parties)
        {
            var info = party.Info;
            var state = PartyDiscovery.StateOf(info, prefix);
            var selected = PartyDiscovery.Selected(party, saved);
            var text = (selected ? L.T("Selected") + " · " : "") + L.T("Level ") + (info.Level > 0 ? info.Level.ToString() : "?") + " · " +
                       info.Players + " / " + info.Max + " · " + (info.Password ? L.T("Password") : info.Open ? L.T("Open") : L.T("Code required")) + (info.Area.Length > 0 ? " · " + info.Area : "");
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

    private readonly Wpf.Ui.Controls.Button _internetButton, _lanButton;
    private readonly TextBlock _inviteHint;
    private string _internetCode = "", _lanCode = "";
    private string? _publicIp;
    private string _publicKey = "";
    private DateTime _publicAt;
    private bool _publicLooking;
    private readonly System.Windows.Controls.ComboBox _vpnPick;
    private readonly FrameworkElement _vpnPickRow;
    private bool _syncingVpn;
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
        var pref = Ini.Get("party_vpn", "auto");
        var vpns = NetworkRoute.VpnLabels(_adapters);
        _vpnPickRow.Visibility = vpns.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
        _syncingVpn = true;
        if (!_vpnPick.Items.Cast<string>().SequenceEqual(vpns))
        {
            _vpnPick.Items.Clear();
            foreach (var vpn in vpns) _vpnPick.Items.Add(vpn);
        }
        _vpnPick.SelectedItem = vpns.FirstOrDefault(l => l.Contains(pref, StringComparison.OrdinalIgnoreCase) || pref.Contains(l, StringComparison.OrdinalIgnoreCase));
        _syncingVpn = false;
        UpdateInvite();
        UpdateVpn();
    }

    /// <summary>The Invite friends card: the code for the way friends join (Internet code, or the local network / VPN code).
    /// While the game hosts it is the engine's own (party_code.txt: UPnP, IPv6); before that it is made here from the saved
    /// secret, the password, the port and the address (PartyCodes), so it can be copied without starting the game.</summary>
    private void UpdateInvite()
    {
        _privateCodes.Visibility = S.Str("party_mode") == "host" && !IsPublic ? Visibility.Visible : Visibility.Collapsed;
        if (S.Str("party_mode") != "host" || IsPublic) { _internetCode = _lanCode = ""; return; }
        var password = S.PartyPassword.Length > 0;
        var port = int.TryParse(S.Str("party_port"), out var p) && p is >= 1 and <= 65535 ? p : NetworkRoute.DefaultPort;
        var file = Ws.Game.Running ? PartyCodes.ReadFile(GameEnvironment.UserDir(S)) : (Internet: "", Lan: "");
        var secret = PartyCodes.HostSecret(GameEnvironment.UserDir(S), password, save: !Ws.Game.Running && _previewPublic == null);
        _internetCode = file.Internet;
        _lanCode = file.Lan;
        if (_internetCode.Length == 0 && _publicIp != null && IPAddress.TryParse(_publicIp, out var ip))
            _internetCode = PartyCodes.Encode(false, password, ip.GetAddressBytes(), port, secret);
        var route = NetworkRoute.Choose(_adapters, Ini.Get("party_vpn", "auto") == "off" ? "auto" : Ini.Get("party_vpn", "auto"), port);
        if (_lanCode.Length == 0 && IPAddress.TryParse(route.Address.Split(':')[0], out var local))
            _lanCode = PartyCodes.Encode(true, password, local.GetAddressBytes(), port, secret);
        _internetButton.IsEnabled = _internetCode.Length > 0;
        _lanButton.IsEnabled = _lanCode.Length > 0;
        SetHint(_inviteHint, (_internetCode.Length > 0 ? _internetCode : L.T("Internet code unavailable until your public address is known.")) + "\n" +
            (_lanCode.Length > 0 ? _lanCode : L.T("LAN code unavailable: no VPN or local network found.")));
    }

    /// <summary>Looks up this PC's public address for the Internet code (the Host public address, else STUN) off the UI
    /// thread: when the page opens, when those settings change, every few minutes, and again soon after a failure.</summary>
    private async void RefreshPublic()
    {
        if (_publicLooking || S.Str("party_mode") != "host" || IsPublic) return;
        var over = S.Str("party_public_addr").Trim();
        var stun = S.Str("party_stun").Trim();
        var age = DateTime.UtcNow - _publicAt;
        if (over + "|" + stun == _publicKey && age < (_publicIp != null ? TimeSpan.FromMinutes(5) : TimeSpan.FromSeconds(30))) return;
        _publicLooking = true;
        UpdateInvite();
        string? found = null;
        // A best-effort lookup: whatever goes wrong (no network, a mistyped server) is "not found", never a crash.
        try { found = await PartyCodes.PublicAddressAsync(over, stun).ConfigureAwait(false); }
        catch (Exception e) when (e is not OutOfMemoryException) { }
        await Dispatcher.InvokeAsync(() => {
            _publicIp = found;
            _publicKey = over + "|" + stun;
            _publicAt = DateTime.UtcNow;
            _publicLooking = false;
            UpdateInvite();
        });
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
    private void RefreshLists()
    {
        _book.Clear(); _partySignature = _lobbySignature = "\0";
        _lobby?.Dispose(); _lobby = null;
        RefreshRoute(); StartListener(restart: true); RefreshParties(); RefreshLobby();
    }

    private async Task StartJoin()
    {
        if (Ws.Game.Running) { Prompt.Tell(L.T("Party"), L.T("Restart the game to join the party.")); return; }
        if (string.IsNullOrWhiteSpace(_codeBox.Text)) { Prompt.Tell(L.T("Party"), L.T("Enter the host's code or address first.")); return; }
        await MainWindow.Instance!.PlayOrStopAsync();
    }

    /// <summary>The red notice over the Join lists while the Nightwatch VPN is down.</summary>
    private Border JoinBanner()
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var icon = new SymbolIcon { Symbol = SymbolRegular.Warning24, FontSize = 28, Margin = new Thickness(0, 0, 14, 0), VerticalAlignment = VerticalAlignment.Center };
        var title = Text(L.T("Nightwatch VPN not connected"), "Body");
        title.FontWeight = FontWeights.SemiBold;
        var hint = Text(L.T("Connect to see public parties, or use a private party code below."), "Faint");
        hint.TextWrapping = TextWrapping.Wrap;
        var words = new StackPanel { VerticalAlignment = VerticalAlignment.Center, Children = { title, hint } };
        var action = Row(_joinRing, _joinConnect);
        action.VerticalAlignment = VerticalAlignment.Center;
        action.Margin = new Thickness(12, 0, 0, 0);
        Grid.SetColumn(words, 1);
        Grid.SetColumn(action, 2);
        grid.Children.Add(icon); grid.Children.Add(words); grid.Children.Add(action);
        var banner = new Border { Child = grid, BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(14, 12, 14, 12), Margin = new Thickness(14, 8, 14, 4) };
        banner.SetResourceReference(Border.BorderBrushProperty, "DangerTextBrush");
        icon.SetResourceReference(ForegroundProperty, "DangerTextBrush");
        return banner;
    }

    // ---- The silent connection check (Core/ConnectionCheck): a banner only when something is wrong ----

    private Border _checkBanner = null!;
    private StackPanel _checkLines = null!;
    private Wpf.Ui.Controls.Button _checkFix = null!;
    private TextBlock _checkLine = null!;
    private List<CheckProblem> _problems = new();
    private CheckFacts? _checkFacts;
    private int _checkRun;

    private Border CheckBanner()
    {
        _checkLines = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        _checkFix = SmallButton(L.T("Fix"), SymbolRegular.Wrench24, (_, _) => FixProblems());
        _checkFix.Margin = new Thickness(12, 0, 0, 0);
        _checkFix.VerticalAlignment = VerticalAlignment.Center;
        DockPanel.SetDock(_checkFix, Dock.Right);
        var banner = new Border
        {
            Child = new DockPanel { Children = { _checkFix, _checkLines } },
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(8), Padding = new Thickness(14, 10, 14, 10),
            Margin = new Thickness(14, 8, 14, 4), Visibility = Visibility.Collapsed,
        };
        banner.SetResourceReference(Border.BorderBrushProperty, "DangerTextBrush");
        return banner;
    }

    /// <summary>Runs on tab open and when Host or Join is pressed; a newer run replaces an older one's result.</summary>
    private async void RunCheck()
    {
        var mode = S.Str("party_mode");
        if (mode.Length == 0 || _previewPublic != null) return;
        var run = ++_checkRun;
        var port = int.TryParse(S.Str("party_port"), out var p) && p is >= 1 and <= 65535 ? p : NetworkRoute.DefaultPort;
        var adapters = await Task.Run(NetworkRoute.Enumerate);
        _adapters = adapters;
        var pref = Ini.Get("party_vpn", "auto");
        var start = new CheckFacts
        {
            Hosting = mode == "host",
            PublicParty = IsPublic,
            LocalAddress = NetworkRoute.SelectLan(adapters)?.IpText ?? "",
            OfflineVpns = NetworkRoute.OfflineVpnLabels(adapters, pref),
            VpnsUp = NetworkRoute.VpnLabels(adapters),
            AutomaticRoute = NetworkRoute.HostOverOf(pref) == HostOver.Automatic,
            OpenVpnMissing = NightwatchVpn.Shipped != null && NightwatchVpn.GuiExe() == null,
            SetupPackaged = File.Exists(OpenVpnSetup.PackagedMsi),
            VpnNote = _vpn.Busy || _vpnSetup ? "" : _vpnNote,
            ClockSkew = NightwatchVpn.ClockSkew,
        };
        var stun = S.Str("party_stun");
        var running = Ws.Game.Running;
        var facts = await Task.Run(() => ConnectionCheck.Gather(start, port, stun, running));
        if (run != _checkRun) return;
        _checkFacts = facts;
        _problems = ConnectionCheck.Detect(facts);
        _checkLines.Children.Clear();
        foreach (var problem in _problems)
        {
            var line = Text(problem.Text, "Faint");
            line.TextWrapping = TextWrapping.Wrap;
            line.Margin = new Thickness(0, 2, 0, 2);
            _checkLines.Children.Add(line);
        }
        _checkFix.Visibility = _problems.Any(c => c.Fix != CheckFix.None) ? Visibility.Visible : Visibility.Collapsed;
        _checkBanner.Visibility = _problems.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        _checkLine.Text = (_problems.Count == 0 ? L.T("Connection check: no problems found") : L.T("Connection check: problems found")) + " (" + DateTime.Now.ToString("t") + ")";
    }

    /// <summary>Everything fixable, in one go: the firewall's commands are shown first as always.</summary>
    private async void FixProblems()
    {
        foreach (var fix in _problems.Select(c => c.Fix).Distinct().ToList())
        {
            if (fix == CheckFix.Firewall) FixFirewall();
            else if (fix == CheckFix.VpnSetup) await ToggleVpn();
            else if (fix == CheckFix.NextPort && _checkFacts is { NextFreePort: > 0 } facts
                     && Prompt.Confirm(L.T("Party port"), L.T("Move the party to port ") + facts.NextFreePort + L.T("? Friends then need your new code."), L.T("Move")))
            {
                S.Set("party_port", facts.NextFreePort.ToString());
                Ws.SaveSoon();
                UpdateInvite();
            }
        }
        // The elevated window takes a moment to finish its rules.
        await Task.Delay(TimeSpan.FromSeconds(8));
        RunCheck();
    }

    private void ApplyHostMode(bool isPublic, bool show) =>
        CoopHostMode.Apply(isPublic, show, Ini.Get("party_vpn", "auto"), (k, v) => Ini.Set(k, v));

    private async void ChangeHostMode(string value)
    {
        if (_changingHost) { _hostMode.Select(IsPublic ? 0 : 1, notify: false); return; }
        var isPublic = value == "1";
        if (isPublic == IsPublic) return;
        var up = VpnLobby.TunnelIp(_adapters) != null;
        if (!isPublic && up && !Prompt.Confirm(L.T("Private party"),
            L.T("Private party turns off the Nightwatch VPN. You can still play with friends over the internet, LAN or other VPNs."), L.T("Disconnect")))
        { _hostMode.Select(0, notify: false); return; }
        _changingHost = true;
        _hostMode.IsEnabled = false;
        try
        {
            if (!isPublic && up)
            {
                await ToggleVpn();
                if (VpnLobby.TunnelIp(_adapters) != null) { _hostMode.Select(0, notify: false); return; }
            }
            S.Set("party_host_public", isPublic ? "1" : "0");
            ApplyHostMode(isPublic, Ini.Get("party_discovery", "1") == "1");
            Ws.SaveSoon(); ShowMode();
            if (isPublic && !up) await ToggleVpn();
        }
        finally { _changingHost = false; _hostMode.IsEnabled = true; UpdateVpn(); }
    }

    private void ShowMode()
    {
        var mode = S.Str("party_mode");
        foreach (var (element, when) in _byMode)
            element.Visibility = when(mode) ? Visibility.Visible : Visibility.Collapsed;
        SyncSharedRows();
        UpdateRoute();
        RefreshPublic();
        StartListener();
    }
    // Party rules from engine/gpu/shim/bbport_game_menu_pages.cpp; My party descriptions from
    // engine/docs/party/my_party_settings.md, plus the remaining My party rows.
    private static string? PartyInfo(string key) => key switch
    {
        "party_kill_record" => L.T("A party kill stays dead for everyone, even when maps reload, until a rest."),
        "party_gather" => L.T("Party > Gather the party brings every guest to you; each guest is asked first."),
        "party_guest_gather" => L.T("A guest may use Party > Gather the party too: the host and the other guests are asked and come to that guest by an in-world warp, never through a loading screen. Off: only the host gathers. The host's rule: guests follow it."),
        "party_layer_travel" => L.T("In the host's Chalice Dungeon a lamp lists the Hunter's Dream and every lit lamp of the dungeon, and the party goes with the host. Off: the lamp only offers the way back to the Hunter's Dream. The host's rule: guests follow it."),
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
    private sealed record OfflineCharacter(int slot, string name, int level, string? replaces);

    private async Task CopyPartyToOffline()
    {
        var title = L.T("Copy party character to offline");
        try
        {
            var characters = System.Text.Json.JsonSerializer.Deserialize<OfflineCharacter[]>(
                await SeamlessSession.OfflineTool(S, "list")) ?? [];
            if (characters.Length == 0) { Prompt.Tell(title, L.T("There is no party character yet.")); return; }
            var choice = Prompt.Choose(title, L.T("Which character?"), characters.Select(c => $"{c.slot + 1}: {c.name} ({c.level})").ToArray());
            if (choice < 0) return;
            var pick = characters[choice];
            var replaces = pick.replaces is null ? "" : L.F($"\n\nIt replaces {pick.replaces} in single-player slot {pick.slot + 1}.");
            if (!Prompt.Confirm(title,
                    L.F($"Copy {pick.name} (level {pick.level}) to single-player slot {pick.slot + 1}? Party-only state is cleaned out of the copy and your party save is not changed.{replaces}\n\nYour single-player saves are copied to user\\save_backups first."),
                    L.T("Copy")))
                return;
            Prompt.Tell(title, await SeamlessSession.OfflineTool(S, "copy", pick.slot.ToString()));
        }
        catch (Exception e) { Prompt.Tell(title, e.Message); }
    }
    private async Task SaveAction(string action)
    {
        try { Prompt.Tell(L.T("Party saves"), await SeamlessSession.SaveTool(S, action)); }
        catch (Exception e) { Prompt.Tell(L.T("Party saves"), e.Message); }
    }
}
