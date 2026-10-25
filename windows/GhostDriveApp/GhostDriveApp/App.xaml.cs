using Microsoft.UI.Xaml;

namespace GhostDriveApp;

public partial class App : Application
{
    private MainWindow? window;

    public App()
    {
        InitializeComponent();
    }

    // `GhostDriveApp.exe --page devices` opens on a page other than Home (home, devices, add,
    // settings); CI uses it for screenshots.
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        var argv = Environment.GetCommandLineArgs();
        var page = "home";
        for (var i = 1; i + 1 < argv.Length; ++i)
        {
            if (argv[i] == "--page") page = argv[i + 1];
        }
        window = new MainWindow(page);
        window.Activate();
    }
}
