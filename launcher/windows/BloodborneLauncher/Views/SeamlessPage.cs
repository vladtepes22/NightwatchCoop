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

    public SeamlessPage() : base(L.T("Co-op"),
        L.T("Play the campaign together with visy's seamless co-op. The host shares a party code; everyone uses the same build, gameplay mods and party size."))
    {
        // Single player shows only Party saves; Join leaves out the host's rows, Host the guest's.
        // The connection card (Host and Join): the route the party uses, its state and the address to give friends. The
        // adapters are read off the UI thread (RefreshRoute); the address is shown on this screen only.
        Group(L.T("Party"));
        var routeRows = Rows(InParty);
        _copyAddress = SmallButton(L.T("Copy"), SymbolRegular.Copy24, (_, _) => { if (_route.Address.Length > 0) Clipboard.SetText(_route.Address); });
        Add(Card(SymbolRegular.Router24, L.T("Your connection"),
            L.T("How the party reaches you: your VPN (Radmin VPN, Hamachi, ZeroTier, Tailscale) when one is found, else your local network. Friends on the same VPN or network can join with the address shown here. It is only shown on this screen."),
            _copyAddress, out _routeHint, wide: true), routeRows);
        SettingCombo(SymbolRegular.People24, L.T("Mode"), L.T("Host starts the party; Join connects to the host."),
            new[] { new Choice(L.T("Single player"), ""), new Choice(L.T("Host"), "host"), new Choice(L.T("Join"), "join") }, "party_mode",
            changed: ShowMode, buttons: true);
        var party = Rows(InParty);
        TextRow(SymbolRegular.Person24, L.T("Your name"), L.T("1–16 letters, numbers, underscores or hyphens."), L.T("Hunter"),
            () => SeamlessSettings.Name(S), v => S.Set("party_name", v), party);
        TextRow(SymbolRegular.Link24, L.T("Host code or address"), L.T("Paste the host's Internet code, LAN code or host:port."), "BBP3-…",
            () => S.Str("party_code"), v => S.Set("party_code", v), Rows(Joining));
        party = Rows(InParty);
        var password = new PasswordBox { Width = 300, Password = S.PartyPassword, PlaceholderText = L.T("Optional party password"), PlaceholderEnabled = true };
        password.PasswordChanged += (_, _) => { S.PartyPassword = password.Password; Ws.SaveSoon(); };
        Add(Card(SymbolRegular.Password24, L.T("Password"), L.T("Use the same password on every PC. Stored encrypted for your Windows user."), password, out _), party);
        ComboRow(SymbolRegular.People24, L.T("Party size"), L.T("Host and guests use the same limit. Five players is experimental."),
            new[] { new Choice("2", "2"), new Choice("3", "3"), new Choice("4", "4"), new Choice(L.T("5 (experimental)"), "5") },
            () => S.Str("party_max") is { Length: > 0 } n ? n : "3", v => S.Set("party_max", v), party);

        _byMode.Add((Group(L.T("Connection")), InParty));
        party = Rows(InParty);
        TextRow(SymbolRegular.Router24, L.T("Party port"), L.T("TCP and UDP. Default: 9307."), "9307", () => S.Str("party_port"), v => S.Set("party_port", v), party, width: 120);
        ToggleRow(SymbolRegular.Router24, L.T("UPnP"), L.T("Map the host's port automatically."), () => SeamlessSettings.Flag(S, "party_upnp"), v => S.Set("party_upnp", v), Rows(Hosting));
        party = Rows(InParty);
        ToggleRow(SymbolRegular.Globe24, L.T("DHT rendezvous"), L.T("Optional NAT traversal through the public BitTorrent DHT. Enable on every PC to use it."),
            () => Ini.Get("party_rendezvous", "0") == "1", v => Ini.Set("party_rendezvous", v ? "1" : "0"), party);
        // Kept in bbport.ini like the game's System > Party network rows, so both show the same value.
        ToggleRow(SymbolRegular.People24, L.T("Relay for party members"), L.T("A member that cannot reach the host joins through you (party members only, capped upload)."),
            () => Ini.Get("party_member_relay", "1") == "1", v => Ini.Set("party_member_relay", v ? "1" : "0"), party);
        ToggleRow(SymbolRegular.Globe24, L.T("IPv6"), L.T("Try IPv6 when both PCs have it. A host without an open port then puts its IPv6 address into the party code."),
            () => Ini.Get("party_ipv6", "0") == "1", v => Ini.Set("party_ipv6", v ? "1" : "0"), party);
        TextRow(SymbolRegular.Server24, L.T("STUN server"), L.T("Empty uses stun.l.google.com:19302. Use off to disable."), L.T("Default"),
            () => S.Str("party_stun"), v => S.Set("party_stun", v), party);
        var host = Rows(Hosting);
        TextRow(SymbolRegular.Globe24, L.T("Host public address"), L.T("Leave on Automatic unless guests reach you another way: your VPN address (Radmin VPN, Tailscale, ZeroTier), or your public IP or DNS name when you forwarded the port yourself (host or host:port). The party code then carries it."), L.T("Automatic"),
            () => S.Str("party_public_addr"), v => S.Set("party_public_addr", v), host);
        ComboRow(SymbolRegular.Router24, L.T("Host over a VPN"), L.T("Auto picks your Radmin VPN, Hamachi, ZeroTier or Tailscale adapter when you have one, so friends in that network join with the address above. Off uses your network and Internet address. Read when the game starts."),
            VpnChoices(), VpnCurrent, v => Ini.Set("party_vpn", v), host, changed: UpdateRoute);
        ToggleRow(SymbolRegular.People24, L.T("Show my party to friends"), L.T("While you host, the game announces the party's name, level and players on your VPN and local network, so friends see it under Parties on your network when they join. Never the password or your address. Takes effect at once."),
            () => Ini.Get("party_discovery", "1") == "1", v => Ini.Set("party_discovery", v ? "1" : "0"), host);
        _codes = Text("", "Faint");
        _codes.TextWrapping = TextWrapping.Wrap;
        host.Children.Add(_codes);
        Add(Card(SymbolRegular.Copy24, L.T("Share party codes"), L.T("Available after the host starts. Use the LAN code on the same router."),
            SmallButton(L.T("Copy codes"), SymbolRegular.Copy24, (_, _) => Clipboard.SetText(SeamlessSettings.Codes(S))), out _), host);

        party = Rows(InParty);
        _status = Text(L.T("Start the game to see party status."), "Faint");
        _status.TextWrapping = TextWrapping.Wrap;
        party.Children.Add(_status);
        foreach (var (title, action) in new[] { (L.T("Leave party"), "leave"), (L.T("Rejoin party"), "rejoin") })
        {
            var command = action;
            Add(Card(SymbolRegular.People24, title, null,
                SmallButton(title, SymbolRegular.People24, (_, _) => {
                    try { SeamlessSession.Command(S, command); }
                    catch (Exception e) { Prompt.Tell(L.T("Party"), e.Message); }
                }), out _), party);
        }
        host = Rows(Hosting);
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
            }), out _), host);
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

        var join = Rows(Joining);
        Add(Card(SymbolRegular.Link24, L.T("Guest connection code"), L.T("Guests can share this with a host when NAT prevents a direct connection."),
            SmallButton(L.T("Copy guest code"), SymbolRegular.Copy24, (_, _) => {
                var code = SeamlessSession.Read(S)?.guest_code;
                if (string.IsNullOrEmpty(code)) Prompt.Tell(L.T("Party"), L.T("No guest connection code is available yet."));
                else Clipboard.SetText(code);
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

        // Party settings as in the game's System > Party rules / My party pages (visy 0.7.3): the host's
        // rules for the whole party, then what each player chooses for itself.
        _byMode.Add((Group(L.T("Party rules")), Hosting));
        host = Rows(Hosting);
        host.Children.Add(Text(L.T("The host's rules for the whole party. When you join, the host's count and yours are kept for when you host (in the game: System > Party rules)."), "Faint"));
        IniToggles(host,
            ("party_kill_record", L.T("Enemies stay dead"), true), ("party_gather", L.T("Host can gather the party"), true),
            ("party_npc_mirror", L.T("NPCs as in the host's world"), true),
            ("party_covenant_per_player", L.T("Covenant outcomes per player"), true),
            ("party_travel_vote", L.T("Party votes on travel"), false),
            ("party_npc_fights", L.T("NPC hunters fight the whole party"), true),
            ("party_any_kidnap", L.T("Anyone can be abducted"), true),
            ("party_friendly_fire", L.T("Friendly fire / PvP"), false), ("party_share_lamps", L.T("Shared lamps"), true),
            ("party_guests_bring_lamps", L.T("Guests bring their lamps"), false),
            ("party_rot", L.T("Rot on death"), false), ("party_rot_cleanse_boss", L.T("Boss kill cleanses rot"), true),
            ("party_rot_cleanse_killer", L.T("Killing your killer cleanses its rot"), true),
            ("party_rot_cleanse_echoes", L.T("Retrieving Blood Echoes cleanses its rot"), true));
        IniNumbers(host,
            ("party_boss_hp", L.T("Boss HP per guest (%)"), 150), ("party_boss_damage", L.T("Boss damage per guest (%)"), 10),
            ("party_boss_poise", L.T("Boss poise per guest (%)"), 100), ("party_enemy_hp", L.T("Enemy HP per guest (%)"), 50),
            ("party_enemy_damage", L.T("Enemy damage per guest (%)"), 0), ("party_enemy_poise", L.T("Enemy poise per guest (%)"), 0));
        // Session rules the host decides (0.7.5): live ones first, then the start-up rules a guest has to match.
        IniToggles(host,
            ("party_open_world", L.T("Remove multiplayer barriers"), true), ("party_story_mirror", L.T("Cutscenes for every player"), true),
            ("party_guest_respawn", L.T("Guest respawn"), true), ("party_guest_revive_inplace", L.T("Get up without a loading screen"), true),
            ("party_guest_refill", L.T("Refill guest supplies"), true), ("party_chalice_rejoin", L.T("Stay in Chalice Dungeons"), true));
        IniToggles(host,
            ("party_level_sync", L.T("Level sync"), true), ("party_phantom_hp", L.T("Phantom health cut"), true));
        StartupRows(host);
        IniToggles(host, ("party_skip_prologue", L.T("Skip the prologue (new characters)"), false));

        // Joining: the rules above are the host's and reach you on connect. Only the start-up rules need your own
        // values to match, so they stay here (and are never changed by the launcher).
        _byMode.Add((Group(L.T("Start-up rules")), Joining));
        var rules = Rows(Joining);
        rules.Children.Add(Text(L.T("The host refuses a guest whose start-up rules differ from its own. Leave them as they are unless the host changed them; if it did, set the same values here."), "Faint"));
        StartupRows(rules);
        _startupWarning = Text("", "Faint");
        _startupWarning.TextWrapping = TextWrapping.Wrap;
        rules.Children.Add(_startupWarning);

        _byMode.Add((Group(L.T("My party")), InParty));
        party = Rows(InParty);
        party.Children.Add(Text(L.T("Your own party settings: your screen, what you get as a guest, story sync (in the game: System > My party)."), "Faint"));
        IniToggles(party,
            ("party_hud", L.T("Party HUD"), true), ("party_hud_ping", L.T("Ping in the party list"), true),
            ("party_name_tags", L.T("Party name tags"), true), ("party_pings", L.T("Pings"), true),
            ("party_through_walls", L.T("Show party through walls"), false),
            ("party_glow_off", L.T("Companions without glow"), true), ("party_toasts", L.T("Join, leave and travel notices"), true),
            ("party_npc_talk", L.T("Share NPC dialogue"), true),
            ("party_key_view", L.T("Own key items as a guest"), true), ("party_key_items_summoned", L.T("Key items at once"), true));
        IniNumbers(party, ("party_npc_talk_range", L.T("NPC dialogue range (m)"), 25), ("party_overhead", L.T("Name tag detail (0–4)"), 3));
        IniToggles(party, SeamlessSettings.GameSwitches.Where(o => SeamlessSettings.OwnSwitches.Contains(o.Key)).Select(o => (o.Key, o.Label, true)).ToArray());
        foreach (var option in SeamlessSettings.Switches)
        {
            var key = option.Key;
            ToggleRow(SymbolRegular.People24, option.Label, PartyInfo(key), () => SeamlessSettings.Flag(S, key), v => S.Set(key, v), party);
        }

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
        Add(Card(SymbolRegular.ArrowExport24, L.T("Export for save editor"), L.T("A copy of one character save that save editors can open. Your save is not changed."),
            SmallButton(L.T("Export"), SymbolRegular.ArrowExport24, async (_, _) => await ExportForEditor()), out _));
        Add(Card(SymbolRegular.ArrowImport24, L.T("Import from save editor"), L.T("Puts an edited export back into its slot. Your saves are backed up first."),
            SmallButton(L.T("Import"), SymbolRegular.ArrowImport24, async (_, _) => await ImportFromEditor()), out _));
        ComboRow(SymbolRegular.Document24, L.T("Party log detail"), L.T("Verbose and trace logging add overhead."),
            new[] { new Choice(L.T("Normal"), ""), new Choice(L.T("Verbose"), "verbose"), new Choice(L.T("Trace"), "trace") },
            () => S.Str("party_log"), v => S.Set("party_log", v));

        _timer.Tick += (_, _) => {
            _codes.Text = SeamlessSettings.Codes(S);
            UpdateStartupWarning();
            if (++_ticks % 5 == 0) RefreshRoute();
            var status = SeamlessSession.Read(S);
            _status.Text = status == null ? (Ws.Game.Running ? L.T("Waiting for party status…") : L.T("Game stopped.")) :
                $"{PartyRole(status.role)}：{PartyState(status.state)}  {status.detail}\n{status.network}\n" +
                string.Join("\n", status.members.Select(m => $"{m.name}：{(m.in_world ? L.T("in host world") : m.connected ? L.T("connected") : L.T("disconnected"))}  {(m.ping_ms < 0 ? "" : m.ping_ms + " ms")}  {m.area}")) +
                $"\n{status.@event}\n{status.alert}";
        };
        Loaded += (_, _) => { _codes.Text = SeamlessSettings.Codes(S); _timer.Start(); RefreshRoute(); };
        Unloaded += (_, _) => _timer.Stop();
        ShowMode();
    }
    private readonly Wpf.Ui.Controls.Button _copyAddress;
    private readonly TextBlock _routeHint;
    private IReadOnlyList<NetAdapter> _adapters = Array.Empty<NetAdapter>();
    private Route _route = Route.None;
    private bool _adaptersBusy;
    private int _ticks;

    private static Choice[] VpnChoices()
    {
        var choices = new List<Choice>
        {
            new(L.T("Auto"), "auto"), new(L.T("Off"), "off"), new("Radmin VPN", "Radmin VPN"), new("Hamachi", "Hamachi"),
            new("ZeroTier", "ZeroTier"), new("Tailscale", "Tailscale"),
        };
        var current = NetworkRoute.Normalize(Ini.Get("party_vpn", "auto"));
        if (!choices.Any(c => c.Value.Equals(current, StringComparison.OrdinalIgnoreCase))) choices.Add(new Choice(current, current));
        return choices.ToArray();
    }

    private static string VpnCurrent()
    {
        var current = NetworkRoute.Normalize(Ini.Get("party_vpn", "auto"));
        return VpnChoices().First(c => c.Value.Equals(current, StringComparison.OrdinalIgnoreCase)).Value;
    }

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
        if (NetworkRoute.OpenNetworkWarning(S.Str("party_mode"), S.PartyPassword, _route))
            lines.Add(L.T("No password over ") + _route.Label + L.T(": anyone in that network can join you. Set a Password."));
        SetHint(_routeHint, string.Join("\n", lines));
        _copyAddress.IsEnabled = _route.Address.Length > 0;
    }

    private TextBlock _startupWarning = null!;

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

    private static string StartupLabel(string key) => key switch
    {
        "party_guest_lamps" => L.T("Guests may use lamps and headstones; the whole party travels with them. The host's rule, read at game start: a guest must have the same value, or the host refuses the join."),
        "party_guest_mark" => L.T("A guest's Hunter's Mark asks whether to travel alone or with the party; off uses the game's own behavior. The host's rule, read at game start: a guest must have the same value, or the host refuses the join."),
        "party_full_rewards" => L.T("Guests receive full Blood Echoes and enemy drops like the host; off uses the game's co-op amounts. The host's rule, read at game start: a guest must have the same value, or the host refuses the join."),
        "party_guest_insight" => L.T("Controls the Insight a guest receives when the host kills a boss. The host's rule, read at game start: a guest must have the same value, or the host refuses the join."),
        _ => L.T("Party starts"),
    };

    /// <summary>On the Join side: names the start-up rules that differ from the defaults, since a host with the defaults
    /// refuses this guest. Nothing is changed here.</summary>
    private void UpdateStartupWarning()
    {
        var differs = SeamlessSettings.StartupDifferences(key => key == "party_start" ? S.Str(key) : Ini.Get(key, ""));
        _startupWarning.Text = differs.Count == 0 ? "" :
            L.T("Not the defaults: ") + string.Join(", ", differs.Select(StartupLabel)) + L.T(". A host that kept its defaults will refuse you until these match.");
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
        UpdateRoute();
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
