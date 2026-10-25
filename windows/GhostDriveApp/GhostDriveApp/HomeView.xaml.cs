using GhostDriveApp.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GhostDriveApp;

// This device, whether GhostDrive runs, where the drive is mounted, and the rendezvous state.
public sealed partial class HomeView : UserControl, IPage
{
    private AppModel? model;
    private bool busy;

    public HomeView()
    {
        InitializeComponent();
    }

    public void Attach(AppModel m, MainWindow window) => model = m;

    public void Update()
    {
        if (model is null) return;
        var s = model.Status;

        DeviceName.Text = s is null ? "This PC" : s.Device;
        DeviceId.Text = s is null ? "" : "Device ID: " + s.Id;

        RunningDot.Fill = Ui.StateBrush(s is not null, warn: !model.CheckedOnce);
        RunningText.Text = !model.CheckedOnce ? "Checking..." : s is not null ? "Running" : "Not running";
        RunningDetail.Text = s is not null || !model.CheckedOnce
            ? ""
            : model.Cli.Path is null
                ? model.Cli.Problem ?? ""
                : "Start GhostDrive to mount the drive and reach your other devices.";
        RunningDetail.Visibility = Ui.Visible(RunningDetail.Text.Length > 0);
        StartButton.Visibility = Ui.Visible(model.CheckedOnce && s is null);
        StartRow.Visibility = Ui.Visible(busy || StartButton.Visibility == Visibility.Visible);
        StartButton.IsEnabled = !busy && model.Cli.Path is not null;

        var mounted = s?.Mountpoint;
        if (mounted is not null)
        {
            MountText.Text = "Mounted at " + DisplayText.Clean(mounted, 200);
        }
        else if (s is not null)
        {
            MountText.Text = "Not mounted.";
        }
        else
        {
            MountText.Text = "Mounts at " + DisplayText.Clean(model.MountLocation ?? "the default location", 200) +
                " when GhostDrive runs.";
        }
        OpenButton.IsEnabled = mounted is not null && Directory.Exists(MountLocation.ExplorerTarget(mounted));

        UpdateRendezvous(s);

        Ui.Show(RemovedBar, InfoBarSeverity.Error, s?.RemovedBy is { } by
            ? $"{by} removed this device from the drive. It can no longer reach the other devices."
            : null, s?.RemovedBy is null ? "" : "Removed from the drive");
        Ui.Show(DiscoveryBar, InfoBarSeverity.Warning, s?.DiscoveryError,
            s?.DiscoveryError is null ? "" : "Finding devices");
    }

    private void UpdateRendezvous(DriveStatus? s)
    {
        var rv = s?.Rendezvous;
        if (s is null)
        {
            RendezvousDot.Fill = Ui.Brush("SystemFillColorNeutralBrush");
            RendezvousText.Text = "Unknown while GhostDrive is not running.";
            RendezvousDetail.Text = "";
        }
        else if (rv is null)
        {
            RendezvousDot.Fill = Ui.Brush("SystemFillColorNeutralBrush");
            RendezvousText.Text = "No rendezvous server set.";
            RendezvousDetail.Text =
                "Devices are reached at the addresses they were last seen at. Set a server in Settings to reach devices on other networks.";
        }
        else if (rv.Connected)
        {
            RendezvousDot.Fill = Ui.StateBrush(true);
            RendezvousText.Text = "Connected to " + rv.Address;
            RendezvousDetail.Text = rv.Observed is null ? "" : "The server sees this PC at " + rv.Observed + ".";
        }
        else
        {
            RendezvousDot.Fill = Ui.StateBrush(false, warn: true);
            RendezvousText.Text = "Not connected to " + rv.Address;
            RendezvousDetail.Text = "GhostDrive keeps trying. Check the address and that the server is running.";
        }
        RendezvousDetail.Visibility = Ui.Visible(RendezvousDetail.Text.Length > 0);
    }

    private async void OnStart(object sender, RoutedEventArgs e)
    {
        if (model is null || busy) return;
        busy = true;
        Busy.IsActive = true;
        StartButton.IsEnabled = false;
        Ui.Hide(ActionResult);
        try
        {
            var problem = await model.StartDaemonAsync();
            if (problem is not null) Ui.Show(ActionResult, InfoBarSeverity.Error, problem);
        }
        finally
        {
            busy = false;
            Busy.IsActive = false;
            Update();
        }
    }

    private void OnOpen(object sender, RoutedEventArgs e)
    {
        if (model?.Status?.Mountpoint is not { } mountpoint) return;
        try
        {
            Ui.OpenInExplorer(MountLocation.ExplorerTarget(mountpoint));
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            Ui.Show(ActionResult, InfoBarSeverity.Error, "Could not open Explorer: " + ex.Message);
        }
    }
}
