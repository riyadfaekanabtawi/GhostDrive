using GhostDriveApp.Core;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.ApplicationModel.DataTransfer;

namespace GhostDriveApp;

// `ghostdrive invite`: the code, large and copyable, and how long it stays valid.
public sealed partial class AddDeviceView : UserControl, IPage
{
    private AppModel? model;
    private DispatcherQueueTimer? tick;
    private string? code;
    private DateTime expires;
    private bool busy;
    private bool copied;

    public AddDeviceView()
    {
        InitializeComponent();
    }

    public void Attach(AppModel m, MainWindow window)
    {
        model = m;
        tick = DispatcherQueue.CreateTimer();
        tick.Interval = TimeSpan.FromSeconds(1);
        tick.Tick += (_, _) => ShowCountdown();
    }

    public void Update()
    {
        if (model is null) return;
        CreateButton.IsEnabled = !busy && model.Running;
        if (!model.Running && model.CheckedOnce && !busy)
        {
            Ui.Show(Result, InfoBarSeverity.Informational, "Start GhostDrive from Home to create an invite.");
        }
        else if (Result.Severity == InfoBarSeverity.Informational)
        {
            Ui.Hide(Result);
        }
    }

    private async void OnCreate(object sender, RoutedEventArgs e)
    {
        if (model is null || busy) return;
        busy = true;
        CreateButton.IsEnabled = false;
        Busy.IsActive = true;
        Ui.Hide(Result);
        // Counted from before the request, so the countdown never outlasts the code.
        var started = DateTime.UtcNow;
        try
        {
            var output = await model.RunAsync(Commands.Invite(model.Settings), TimeSpan.FromSeconds(20));
            var invite = output.Succeeded ? Invite.Parse(output.Stdout) : null;
            if (invite is null)
            {
                Ui.Show(Result, InfoBarSeverity.Error, output.Succeeded
                    ? "ghostdrive invite printed no code."
                    : DisplayText.Failure("Could not create an invite", output));
                return;
            }
            code = invite.Code;
            copied = false;
            var minutes = invite.Minutes ?? Commands.InviteSeconds / 60;
            expires = started + TimeSpan.FromSeconds(Math.Min(Commands.InviteSeconds, minutes * 60));
            CodeText.Text = code;
            CodeCard.Visibility = Visibility.Visible;
            CopyButton.IsEnabled = true;
            ShowCountdown();
            tick?.Start();
        }
        finally
        {
            busy = false;
            Busy.IsActive = false;
            Update();
        }
    }

    private void ShowCountdown()
    {
        if (code is null) return;
        var left = expires - DateTime.UtcNow;
        if (left <= TimeSpan.Zero)
        {
            tick?.Stop();
            code = null;
            CodeText.Text = "This code has expired. Create a new one.";
            CountdownText.Text = "";
            CopyButton.IsEnabled = false;
            return;
        }
        CountdownText.Text = (copied ? "Copied. Valid for " : "Valid for ") + Countdown.Format(left);
    }

    // Kept out of clipboard history and cloud clipboard: the code lets anyone join.
    private void OnCopy(object sender, RoutedEventArgs e)
    {
        if (code is null) return;
        var package = new DataPackage();
        package.SetText(code);
        var options = new ClipboardContentOptions { IsAllowedInHistory = false, IsRoamable = false };
        if (!Clipboard.SetContentWithOptions(package, options)) Clipboard.SetContent(package);
        Clipboard.Flush();
        copied = true;
        ShowCountdown();
    }
}
