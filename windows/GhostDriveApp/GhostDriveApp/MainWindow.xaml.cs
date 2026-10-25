using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GhostDriveApp;

// One page per navigation item, each a UserControl that redraws from the AppModel when the
// status changes.
public interface IPage
{
    void Attach(AppModel model, MainWindow window);
    void Update();
}

public sealed partial class MainWindow : Window
{
    private readonly AppModel model;
    private string page = "home";

    public MainWindow(string startPage)
    {
        InitializeComponent();
        Title = "GhostDrive";
        AppWindow.Resize(new Windows.Graphics.SizeInt32(980, 720));
        model = new AppModel(DispatcherQueue.GetForCurrentThread());
        foreach (var p in Pages) p.Attach(model, this);
        model.Changed += Update;
        Show(startPage);
        Update();
        model.StartPolling();
    }

    private IEnumerable<IPage> Pages =>
        new IPage[] { FirstRunPage, HomePage, DevicesPage, AddPage, SettingsPage };

    public void Show(string tag)
    {
        var item = tag switch
        {
            "devices" => DevicesItem,
            "add" => AddItem,
            "settings" => SettingsItem,
            _ => HomeItem,
        };
        if (!item.IsEnabled) item = HomeItem;
        page = (string)item.Tag;
        if (!ReferenceEquals(Nav.SelectedItem, item)) Nav.SelectedItem = item;
        UpdateVisibility();
    }

    private void OnNavigationChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem { Tag: string tag } && tag != page) Show(tag);
    }

    private void Update()
    {
        var cli = model.Cli;
        CliProblem.IsOpen = cli.Path is null;
        CliProblem.Message = cli.Problem ?? "";

        // Before the first status check the app cannot tell a stopped drive from no drive.
        var hasDrive = model.HasDrive;
        DevicesItem.IsEnabled = hasDrive;
        AddItem.IsEnabled = hasDrive;
        if (!hasDrive && page is "devices" or "add") Show("home");
        UpdateVisibility();
        foreach (var p in Pages) p.Update();
    }

    private void UpdateVisibility()
    {
        var firstRun = model is not null && !model.HasDrive && model.CheckedOnce;
        FirstRunPage.Visibility = Visible(page == "home" && firstRun);
        HomePage.Visibility = Visible(page == "home" && !firstRun);
        DevicesPage.Visibility = Visible(page == "devices");
        AddPage.Visibility = Visible(page == "add");
        SettingsPage.Visibility = Visible(page == "settings");
    }

    private static Visibility Visible(bool on) => on ? Visibility.Visible : Visibility.Collapsed;
}
