using GhostDriveApp.Core;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace GhostDriveApp;

// The other devices from `status --json`, with Remove (`ghostdrive remove <ID>`).
public sealed partial class DevicesView : UserControl, IPage
{
    private AppModel? model;
    private IReadOnlyList<PeerStatus>? shown;
    private bool removing;

    public DevicesView()
    {
        InitializeComponent();
    }

    public void Attach(AppModel m, MainWindow window) => model = m;

    public void Update()
    {
        if (model is null) return;
        var s = model.Status;
        if (s is null)
        {
            Summary.Text = model.CheckedOnce
                ? "GhostDrive is not running. Start it from Home to see your devices."
                : "Checking...";
            List.Children.Clear();
            shown = null;
            return;
        }
        var online = s.Peers.Count(p => p.Online);
        Summary.Text = s.Peers.Count == 0
            ? "No other devices yet. Add one from Add device."
            : $"{s.Peers.Count} other device{(s.Peers.Count == 1 ? "" : "s")}, {online} online.";
        if (shown is not null && shown.SequenceEqual(s.Peers)) return;
        shown = s.Peers;
        List.Children.Clear();
        foreach (var peer in s.Peers) List.Children.Add(Row(peer));
    }

    private UIElement Row(PeerStatus peer)
    {
        var grid = new Grid
        {
            Padding = new Thickness(16, 12, 16, 12),
            CornerRadius = new CornerRadius(6),
            Background = Ui.Brush("CardBackgroundFillColorDefaultBrush"),
            BorderBrush = Ui.Brush("CardStrokeColorDefaultBrush"),
            BorderThickness = new Thickness(1),
            ColumnSpacing = 16,
        };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var text = new StackPanel { Spacing = 4 };
        text.Children.Add(new TextBlock
        {
            Text = peer.Name,
            FontWeight = FontWeights.SemiBold,
            TextTrimming = TextTrimming.CharacterEllipsis,
        });
        var state = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        state.Children.Add(new Ellipse
        {
            Width = 8,
            Height = 8,
            VerticalAlignment = VerticalAlignment.Center,
            Fill = peer.Connection switch
            {
                PeerConnection.Direct => Ui.StateBrush(true),
                PeerConnection.Relayed => Ui.StateBrush(false, warn: true),
                _ => Ui.Brush("SystemFillColorNeutralBrush"),
            },
        });
        state.Children.Add(new TextBlock { Text = peer.ConnectionText });
        text.Children.Add(state);
        text.Children.Add(new TextBlock
        {
            Text = "ID " + peer.Id,
            FontFamily = new FontFamily("Consolas"),
            IsTextSelectionEnabled = true,
            TextWrapping = TextWrapping.Wrap,
            Foreground = Ui.Brush("TextFillColorSecondaryBrush"),
        });
        grid.Children.Add(text);

        var remove = new Button { Content = "Remove", VerticalAlignment = VerticalAlignment.Center };
        remove.Click += async (_, _) => await RemoveAsync(peer, remove);
        Grid.SetColumn(remove, 1);
        grid.Children.Add(remove);
        return grid;
    }

    // The remove command runs only from RemoveFlow's confirmed path (GhostDriveApp.Core).
    private async Task RemoveAsync(PeerStatus peer, Button button)
    {
        if (model is null || removing) return;
        removing = true;
        button.IsEnabled = false;
        Ui.Hide(Result);
        try
        {
            var outcome = await RemoveFlow.RunAsync(peer, model.Settings,
                prompt => Ui.ConfirmAsync(XamlRoot, prompt),
                args => model.RunAsync(args, TimeSpan.FromSeconds(30)));
            if (!outcome.Ran) return;
            if (outcome.Succeeded)
            {
                Ui.Show(Result, outcome.HasHint ? InfoBarSeverity.Warning : InfoBarSeverity.Success,
                    outcome.Message, "Removed");
            }
            else
            {
                Ui.Show(Result, InfoBarSeverity.Error, outcome.Message, "Could not remove " + peer.Name);
            }
            await model.RefreshAsync();
        }
        finally
        {
            removing = false;
            button.IsEnabled = true;
        }
    }
}
