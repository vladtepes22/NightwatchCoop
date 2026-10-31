using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views;

// Row view-models of the save editor's virtualized lists. They own their edit state, so filtering and
// scrolling never lose a change; the window only asks them what changed.
internal sealed class RowCommand(Action run, Func<bool>? can = null) : ICommand
{
    public event EventHandler? CanExecuteChanged;
    public bool CanExecute(object? parameter) => can?.Invoke() ?? true;
    public void Execute(object? parameter) => run();
    public void Raise() => CanExecuteChanged?.Invoke(this, EventArgs.Empty);
}

internal abstract class RowBase : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    protected void Raise(params string[] names)
    {
        foreach (var name in names) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}

internal sealed class ItemRow : RowBase
{
    public const int MaxGemEffects = 3;
    private readonly Action _changed;
    private string _count;
    private int _level;
    private bool _removed;
    private SaveEditor.GemEffect[] _effects;
    private ImageSource? _icon;
    private bool _iconAsked;
    private string? _previewName;
    private bool? _previewCursed;
    public SaveEditor.Item Item { get; }
    public RowCommand LevelDown { get; }
    public RowCommand LevelUp { get; }
    public RowCommand RemoveCommand { get; }
    public RowCommand EditGemCommand { get; }

    public ItemRow(SaveEditor.Item item, Action changed, Action<ItemRow>? editGem = null)
    {
        Item = item; _changed = changed;
        _count = item.Count.ToString(CultureInfo.InvariantCulture); _level = item.Level ?? 0;
        _effects = item.Gem?.Effects ?? [];
        LevelDown = new RowCommand(() => Level--, () => CanEditLevel && !Removed && Level > 0);
        LevelUp = new RowCommand(() => Level++, () => CanEditLevel && !Removed && Level < Item.MaxLevel);
        RemoveCommand = new RowCommand(() => Removed = !Removed);
        EditGemCommand = new RowCommand(() => editGem?.Invoke(this), () => CanEditGem && !Removed);
    }

    public string Title => _previewName ?? Item.Name ?? L.T("Unnamed item");
    public string Meta => Item.Gem is { } gem ? GemMeta(gem with { Cursed = _previewCursed ?? gem.Cursed })
        : (Item.Section == 0 ? "" : L.T("Key item") + " · ") + L.T("Item ID") + " " + Item.Number;
    public bool IsKey => Item.Section is 1 or 2;
    public bool Locked => Item.Locked;
    // Decoded off the UI thread when a row first shows; the row fills in a moment later.
    public ImageSource? Icon
    {
        get
        {
            if (!_iconAsked) { _iconAsked = true; FetchIcon(); }
            return _icon;
        }
    }
    private async void FetchIcon()
    {
        var path = Item.Icon;
        _icon = await Task.Run(() => LoadIcon(path));
        if (_icon != null) Raise(nameof(Icon));
    }
    // Gem effects, or a weapon's slots, under the title.
    public string Details => Item.Gem != null ? string.Join("\n", _effects.Select(e => e.Text ?? L.T("Hidden effect")))
        : Item.Sockets is { Length: > 0 } sockets ? string.Join("  ·  ", sockets.Select(s => s.Gem ?? string.Format(L.T("{0} slot, empty"), ShapeName(s.Shape))))
        : "";
    public Visibility DetailsVisibility => Details.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    public Visibility CountVisibility => Locked ? Visibility.Collapsed : Visibility.Visible;
    // A gem is one instance with its own effects: it is removed, never stacked.
    public Visibility CountBoxVisibility => Locked || Item.Gem != null ? Visibility.Collapsed : Visibility.Visible;
    // Runes carry hidden effects that make the rune, so only blood gems are edited.
    public bool CanEditGem => Item.Gem is { Type: 1 };
    public Visibility GemEditVisibility => CanEditGem ? Visibility.Visible : Visibility.Collapsed;
    public string EditGemText => L.T("Edit effects");
    public IReadOnlyList<SaveEditor.GemEffect> Effects
    {
        get => _effects;
        set
        {
            _effects = value.ToArray();
            Raise(nameof(Effects), nameof(Details), nameof(DetailsVisibility), nameof(Changed)); _changed();
        }
    }
    public bool GemChanged => !_removed && Item.Gem != null && !_effects.Select(e => e.Id).SequenceEqual(Item.Gem.Effects.Select(e => e.Id));

    private static string GemMeta(SaveEditor.Gem gem)
    {
        var parts = new List<string> { gem.Sort switch { 1 => L.T("Caryll rune"), 2 => L.T("Oath rune"), _ => gem.Type == 2 ? L.T("Rune") : L.T("Blood gem") } };
        if (gem.Type == 1) parts.Add(ShapeName(gem.Shape));
        if (gem.Cursed) parts.Add(L.T("Cursed"));
        if (gem.SocketedIn is { } holder)
            parts.Add(string.Format(L.T("In {0}"), holder.Number == 1000 ? L.T("Memory Altar") : holder.Name ?? L.T("a weapon")));
        return string.Join(" · ", parts);
    }
    public static string ShapeName(int shape) => shape switch
    {
        1 => L.T("Radial"), 2 => L.T("Triangle"), 4 => L.T("Waning"), 8 => L.T("Circle"), 63 => L.T("Droplet"), _ => L.T("Unknown shape")
    };
    /// <summary>Name and icon after an unsaved gem edit, the change marker stays until Apply or Revert.</summary>
    public void ShowPreview(SaveEditor.GemPreview preview)
    {
        _previewName = preview.Name;
        _previewCursed = preview.Cursed;
        _iconAsked = true;
        _icon = LoadIcon(preview.Icon);
        Raise(nameof(Title), nameof(Meta), nameof(Icon));
    }
    internal static ImageSource? LoadIcon(string? path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) return null;
        try
        {
            // Our own stream, so the file closes here even when decoding fails (a UriSource stays open until collected).
            using var file = File.OpenRead(path);
            var image = new BitmapImage();
            image.BeginInit(); image.CacheOption = BitmapCacheOption.OnLoad; image.DecodePixelWidth = 44; image.StreamSource = file; image.EndInit();
            image.Freeze(); return image;
        }
        // Whatever is wrong with the file (half written, damaged, an imaging error) means no icon, never a crash: this
        // runs on a pool thread behind an async void, where an escaping exception ends the launcher.
        catch (Exception) { return null; }
    }
    public string Group => IsKey ? L.T("Key items") : Item.Gem?.Type == 2 ? L.T("Runes") : KindLabel(Item.Kind);
    public string Kind => Item.Kind;
    public int Order => IsKey ? 5 : Item.Kind switch { "goods" => 0, "weapon" => 1, "armor" => 2, "accessory" => 3, "gem" => 4, _ => 6 };
    public bool HasLevel => Item.Level.HasValue;
    public bool CanEditLevel => Item.Level.HasValue && Item.MaxLevel is > 0;
    public Visibility LevelVisibility => HasLevel ? Visibility.Visible : Visibility.Collapsed;
    public string LevelText => "+" + _level;
    public bool CountEnabled => !_removed;
    public string RemoveText => _removed ? L.T("Undo") : L.T("Remove");

    public string Count
    {
        get => _count;
        set { _count = value; Raise(nameof(Count), nameof(CountValid), nameof(Changed)); _changed(); }
    }
    public int Level
    {
        get => _level;
        set
        {
            if (value == _level) return;
            _level = value; Raise(nameof(Level), nameof(LevelText), nameof(Changed)); RaiseLevelCommands(); _changed();
        }
    }
    public bool Removed
    {
        get => _removed;
        set
        {
            _removed = value;
            Raise(nameof(Removed), nameof(CountEnabled), nameof(RemoveText), nameof(Changed)); RaiseLevelCommands(); _changed();
        }
    }
    private void RaiseLevelCommands() { LevelDown.Raise(); LevelUp.Raise(); EditGemCommand.Raise(); }

    public bool CountValid => _removed || (int.TryParse(_count, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n >= 1);
    public int CountNumber => int.Parse(_count, CultureInfo.InvariantCulture);
    public bool CountChanged => !_removed && int.TryParse(_count, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) && n != Item.Count;
    public bool LevelChanged => !_removed && Item.Level.HasValue && _level != Item.Level;
    public bool Changed => _removed || CountChanged || LevelChanged || GemChanged || !CountValid;

    public IEnumerable<string> Describe()
    {
        if (_removed) { yield return L.T("Remove") + ": " + Title; yield break; }
        if (CountChanged) yield return Title + ": " + Item.Count.ToString("N0") + " → " + CountNumber.ToString("N0");
        if (LevelChanged) yield return Title + ": +" + Item.Level + " → +" + _level;
        if (GemChanged) yield return Title + ": " + string.Join(", ", _effects.Select(e => e.Text ?? e.Id.ToString(CultureInfo.InvariantCulture)));
    }

    public static string KindLabel(string kind) => kind switch
    {
        "goods" => L.T("Goods"), "weapon" => L.T("Weapons"), "armor" => L.T("Armor"),
        "accessory" => L.T("Accessories"), "gem" => L.T("Gems"), _ => L.T("Items")
    };
}

internal sealed class FlagRow : RowBase
{
    private readonly Action _changed;
    private bool _on;
    public SaveEditor.Flag Flag { get; }
    public FlagRow(SaveEditor.Flag flag, Action changed) { Flag = flag; _on = flag.On; _changed = changed; }
    // Ending labels are the editor's own fixed strings; boss and lamp names come from the game's text.
    public string Label => Flag.Kind == "ending" ? L.T(Flag.Label) : Flag.Label;
    public string Group => Flag.Kind == "ending" ? L.T(Flag.Group) : Flag.Group;
    public bool On
    {
        get => _on;
        set { if (_on == value) return; _on = value; Raise(nameof(On), nameof(Changed)); _changed(); }
    }
    public bool Changed => _on != Flag.On;
}

internal static class SaveEditorXaml
{
    private const string Header = "xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" " +
        "xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\" xmlns:ui=\"http://schemas.lepo.co/wpfui/2022/xaml\"";

    private static T Parse<T>(string xaml) => (T)XamlReader.Parse(xaml);

    public static DataTemplate ItemTemplate() => Parse<DataTemplate>($$"""
        <DataTemplate {{Header}}>
          <Border x:Name="Bar" Padding="10,6,22,6" BorderThickness="3,0,0,0" BorderBrush="Transparent">
            <Grid>
              <Grid.ColumnDefinitions>
                <ColumnDefinition Width="52"/><ColumnDefinition Width="*"/><ColumnDefinition Width="Auto"/><ColumnDefinition Width="116"/><ColumnDefinition Width="92"/>
              </Grid.ColumnDefinitions>
              <Border Width="44" Height="44" CornerRadius="6" VerticalAlignment="Center" HorizontalAlignment="Left" Background="#18000000">
                <Image x:Name="Icon" Source="{Binding Icon}" Width="44" Height="44" RenderOptions.BitmapScalingMode="HighQuality"/>
              </Border>
              <StackPanel Grid.Column="1" VerticalAlignment="Center" Margin="0,0,12,0">
                <TextBlock x:Name="Title" Text="{Binding Title}" ToolTip="{Binding Title}" TextTrimming="CharacterEllipsis" Style="{DynamicResource RowTitle}"/>
                <TextBlock Text="{Binding Meta}" TextTrimming="CharacterEllipsis" Style="{DynamicResource StatLabel}"/>
                <TextBlock Text="{Binding Details}" Visibility="{Binding DetailsVisibility}" TextWrapping="Wrap" Margin="0,3,0,0" Style="{DynamicResource Muted}"/>
              </StackPanel>
              <StackPanel Grid.Column="2" Orientation="Horizontal" VerticalAlignment="Center" Margin="0,0,14,0" Visibility="{Binding LevelVisibility}">
                <ui:Button Content="−" Command="{Binding LevelDown}" Width="28" Height="28" Padding="0" AutomationProperties.Name="Decrease weapon level"/>
                <TextBlock Text="{Binding LevelText}" Width="42" TextAlignment="Center" VerticalAlignment="Center" Style="{DynamicResource RowTitle}"/>
                <ui:Button Content="+" Command="{Binding LevelUp}" Width="28" Height="28" Padding="0" AutomationProperties.Name="Increase weapon level"/>
              </StackPanel>
              <ui:Button x:Name="EditGem" Grid.Column="2" Content="{Binding EditGemText}" Command="{Binding EditGemCommand}" Visibility="{Binding GemEditVisibility}"
                         VerticalAlignment="Center" Margin="0,0,14,0" Padding="10,5" AutomationProperties.Name="Edit effects"/>
              <ui:TextBox x:Name="CountBox" Grid.Column="3" Text="{Binding Count, UpdateSourceTrigger=PropertyChanged}" IsEnabled="{Binding CountEnabled}"
                          Visibility="{Binding CountBoxVisibility}" Padding="8,5" VerticalAlignment="Center" AutomationProperties.Name="Count"/>
              <ui:Button Grid.Column="4" Content="{Binding RemoveText}" Command="{Binding RemoveCommand}" HorizontalAlignment="Right" VerticalAlignment="Center"
                         Visibility="{Binding CountVisibility}" Padding="10,5" Appearance="Transparent"/>
            </Grid>
          </Border>
          <DataTemplate.Triggers>
            <DataTrigger Binding="{Binding Changed}" Value="True">
              <Setter TargetName="Bar" Property="BorderBrush" Value="{DynamicResource AccentTextFillColorPrimaryBrush}"/>
            </DataTrigger>
            <DataTrigger Binding="{Binding CountValid}" Value="False">
              <Setter TargetName="CountBox" Property="BorderBrush" Value="{DynamicResource DangerTextBrush}"/>
            </DataTrigger>
            <DataTrigger Binding="{Binding Removed}" Value="True">
              <Setter TargetName="Title" Property="TextDecorations" Value="Strikethrough"/>
              <Setter TargetName="Title" Property="Opacity" Value="0.55"/>
            </DataTrigger>
          </DataTemplate.Triggers>
        </DataTemplate>
        """);

    public static DataTemplate FlagTemplate() => Parse<DataTemplate>($$"""
        <DataTemplate {{Header}}>
          <Border x:Name="Bar" Padding="12,5,22,5" BorderThickness="3,0,0,0" BorderBrush="Transparent">
            <Grid>
              <TextBlock Text="{Binding Label}" TextTrimming="CharacterEllipsis" VerticalAlignment="Center" Margin="0,0,70,0" Style="{DynamicResource RowTitle}"/>
              <ui:ToggleSwitch IsChecked="{Binding On, Mode=TwoWay}" HorizontalAlignment="Right" VerticalAlignment="Center"
                               AutomationProperties.Name="{Binding Label}"/>
            </Grid>
          </Border>
          <DataTemplate.Triggers>
            <DataTrigger Binding="{Binding Changed}" Value="True">
              <Setter TargetName="Bar" Property="BorderBrush" Value="{DynamicResource AccentTextFillColorPrimaryBrush}"/>
            </DataTrigger>
          </DataTemplate.Triggers>
        </DataTemplate>
        """);

    public static DataTemplate GroupHeaderTemplate() => Parse<DataTemplate>($$"""
        <DataTemplate {{Header}}>
          <TextBlock Text="{Binding Name}" Margin="12,16,0,6" Style="{DynamicResource GroupHeader}"/>
        </DataTemplate>
        """);

    // Plain rows with a divider: no selection chrome, because the controls inside are what the user touches.
    // gutter: room kept free on the right for the scroll bar that floats over that edge.
    public static Style RowStyle(double gutter) => Parse<Style>($$"""
        <Style {{Header}} TargetType="ListBoxItem">
          <Setter Property="Focusable" Value="False"/>
          <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
          <Setter Property="Template">
            <Setter.Value>
              <ControlTemplate TargetType="ListBoxItem">
                <Border x:Name="Chrome" Margin="0,0,{{gutter.ToString(System.Globalization.CultureInfo.InvariantCulture)}},0" BorderThickness="0,0,0,1" BorderBrush="{DynamicResource ControlStrokeColorDefaultBrush}" Background="Transparent">
                  <ContentPresenter/>
                </Border>
                <ControlTemplate.Triggers>
                  <Trigger Property="IsMouseOver" Value="True">
                    <Setter TargetName="Chrome" Property="Background" Value="{DynamicResource SidebarItemHoverBrush}"/>
                  </Trigger>
                </ControlTemplate.Triggers>
              </ControlTemplate>
            </Setter.Value>
          </Setter>
        </Style>
        """);
}
