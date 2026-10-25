using GhostDriveApp.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GhostDriveApp;

// Start and stop, start at login (`autostart on/off`), the rendezvous server
// (`ghostdrive rendezvous`), and the app's folder settings.
public sealed partial class SettingsView : UserControl, IPage
{
    private AppModel? model;
    private bool busy;
    private bool settingSwitch;
    private bool loaded;
    private bool? lastRunning;

    public SettingsView()
    {
        InitializeComponent();
    }

    public void Attach(AppModel m, MainWindow window)
    {
        model = m;
        LoadBoxes();
    }

    private void LoadBoxes()
    {
        if (model is null) return;
        var s = model.Settings;
        DataFolderBox.Text = s.DataFolder;
        MountBox.Text = s.MountLocation;
        CliBox.Text = s.CliPath;
        var env = new Func<string, string?>(Environment.GetEnvironmentVariable);
        DataFolderBox.PlaceholderText = Locations.DataFolder(new AppSettings(), env) ?? "";
        MountBox.PlaceholderText = Locations.Mount(new AppSettings(), env) ?? "";
        CliBox.PlaceholderText = Path.Combine(AppContext.BaseDirectory, Locations.CliName);
    }

    public void Update()
    {
        if (model is null) return;
        var running = model.Running;
        StartButton.IsEnabled = !busy && model.CheckedOnce && !running && model.HasDrive;
        StopButton.IsEnabled = !busy && running;
        LoginSwitch.IsEnabled = !busy && model.HasDrive;
        RendezvousButton.IsEnabled = model.HasDrive;
        RunState.Text = !model.CheckedOnce ? "Checking..." : running ? "Running" : "Not running";
        // Read the settings that live in the drive when the page first has a drive, and again
        // whenever the daemon starts or stops (autostart and the daemon change together).
        if (model.HasDrive && (!loaded || lastRunning != running))
        {
            loaded = true;
            lastRunning = running;
            _ = LoadDriveSettingsAsync();
        }
    }

    private async Task LoadDriveSettingsAsync()
    {
        if (model is null) return;
        var rv = await model.RunAsync(Commands.RendezvousShow(model.Settings));
        RendezvousCurrent.Text = rv.Succeeded
            ? RendezvousSetting.Parse(rv.Stdout) is { } setting
                ? $"Current server: {setting.Address} ({setting.Fingerprint})"
                : "No server set."
            : DisplayText.Failure("Could not read the rendezvous setting", rv);

        var auto = await model.RunAsync(Commands.AutostartStatus(model.Settings));
        if (auto.Succeeded && AutostartText.IsSetUp(auto.Stdout) is { } on)
        {
            settingSwitch = true;
            LoginSwitch.IsOn = on;
            settingSwitch = false;
        }
    }

    private async Task RunAsync(Func<Task<string?>> action)
    {
        if (busy) return;
        busy = true;
        RunBusy.IsActive = true;
        Ui.Hide(RunResult);
        Update();
        try
        {
            if (await action() is { } problem) Ui.Show(RunResult, InfoBarSeverity.Error, problem);
        }
        finally
        {
            busy = false;
            RunBusy.IsActive = false;
            Update();
        }
    }

    private async void OnStart(object sender, RoutedEventArgs e) =>
        await RunAsync(() => model!.StartDaemonAsync());

    private async void OnStop(object sender, RoutedEventArgs e) =>
        await RunAsync(() => model!.StopDaemonAsync());

    private async void OnLoginToggled(object sender, RoutedEventArgs e)
    {
        if (settingSwitch || model is null) return;
        var on = LoginSwitch.IsOn;
        await RunAsync(async () =>
        {
            var output = await model.RunAsync(
                on ? Commands.AutostartOn(model.Settings) : Commands.AutostartOff(model.Settings),
                TimeSpan.FromSeconds(60));
            await model.RefreshAsync();
            if (output.Succeeded)
            {
                Ui.Show(RunResult, InfoBarSeverity.Success,
                    string.Join(Environment.NewLine, DisplayText.Lines(output.Stdout, maxLines: 4, maxLength: 200)));
                return null;
            }
            settingSwitch = true;
            LoginSwitch.IsOn = !on;
            settingSwitch = false;
            return DisplayText.Failure(on ? "Could not set up start at login" : "Could not turn off start at login",
                output);
        });
    }

    private async void OnSetRendezvous(object sender, RoutedEventArgs e)
    {
        if (model is null) return;
        var address = RendezvousAddress.Text.Trim();
        if (Input.RendezvousAddressProblem(address) is { } problem)
        {
            Ui.Show(RendezvousResult, InfoBarSeverity.Warning, problem);
            return;
        }
        if (Input.Fingerprint(RendezvousFingerprint.Text) is not { } fingerprint)
        {
            Ui.Show(RendezvousResult, InfoBarSeverity.Warning,
                "Enter the fingerprint ghostdrive-rendezvous printed when it started (letters A-Z and digits 2-7).");
            return;
        }
        RendezvousButton.IsEnabled = false;
        RendezvousBusy.IsActive = true;
        Ui.Hide(RendezvousResult);
        try
        {
            var output = await model.RunAsync(Commands.RendezvousSet(model.Settings, address, fingerprint));
            if (output.Succeeded)
            {
                Ui.Show(RendezvousResult, InfoBarSeverity.Success,
                    string.Join(Environment.NewLine, DisplayText.Lines(output.Stdout, maxLines: 4, maxLength: 200)));
                RendezvousAddress.Text = "";
                RendezvousFingerprint.Text = "";
                await LoadDriveSettingsAsync();
            }
            else
            {
                Ui.Show(RendezvousResult, InfoBarSeverity.Error, DisplayText.Failure("Could not set the server", output));
            }
        }
        finally
        {
            RendezvousButton.IsEnabled = model.HasDrive;
            RendezvousBusy.IsActive = false;
        }
    }

    private void OnSave(object sender, RoutedEventArgs e)
    {
        if (model is null) return;
        var problem = Input.DataFolderProblem(DataFolderBox.Text) ?? Input.MountLocationProblem(MountBox.Text);
        var cli = CliBox.Text.Trim();
        if (problem is null && cli.Length > 0 && !MountLocation.IsFullPath(cli))
        {
            problem = "Give the full path of ghostdrive.exe, or leave it empty.";
        }
        if (problem is not null)
        {
            Ui.Show(SaveResult, InfoBarSeverity.Warning, problem);
            return;
        }
        var settings = new AppSettings
        {
            DataFolder = DataFolderBox.Text.Trim(),
            MountLocation = MountBox.Text.Trim(),
            CliPath = cli,
        };
        try
        {
            model.SaveSettings(settings);
            loaded = false;
            Ui.Show(SaveResult, InfoBarSeverity.Success, "Saved.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Ui.Show(SaveResult, InfoBarSeverity.Error, "Could not save the settings: " + ex.Message);
        }
    }
}
