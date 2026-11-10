using System.IO;
using System.Security.Cryptography;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// Makes a randomized world from a code or new options, and lists the worlds on this PC. The code alone decides a world,
    /// so the page has one set of options and the code box always shows the code they make: a row changes it, a pasted code
    /// fills the rows. The seed has no row: a new one each time the page opens (unless it holds a pasted code), since the
    /// code is what players share. A pasted code's settings without a row carry over, and a note says when the rows no
    /// longer make the pasted code.
    /// </summary>
    public sealed class RandomizerPage : LauncherPage
    {
        private readonly StackPanel _modChoices = new(), _worlds = new();
        private readonly HashSet<string> _skipped = new();
        private readonly TextBlock _status, _codeNote;
        private readonly Wpf.Ui.Controls.TextBox _codeBox, _nameBox;
        private readonly Button _generate, _cancel, _backToPasted;
        private readonly System.Windows.Controls.ProgressBar _bar = new() { Height = 4, Minimum = 0, Maximum = 1, Visibility = Visibility.Collapsed, Margin = new Thickness(14, 0, 14, 12) };
        private readonly List<(FrameworkElement Control, Action Sync, Func<bool>? Editable)> _rows = new();
        private Randomizer.Options _o = Randomizer.Standard(NewSeed());
        private WorldCodeResult? _pasted;  // the last valid code typed or pasted: its hidden settings and the note's "changed from"
        private string? _code;             // what the code box shows, null while it holds no valid code
        private bool _syncing, _busy;
        private bool _nocturne; // a ticked mod places the Coiled Host (Nocturne v2.0.0+; option coiled_host has him to act on)
        private CancellationTokenSource? _stop;

        private static readonly Choice[] OffOrOn = { new(L.T("Off"), "same"), new(L.T("On"), "swap") };

        public RandomizerPage() : base(L.T("Randomizer"),
            L.T("Each code is one world. Everyone in a party needs the same code and the same mods."))
        {
            Group(L.T("Your world"));
            _codeBox = Box("R2-...", 300, CodeTyped);
            _backToPasted = SmallButton(L.T("Back to it"), SymbolRegular.ArrowUndo24, (_, _) => _codeBox.Text = _pasted?.Canonical ?? "");
            Add(Card(SymbolRegular.Key24, L.T("Code"), L.T("This is the world the options below make, and it changes when you change them. You get a fresh one every time you open this page. To play a friend's world, paste their code here and the options fill in by themselves."),
                Row(_codeBox, _backToPasted), out _codeNote));
            _nameBox = Box(L.T("World 1"), 240, _ => { });
            _nameBox.MaxLength = WorldSaves.MaxName;
            Add(Card(SymbolRegular.Rename24, L.T("World name"), L.T("Names this world's saves on this PC. It is never shared: friends who join name their own copy."),
                _nameBox, out _));

            Group(L.T("Options"));
            var enemies = Section(L.T("Enemies"), open: true);
            Bind(ComboRow(SymbolRegular.ArrowShuffle24, L.T("Enemies that change"), L.T("How many enemies get replaced. The rest stay as they are."),
                new[] { new Choice("100 %", "100"), new Choice("75 %", "75"), new Choice("50 %", "50"), new Choice("25 %", "25") },
                () => _o.Chance.ToString(), v => Set(_o with { Chance = int.Parse(v) }), enemies, changed: Changed), () => _o.Chance.ToString());
            Bool(enemies, SymbolRegular.ArrowExportUp24, L.T("Enemies away from home"), L.T("All enemies are randomized per map, and enemies native to each map never spawn there. An enemy that was waiting in a pose only a native could do just stands instead."),
                o => o.AwayFromHome, (o, b) => o with { AwayFromHome = b });
            Bool(enemies, SymbolRegular.ArrowShuffle24, L.T("More randomness"), L.T("Enemies the game scripts an animation on (eating, sleeping, sitting, appearing, ambush attacks) can become any enemy. If the new one lacks that animation, it just stands there instead."),
                o => o.FreePoses, (o, b) => o with { FreePoses = b });
            Bool(enemies, SymbolRegular.PeopleTeam24, L.T("Random summons in the Shadows' arena"), L.T("On, the snakes the Shadows of Yharnam summon are random enemies. Off, nothing is summoned in that arena, whoever fights there."),
                o => o.ShadowsSummons, (o, b) => o with { ShadowsSummons = b });
            Bool(enemies, SymbolRegular.Crown24, L.T("Include chalice dungeon enemies"), L.T("The regular enemies of the Chalice Dungeons, such as Merciless Watchers, Keepers of the Old Lords and Gravekeeper Scorpions, can replace enemies in the main game. Chalice bosses are not included (they have their own option under Bosses). They always give the echoes and drops of the enemy they replace, whatever Echoes and drops says. The dungeons themselves don't change. Making the world takes a little longer."),
                o => o.ChaliceEnemies, (o, b) => o with { ChaliceEnemies = b });
            Bool(enemies, SymbolRegular.Lightbulb24, L.T("Include Winter Lanterns"), L.T("Whether Winter Lanterns can replace other enemies. They inflict Frenzy from a distance just by seeing you, often before you see them, and from up high where you can't reach them. Off, they never turn up anywhere new; the ones the game places itself can still be replaced by other enemies. The Brain of Mensis never moves either way."),
                o => !o.NoWinterLanterns, (o, b) => o with { NoWinterLanterns = !b });
            var exclude = SmallButton(L.T("None"), SymbolRegular.Prohibited24, (_, _) => PickExcluded());
            Add(Card(SymbolRegular.Prohibited24, L.T("Never use these enemies"), L.T("Enemies the randomizer will never place anywhere."), exclude, out _), enemies);
            _rows.Add((exclude, () => exclude.Content = ExcludeSummary(_o.Exclude), null));
            Bind(ComboRow(SymbolRegular.ArrowShuffle24, L.T("Enemy variety"), L.T("Every area has a memory limit: how much the game loads there as it ships. A replacement has to fit under that limit, and this setting raises it, along with how many kinds of enemies an area can hold. 100 % keeps the limit as it is: the fewest crashes, but less variety. 150 % and 200 % give you more kinds of enemies, but can crash weaker PCs when memory runs out. 100 to 150 % crashes the least."),
                new[] { new Choice(L.T("100 % (as the game ships)"), "100"), new Choice("150 %", "150"), new Choice("200 %", "200") },
                () => _o.Headroom.ToString(), v => Set(_o with { Headroom = int.Parse(v) }), enemies, changed: Changed), () => _o.Headroom.ToString());
            Bool(enemies, SymbolRegular.Dumbbell24, L.T("Difficult enemies"), L.T("Replacements come from the toughest enemies in the game, as long as the area has the memory for them."),
                o => o.DifficultEnemies, (o, b) => o with { DifficultEnemies = b });
            Bool(enemies, SymbolRegular.Crown24, L.T("Bosses as enemies"), L.T("Now and then a boss turns up in place of an ordinary enemy (how often: the two rows below). Its health is cut to fit the area and is never more than in its own fight (how much: Boss HP as an enemy, below) and fights like a normal enemy there: no health bar, no fog wall, no boss music, and it drops what the enemy it replaced would have. Its own boss fight stays as it was. Bosses that need their arena to work (Moon Presence, the Witch of Hemwick, the Shadows of Yharnam, Logarius, the Wet Nurse, the One Reborn, the Orphan's second phase, the Blood-starved Beast) never do this. Chalice dungeon bosses join when they are included."),
                o => o.BossesAsEnemies, (o, b) => o with { BossesAsEnemies = b });
            var bossEnemyHint = L.T("Only matters when bosses as enemies is on.");
            Bind(ComboRow(SymbolRegular.Crown24, L.T("Areas with bosses as enemies"), L.T("The chance for each area to have at least one boss in place of its ordinary enemies.") + " " + bossEnemyHint,
                new[] { new Choice("10 %", "10"), new Choice(L.T("25 % (standard)"), "25"), new Choice("50 %", "50"), new Choice("75 %", "75"), new Choice("100 %", "100") },
                () => _o.BossEnemyChance.ToString(), v => Set(_o with { BossEnemyChance = int.Parse(v) }), enemies, changed: Changed),
                () => _o.BossEnemyChance.ToString(), () => _o.BossesAsEnemies);
            Bind(ComboRow(SymbolRegular.Crown24, L.T("Bosses per area"), L.T("The most bosses one area can have as enemies. An area that gets bosses has between one and this many, each a different boss, as long as the area has the memory for them.") + " " + bossEnemyHint,
                new[] { new Choice(L.T("1 (standard)"), "1"), new Choice("2", "2"), new Choice("3", "3"), new Choice("4", "4"), new Choice("5", "5") },
                () => _o.BossEnemyMax.ToString(), v => Set(_o with { BossEnemyMax = int.Parse(v) }), enemies, changed: Changed),
                () => _o.BossEnemyMax.ToString(), () => _o.BossesAsEnemies);
            Bind(ComboRow(SymbolRegular.Crown24, L.T("Boss HP as an enemy"), L.T("A boss standing in for a normal enemy gets this many times the HP of a typical enemy of that area. Then the area's normal difficulty scaling applies to it, like to any enemy. It never goes above the boss's own HP in its real boss fight. Example: in Central Yharnam a typical enemy has about 80 base HP, so 8x gives a boss about 640 HP and 16x about 1,280. In late areas, where the scaling is much higher, the limit takes over: Father Gascoigne has 2,031 HP in his own fight, so as an enemy in the Hunter's Nightmare he has about 2,024 HP at 8x and at 16x alike, never more.") + " " + bossEnemyHint,
                new[] { new Choice(L.T("8x (standard)"), "8"), new Choice("10x", "10"), new Choice("12x", "12"), new Choice("14x", "14"), new Choice("16x", "16") },
                () => _o.BossEnemyHp.ToString(), v => Set(_o with { BossEnemyHp = int.Parse(v) }), enemies, changed: Changed),
                () => _o.BossEnemyHp.ToString(), () => _o.BossesAsEnemies);
            Bool(enemies, SymbolRegular.Crown24, L.T("Huge bosses as enemies"), L.T("Also lets the huge bosses stand in for ordinary enemies: Vicar Amelia, Darkbeast Paarl, Ebrietas, Rom, Ludwig and the big chalice dungeon bosses. They are huge and can get stuck in narrow places such as alleys, stairs and doorways.") + " " + bossEnemyHint,
                o => o.HugeBossesAsEnemies, (o, b) => o with { HugeBossesAsEnemies = b }, null, () => _o.BossesAsEnemies);
            Bind(ComboRow(SymbolRegular.Dumbbell24, L.T("Enemy scaling"), L.T("Randomized enemies scale to the current area."),
                new[] { new Choice(L.T("Off"), "off"), new Choice(L.T("On"), "slot") },
                () => _o.Scaling, v => Set(_o with { Scaling = v }), enemies, changed: Changed), () => _o.Scaling);
            Bind(ComboRow(SymbolRegular.Gift24, L.T("Echoes and drops"), L.T("Whose echoes and drops you get when you kill a randomized enemy."),
                new[] { new Choice(L.T("Original enemy's"), "slot"), new Choice(L.T("Randomized enemy's"), "replacement") },
                () => _o.Rewards, v => Set(_o with { Rewards = v }), enemies, changed: Changed), () => _o.Rewards);

            var items = Section(L.T("Items"), open: false);
            Bool(items, SymbolRegular.Gift24, L.T("Randomize pickup items"), L.T("Items lying around the world swap places. Keys and anything you need to get further stay where they are."), o => o.Treasure, (o, b) => o with { Treasure = b });
            Bind(ComboRow(SymbolRegular.Gift24, L.T("Randomize pickup items with"), L.T("What a pickup item can swap with. Only matters when pickup items are randomized."),
                new[] { new Choice(L.T("Anything with anything"), "any"), new Choice(L.T("Weapons with weapons, armour with armour, etc."), "kind") },
                () => _o.TreasureByType ? "kind" : "any", v => Set(_o with { TreasureByType = v == "kind" }), items, width: 360, changed: Changed), () => _o.TreasureByType ? "kind" : "any");
            Bool(items, SymbolRegular.Cart24, L.T("Randomize shops"), L.T("Every shop item changes except Blood Vials and Quicksilver Bullets. Weapons and armor swap places with one-of-a-kind items found in the world, so a shop weapon can become a pickup and a world-only weapon can be bought; the rest are shuffled within their shop. Consumables are shuffled between the shops. Prices fit the new item. Keys and boss drops stay where they are."), o => o.Shops, (o, b) => o with { Shops = b });
            Bool(items, SymbolRegular.Gift24, L.T("Randomize enemy drops"), L.T("Regular enemies swap what they drop. Bosses keep theirs."), o => o.Drops, (o, b) => o with { Drops = b });
            Bool(items, SymbolRegular.Diamond24, L.T("Gem roulette"), L.T("Every blood gem lying in the world, half of the small Blood Vial and Quicksilver Bullet pickups, and every blood gem enemies drop become droplet gems with three random effects. Droplet gems fit any slot of any weapon. Gems get stronger the later the area. The effects are fixed by the world code, so everyone in the world finds the same gems, and the spoiler lists them."),
                o => o.GemRoulette, (o, b) => o with { GemRoulette = b });
            Bool(items, SymbolRegular.Diamond24, L.T("Cursed gems"), L.T("About one gem in seven is cursed: one of its effects is a drawback (such as lower attack or HP that drains), and in return its other two effects are stronger. Only matters when the gem roulette is on."),
                o => o.CursedGems, (o, b) => o with { CursedGems = b }, null, () => _o.GemRoulette);
            Bool(items, SymbolRegular.Gift24, L.T("Randomize chalice materials"), L.T("Chalice ritual materials can turn up anywhere, the ones sold in shops too. Not recommended: you won't know where to find them, and you may get stuck before the chalice dungeons."),
                o => o.ChaliceMaterials, (o, b) => o with { ChaliceMaterials = b });
            Bool(items, SymbolRegular.Gift24, L.T("Randomize starting armor"), L.T("A new character starts in a random head, chest, arms and legs piece, from any armor the game hands out (mod armor too)."), o => o.StarterArmor, (o, b) => o with { StarterArmor = b });
            Bool(items, SymbolRegular.Gift24, L.T("Randomize starting weapons"), L.T("The free weapons in the Hunter's Dream are drawn again, and you can use them with any stats."), o => o.Starters, (o, b) => o with { Starters = b });
            Bind(ComboRow(SymbolRegular.Gift24, L.T("Starting melee weapons offered"), L.T("How many free melee weapons the Hunter's Dream offers. Above 3 adds random ones you can use with any stats. You still pick one."),
                new[] { new Choice("1", "1"), new Choice("2", "2"), new Choice(L.T("3 (standard)"), "3"), new Choice("4", "4"), new Choice("5", "5") },
                () => _o.StarterMelee.ToString(), v => Set(_o with { StarterMelee = int.Parse(v) }), items, changed: Changed), () => _o.StarterMelee.ToString());
            Bind(ComboRow(SymbolRegular.Gift24, L.T("Starting firearms offered"), L.T("How many free firearms the Hunter's Dream offers. Above 2 adds a random one you can use with any stats. You still pick one."),
                new[] { new Choice("1", "1"), new Choice(L.T("2 (standard)"), "2"), new Choice("3", "3") },
                () => _o.StarterGuns.ToString(), v => Set(_o with { StarterGuns = int.Parse(v) }), items, changed: Changed), () => _o.StarterGuns.ToString());

            var bosses = Section(L.T("Bosses"), open: false);
            Bool(bosses, SymbolRegular.Crown24, L.T("Randomize bosses"), L.T("Bosses swap arenas, fights with more than one body included. Micolash stays where he is."),
                o => o.Bosses, (o, b) => o with { Bosses = b });

            Subheading(bosses, L.T("Fights"));
            var fightHint = L.T("This fight normally has several bosses at once. \"One boss only\" puts a single boss alone in the room; \"A boss for each\" gives every one its own boss. Only matters when bosses are randomized.");
            Bool(bosses, SymbolRegular.ArrowSplit24, L.T("Randomize second phase"),
                L.T("Father Gascoigne, Ludwig and the Orphan of Kos are the only bosses that change into a second form. With this on, they always turn into another boss at that point, one that fights nowhere else in the world, and that boss never changes again. Two chalice bosses join the pool for this, even when chalice bosses are off. With this off, one of the three in an arena that isn't its own fights its first phase only, and the fight ends when that phase dies."),
                o => o.SecondStage, (o, b) => o with { SecondStage = b }, null, () => _o.Bosses);
            Bool(bosses, SymbolRegular.ArrowSwap24, L.T("Include Coiled Host (Nocturne boss)"),
                L.T("Needs Nocturne v2.0.0 or later among the ticked mods below, which adds the Coiled Host as the Shadows of Yharnam's second phase; without it this row is greyed out and does nothing. With this on, the Coiled Host can turn up as the boss of another fight, and after the Shadows die a random boss comes out in his place."),
                o => o.CoiledHost, (o, b) => o with { CoiledHost = b }, null, () => _o.Bosses && _nocturne);
            Bind(ComboRow(SymbolRegular.Alert24, L.T("Randomize bell maidens"), L.T("The One Reborn's bell maidens become random enemies."),
                new[] { new Choice(L.T("Off"), Randomizer.BellMaidenChoices[0]), new Choice(L.T("On"), Randomizer.BellMaidenChoices[1]) },
                () => _o.BellMaidens, v => Set(_o with { BellMaidens = v }), bosses, changed: Changed), () => _o.BellMaidens, () => _o.Bosses);
            Bool(bosses, SymbolRegular.Wand24, L.T("Witch of Hemwick"), fightHint,
                o => o.WitchMix, (o, b) => o with { WitchMix = b },
                new[] { new Choice(L.T("One boss only"), "same"), new Choice(L.T("A boss for each"), "swap") }, () => _o.Bosses);
            Bool(bosses, SymbolRegular.PeopleTeam24, L.T("Shadows of Yharnam"), fightHint,
                o => o.ShadowsMix, (o, b) => o with { ShadowsMix = b },
                new[] { new Choice(L.T("One boss only"), "same"), new Choice(L.T("A boss for each"), "swap") }, () => _o.Bosses);
            var oneOrEach = new[] { new Choice(L.T("One boss only"), "same"), new Choice(L.T("A boss for each boss' HP"), "swap") };
            Bool(bosses, SymbolRegular.Sparkle24, L.T("Celestial Emissary"), fightHint,
                o => o.EmissaryMix, (o, b) => o with { EmissaryMix = b }, oneOrEach, () => _o.Bosses);
            Bool(bosses, SymbolRegular.Beaker24, L.T("Living Failures"), fightHint,
                o => o.FailuresMix, (o, b) => o with { FailuresMix = b }, oneOrEach, () => _o.Bosses);

            Subheading(bosses, L.T("Chalice"));
            Bool(bosses, SymbolRegular.Crown24, L.T("Include chalice dungeon bosses"), L.T("The strongest chalice dungeon bosses join the boss pool, and the bosses that can replace enemies. The dungeons themselves don't change.") + " " + L.T("Only matters when bosses are randomized or bosses as enemies is on."),
                o => o.ChaliceBosses, (o, b) => o with { ChaliceBosses = b }, null, () => _o.Bosses || _o.BossesAsEnemies);
            var strengthChoices = new[] { new Choice(L.T("Main game"), "same"), new Choice(L.T("Chalice variant (stronger)"), "swap") };
            var strengthHint = L.T("Fights as its strongest chalice dungeon version, in its own arena or wherever it ends up.");
            Bool(bosses, SymbolRegular.Dumbbell24, L.T("Rom"), strengthHint, o => o.HardRom, (o, b) => o with { HardRom = b }, strengthChoices);
            Bool(bosses, SymbolRegular.Dumbbell24, L.T("Amygdala"), strengthHint, o => o.HardAmygdala, (o, b) => o with { HardAmygdala = b }, strengthChoices);
            Bool(bosses, SymbolRegular.Dumbbell24, L.T("Ebrietas"), strengthHint, o => o.HardEbrietas, (o, b) => o with { HardEbrietas = b }, strengthChoices);

            Group(L.T("Included mods"));
            Body.Children.Add(Text(L.T("Ticked mods are built into the world and stay on. A world can't keep up with a later change to this list."), "CaptionTextBlockStyle"));
            Body.Children.Add(_modChoices);

            _generate = SmallButton(L.T("Generate"), SymbolRegular.Play24, async (_, _) => await GenerateNow(), ControlAppearance.Primary);
            _cancel = SmallButton(L.T("Stop"), SymbolRegular.Stop24, (_, _) => _stop?.Cancel());
            _cancel.Visibility = Visibility.Collapsed;
            Add(Card(SymbolRegular.Save24, L.T("Generate"), L.T("Each code has its own characters. Your other saves are safe. Takes a minute or two."), Row(_cancel, _generate), out _status));
            _bar.Template = (ControlTemplate)System.Windows.Markup.XamlReader.Parse("""
                <ControlTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ProgressBar">
                    <Grid SnapsToDevicePixels="True">
                        <Border Name="PART_Track" Background="{DynamicResource ControlFillColorSecondaryBrush}" CornerRadius="2" />
                        <Border Name="PART_Indicator" HorizontalAlignment="Left" Background="{DynamicResource AccentTextFillColorPrimaryBrush}" CornerRadius="2" />
                    </Grid>
                </ControlTemplate>
                """);
            Body.Children.Add(new Grid { Height = 16, Children = { _bar } });

            Group(L.T("Worlds on this PC"));
            Body.Children.Add(_worlds);
            Loaded += (_, _) => Sync();
        }

        /// <summary>A new seed on each visit, unless the page holds a pasted code (the code is what players share).</summary>
        public override void OnShown()
        {
            base.OnShown();
            if (!_busy && (_pasted == null || _code != _pasted.Canonical))
            {
                _o = _o with { Seed = NewSeed() };
                if (IsLoaded) Sync();
            }
            Refresh();
        }

        // ---------------------------------------------------------------- options

        private Panel Section(string title, bool open)
        {
            var rows = Advanced(title);
            if (open && rows.Parent is AdvancedSection section) section.IsOpen = true;
            return rows;
        }

        private static void Subheading(Panel into, string title)
        {
            var label = Text(title, "CaptionTextBlockStyle");
            label.Margin = new Thickness(14, 16, 14, 6);
            label.SetResourceReference(TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
            into.Children.Add(label);
        }

        /// <summary>A text box that reports every change (the rows' own TextRow cannot sit beside a button).</summary>
        private static Wpf.Ui.Controls.TextBox Box(string placeholder, double width, Action<string> changed)
        {
            var box = new Wpf.Ui.Controls.TextBox { PlaceholderText = placeholder, PlaceholderEnabled = true, Width = width, VerticalAlignment = VerticalAlignment.Center };
            box.TextChanged += (_, _) => changed(box.Text);
            return box;
        }

        /// <summary>A drop-down row that Sync puts back to the options' value (none selected for a value it does not offer,
        /// such as 35 % from a code).</summary>
        private void Bind(FrameworkElement control, Func<string> get, Func<bool>? editable = null)
        {
            if (control is not ComboBox box) throw new ArgumentException("a drop-down row", nameof(control));
            var parent = box.Parent as FrameworkElement;
            while (parent != null && parent is not SettingRow) parent = parent.Parent as FrameworkElement;
            var row = (SettingRow)parent!;
            if (row.Parent is Panel panel && panel.Children.IndexOf(row) is > 0 and var index && panel.Children[index - 1] is TextBlock)
                row.BorderThickness = new Thickness(0);
            _rows.Add((box, () =>
            {
                box.SelectedIndex = box.Items.Cast<ComboBoxItem>().ToList().FindIndex(item => item.Tag as string == get());
                row.Opacity = !_busy && (editable?.Invoke() ?? true) ? 1 : 0.8;
            }, editable));
        }

        private void Bool(Panel into, SymbolRegular icon, string title, string hint, Func<Randomizer.Options, bool> get, Func<Randomizer.Options, bool, Randomizer.Options> set,
            IReadOnlyList<Choice>? choices = null, Func<bool>? editable = null) =>
            Bind(ComboRow(icon, title, hint, choices ?? OffOrOn, () => get(_o) ? "swap" : "same", v => Set(set(_o, v == "swap")), into, changed: Changed),
                () => get(_o) ? "swap" : "same", editable);

        private void Set(Randomizer.Options options)
        {
            if (!_syncing) _o = options;
        }

        /// <summary>A row changed by hand: rows that depend on it follow.</summary>
        private void Changed()
        {
            if (_syncing) return;
            Sync();
        }

        private void PickExcluded()
        {
            var picker = new EnemyPickerDialog(_o.Exclude) { Owner = Window.GetWindow(this) };
            if (picker.ShowDialog() != true) return;
            _o = _o with { Exclude = picker.Result };
            Sync();
        }

        private static string ExcludeSummary(IReadOnlyList<int> models) => models.Count switch
        {
            0 => L.T("None"),
            1 => EnemyCatalog.All.FirstOrDefault(e => e.Model == models[0])?.Name ?? "c" + models[0].ToString("0000"),
            _ => L.F($"{models.Count} enemies"),
        };

        /// <summary>Puts the options into every row, and the code they make into the code box.</summary>
        private void Sync()
        {
            _syncing = true;
            try
            {
                foreach (var (control, sync, editable) in _rows)
                {
                    sync();
                    control.IsEnabled = !_busy && (editable?.Invoke() ?? true);
                }
                _code = Randomizer.ToCode(_o, _pasted?.Options);
                _codeBox.Text = _code ?? "";
                _codeBox.IsEnabled = _backToPasted.IsEnabled = !_busy;
            }
            finally
            {
                _syncing = false;
            }
            ShowCodeNote();
        }

        /// <summary>The code box changed by hand: a valid code fills the options; anything else waits, and the note says why.</summary>
        private void CodeTyped(string text)
        {
            if (_syncing) return;
            var decoded = WorldCode.Decode(text);
            if (decoded.Kind != WorldCodeKind.Ok)
            {
                _code = null;
                ShowCodeNote(text.Trim().Length == 0 ? L.T("Paste a code, or change any option to get one.")
                    : decoded.Kind == WorldCodeKind.Newer ? Randomizer.ExitText(3) : L.T("Not a world code yet. Codes look like R2-0W000-44JM2-00MP5."));
                return;
            }
            _pasted = decoded;
            _o = Randomizer.FromCode(decoded).Options;
            Sync();
        }

        /// <summary>Under the code: what it shuffles, or why there is no code; and when the options moved away from a pasted
        /// code, that this is no longer that world.</summary>
        private void ShowCodeNote(string? problem = null)
        {
            var changed = problem == null && _code != null && _pasted is { Canonical: { } pasted } && _code != pasted;
            _backToPasted.Visibility = changed ? Visibility.Visible : Visibility.Collapsed;
            if (problem != null || _code == null)
            {
                SetHint(_codeNote, problem ?? L.T("Paste a code, or change any option to get one."));
                return;
            }
            var (options, hidden) = Randomizer.FromCode(WorldCode.Decode(_code));
            var note = Randomizer.Summary(options, hidden);
            if (options.Bosses && options.CoiledHost && !_nocturne)
                note += " " + L.T("The Coiled Host option is skipped: no Nocturne v2.0.0 or later among the ticked mods.");
            if (changed) note = L.F($"Changed from the code you pasted ({_pasted!.Canonical}): this is not that world any more.") + " " + note;
            SetHint(_codeNote, note);
        }

        private static ulong NewSeed() => BitConverter.ToUInt64(RandomNumberGenerator.GetBytes(8)) & 0xFFFFFFFFUL; // codes hold a 32-bit seed

        // ---------------------------------------------------------------- worlds

        private List<Mods.Entry> Installed() => Mods.Load(GameEnvironment.ModsDir(S));

        /// <summary>The enabled mods a new world is built on, in load order (worlds and unticked mods left out).</summary>
        private List<string> PickedMods() =>
            Installed().Where(m => Randomizer.Parse(m.Name) == null && m.Enabled && !_skipped.Contains(m.Name)).Select(m => m.Name).ToList();

        private string ProfileFolder => PlayerProfiles.Folder(S, PlayerProfiles.Current(S));

        private void Refresh()
        {
            _nameBox.PlaceholderText = WorldSaves.DefaultName(ProfileFolder);
            var mods = Installed();
            _modChoices.Children.Clear();
            foreach (var mod in mods.Where(m => Randomizer.Parse(m.Name) == null))
            {
                var box = new CheckBox { Content = mod.Name, IsChecked = mod.Enabled && !_skipped.Contains(mod.Name), Margin = new Thickness(0, 2, 0, 2) };
                var name = mod.Name;
                box.Checked += (_, _) => { _skipped.Remove(name); CheckNocturne(); };
                box.Unchecked += (_, _) => { _skipped.Add(name); CheckNocturne(); };
                _modChoices.Children.Add(box);
            }
            if (_modChoices.Children.Count == 0)
                _modChoices.Children.Add(Empty(L.T("No other mods installed: the world is built on the game alone.")));
            CheckNocturne();

            _worlds.Children.Clear();
            var active = Randomizer.Active(S);
            foreach (var mod in mods)
            {
                if (Randomizer.Parse(mod.Name) is not { } world) continue;
                _worlds.Children.Add(WorldCard(world, world == active, mods));
            }
            if (_worlds.Children.Count == 0)
                _worlds.Children.Add(Empty(L.T("No worlds yet. Generate one above.")));
        }

        /// <summary>Whether a ticked mod places the Coiled Host (Nocturne v2.0.0+), worked out off the UI thread (it reads a map file);
        /// the Coiled Host row follows.</summary>
        private async void CheckNocturne()
        {
            var dir = GameEnvironment.ModsDir(S);
            var picked = PickedMods();
            bool found;
            try { found = await Task.Run(() => Randomizer.HasCoiledHost(dir, picked)); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { found = false; }
            if (found == _nocturne || !picked.SequenceEqual(PickedMods())) return;
            _nocturne = found;
            Sync();
        }

        // added after the page sorts rows into panes, so it gets a row's inset itself (else it sits on the pane's edge)
        private static TextBlock Empty(string text)
        {
            var block = Text(text, "Muted");
            block.Margin = new Thickness(14, 12, 14, 12);
            return block;
        }

        private SettingRow WorldCard(Randomizer.World world, bool inUse, List<Mods.Entry> mods)
        {
            var name = world.Folder;
            var folder = Path.Combine(GameEnvironment.ModsDir(S), name);
            var builtOn = Randomizer.BuiltOn(folder);
            var stale = builtOn != null && Randomizer.IsStale(builtOn, mods, name);
            var (shareable, baseLabel) = builtOn != null ? Randomizer.BaseLabel(GameEnvironment.ModsDir(S), builtOn) : (false, null);
            // Base mods by content (one key per mod, in order): a friend needs the same files, under any folder name.
            var keys = (baseLabel ?? "").Split('+', StringSplitOptions.RemoveEmptyEntries);
            var byContent = builtOn != null && keys.Length == builtOn.Count
                ? builtOn.Select((n, i) => (Name: n, Key: keys[i])).Where(x => x.Key.StartsWith(CuratedMods.ModToken, StringComparison.Ordinal))
                    .Select(x => $"{x.Name} ({x.Key})").ToList()
                : new List<string>();
            var decoded = WorldCode.Decode(world.Code);
            var summary = "";
            if (decoded.Kind == WorldCodeKind.Ok)
            {
                var (options, hidden) = Randomizer.FromCode(decoded);
                summary = Randomizer.Summary(options, hidden) + " ";
            }
            var note = (inUse ? L.T("In use.") : L.T("Not in use.")) + " " + summary + Randomizer.BuiltOnText(builtOn) +
                       (builtOn != null && !shareable ? " " + L.T("Friends can't join it from the Join list: one of the mods it is built on is missing, can't be read or has the same files as another.") : "") +
                       (byContent.Count > 0 ? " " + L.F($"Friends who join it from the Join list need the same files of: {string.Join(", ", byContent)} (any folder name).") : "") +
                       (stale ? " " + L.T("Your mod list changed since this world was made: make it again on your mods, with a new character.") : "");
            var buttons = Row(
                inUse ? SmallButton(L.T("Stop using"), SymbolRegular.Power24, (_, _) => StopUsing())
                      : SmallButton(L.T("Load"), SymbolRegular.Play24, (_, _) => Load(name)),
                SmallButton(L.T("Copy code"), SymbolRegular.Copy24, (_, _) => ClipboardText.Set(world.Code)),
                SpoilerButton(world, folder));
            if (stale)
                buttons.Children.Add(SmallButton(L.T("Make again"), SymbolRegular.ArrowClockwise24, async (_, _) => await Regenerate(world)));
            buttons.Children.Add(IconButton(SymbolRegular.Rename24, L.T("Rename"), (_, _) => Rename(world)));
            buttons.Children.Add(IconButton(SymbolRegular.Delete24, L.T("Delete"), (_, _) => Delete(name)));
            foreach (var button in buttons.Children.OfType<UIElement>()) button.IsEnabled = !_busy;
            // The name is this PC's (WorldSaves); a world never played here yet shows its code.
            var saves = WorldSaves.Find(ProfileFolder, world.Code, world.Hash);
            if (saves != null) note = L.F($"Code {world.Code}.") + " " + note;
            var card = Card(SymbolRegular.Globe24, saves?.Name ?? world.Code, null, buttons, out var line, wide: true);
            SetHint(line, note);
            return card;
        }

        /// <summary>The spoiler shows only on a second press within a few seconds, so a stray click spoils nothing.</summary>
        private Button SpoilerButton(Randomizer.World world, string folder)
        {
            Button button = null!;
            var armed = false;
            var reset = new DispatcherTimer { Interval = TimeSpan.FromSeconds(4) };
            reset.Tick += (_, _) => { reset.Stop(); armed = false; button.Content = L.T("Spoiler"); };
            button = SmallButton(L.T("Spoiler"), SymbolRegular.Eye24, (_, _) =>
            {
                if (!armed)
                {
                    armed = true;
                    button.Content = L.T("Press again to show");
                    reset.Start();
                    return;
                }
                reset.Stop();
                armed = false;
                button.Content = L.T("Spoiler");
                if (WorldSpoiler.Read(folder) is { } spoiler)
                {
                    new SpoilerWindow(world.Code, spoiler).Show();
                    return;
                }
                if (Prompt.Confirm(L.T("Spoiler"), L.T("This world was made before the launcher could show spoilers. Open its folder to read randomizer_spoiler.json?"), L.T("Open folder")))
                    Paths.OpenFolder(folder);
            });
            return button;
        }

        /// <summary>Plays this world: game mode "world" (it and the mods it was built on, My mods kept aside), then the Co-op
        /// page with the party mode on Host.</summary>
        private void Load(string name)
        {
            if (!ChangeMode(() => Randomizer.Use(S, name))) return;
            MainWindow.Instance?.ShowHost();
        }

        /// <summary>Brings back the game mode from before the world was loaded.</summary>
        private void StopUsing() => ChangeMode(() => Randomizer.StopUsing(S));

        private bool ChangeMode(Action change)
        {
            if (Ws.Game.Running)
            {
                SetHint(_status, L.T("Close the game to change the game mode."));
                return false;
            }
            change();
            CuratedMods.Apply(S, GameEnvironment.ModsDir(S));
            Ws.SaveSoon();
            Refresh();
            return true;
        }

        /// <summary>A new name for this world's saves on this PC; nothing moves.</summary>
        private void Rename(Randomizer.World world)
        {
            var current = WorldSaves.Find(ProfileFolder, world.Code, world.Hash)?.Name ?? "";
            var answer = Prompt.Ask(L.T("Rename world"), L.T("Only on this PC: the name is never shared, and the world and its saves stay as they are."),
                new Prompt.Field(L.T("World name"), current, WorldSaves.DefaultName(ProfileFolder)));
            if (answer == null) return;
            if (WorldSaves.ValidName(answer[0], out var error) is not { } valid)
            {
                SetHint(_status, error);
                return;
            }
            try
            {
                WorldSaves.SetName(ProfileFolder, world, valid, GameEnvironment.ModsDir(S), mayCopy: !Ws.Game.Running);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                SetHint(_status, L.F($"The name could not be saved: {e.Message}"));
            }
            Refresh();
        }

        private void Delete(string name)
        {
            if (!Prompt.Confirm(L.T("Delete world"), L.F($"Delete the mod folder {name}? Its characters stay on disk and come back if you generate the same code again."), L.T("Delete"))) return;
            Directory.Delete(Path.Combine(GameEnvironment.ModsDir(S), name), recursive: true);
            if (CuratedMods.Mode(S) == "world" && S.Str(Randomizer.WorldKey) == name)
            {
                Randomizer.StopUsing(S);
                CuratedMods.Apply(S, GameEnvironment.ModsDir(S));
                Ws.SaveSoon();
            }
            Refresh();
        }

        // ---------------------------------------------------------------- generating

        private async Task GenerateNow()
        {
            string? worldName = null;
            if (_nameBox.Text.Trim().Length > 0)
            {
                if (WorldSaves.ValidName(_nameBox.Text, out var error) is not { } valid)
                {
                    SetHint(_status, error);
                    return;
                }
                worldName = valid;
            }
            if (_code == null)
            {
                SetHint(_status, L.T("There is no code to make: paste a whole code."));
                return;
            }
            await Build(_code, PickedMods(), name => L.F($"Made {name}. It is on, and has its own saves."), worldName);
        }

        /// <summary>The same code again on today's mods: a new world (its own saves); the old one stays until deleted.</summary>
        private async Task Regenerate(Randomizer.World world)
        {
            if (!Prompt.Confirm(L.T("Make again"), L.F($"Make {world.Code} again on the mods you have on now? It becomes a new world with its own characters; the old one stays until you delete it."), L.T("Make again"))) return;
            await Build(world.Code, Installed().Where(m => Randomizer.Parse(m.Name) == null && m.Enabled).Select(m => m.Name).ToList(),
                name => L.F($"Made {name} on your current mods. It is on, and has its own saves."));
        }

        // The host's name for the world it built (WorldSaves); a name it had here before stays unless a new one was typed.
        private void NameBuilt(string folder, string? worldName)
        {
            if (Randomizer.Parse(folder) is not { } world) return;
            try
            {
                if (worldName != null) WorldSaves.SetName(ProfileFolder, world, worldName, GameEnvironment.ModsDir(S), mayCopy: !Ws.Game.Running);
                else if (WorldSaves.Find(ProfileFolder, world.Code, world.Hash) == null &&
                         !Directory.Exists(WorldSaves.LegacyFolder(ProfileFolder, world)))
                    WorldSaves.SetName(ProfileFolder, world, WorldSaves.DefaultName(ProfileFolder), GameEnvironment.ModsDir(S));
                _nameBox.Text = "";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // The saves folder can't be written: the world plays under a default name once the game makes its folder.
            }
        }

        /// <param name="worldName">The name the host typed; null: the world keeps the name it has here, or gets "World N".</param>
        private async Task Build(string code, List<string> mods, Func<string, string> done, string? worldName = null)
        {
            if (_busy) return;
            _busy = true;
            _stop = new CancellationTokenSource();
            _generate.IsEnabled = false;
            _cancel.Visibility = Visibility.Visible;
            _bar.Value = 0;
            _bar.Visibility = Visibility.Visible;
            SetHint(_status, L.T("Starting the generator…"));
            Sync();
            Refresh();
            try
            {
                var name = await Randomizer.Generate(S, code, mods, (fraction, step) =>
                {
                    _bar.Value = fraction;
                    SetHint(_status, $"{step} {(int)(fraction * 100)} %");
                }, _stop.Token);
                Randomizer.Use(S, name);
                CuratedMods.Apply(S, GameEnvironment.ModsDir(S));
                Ws.SaveSoon();
                NameBuilt(name, worldName);
                SetHint(_status, done(name));
            }
            catch (OperationCanceledException)
            {
                SetHint(_status, L.T("Stopped. No world was written."));
            }
            catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception or IOException or UnauthorizedAccessException)
            {
                SetHint(_status, e.Message);
            }
            finally
            {
                _busy = false;
                _stop.Dispose();
                _stop = null;
                _generate.IsEnabled = true;
                _cancel.Visibility = Visibility.Collapsed;
                _bar.Visibility = Visibility.Collapsed;
                Sync();
                Refresh();
            }
        }
    }
}
