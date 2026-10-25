using GhostDriveApp.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GhostDriveApp;

// Shown on Home while the data folder has no drive: `ghostdrive init` or `ghostdrive join`.
public sealed partial class FirstRunView : UserControl, IPage
{
    private AppModel? model;
    private bool joining;
    private bool busy;

    public FirstRunView()
    {
        InitializeComponent();
        NameBox.Text = Environment.MachineName;
    }

    public void Attach(AppModel m, MainWindow window) => model = m;

    public void Update()
    {
        if (model is null) return;
        DataFolderText.Text = "Data folder: " +
            DisplayText.Clean(model.DataFolder ?? "(not set)", 200) + ". Change it in Settings.";
    }

    private void OnChooseCreate(object sender, RoutedEventArgs e) => ShowForm(join: false);

    private void OnChooseJoin(object sender, RoutedEventArgs e) => ShowForm(join: true);

    private void ShowForm(bool join)
    {
        joining = join;
        Choices.Visibility = Visibility.Collapsed;
        Form.Visibility = Visibility.Visible;
        FormTitle.Text = join ? "Join with an invite code" : "Create a new drive";
        RunButton.Content = join ? "Join" : "Create drive";
        CodeBox.Visibility = Ui.Visible(join);
        CodeHelp.Visibility = Ui.Visible(join);
        Ui.Hide(Result);
        (join ? CodeBox : NameBox).Focus(FocusState.Programmatic);
    }

    private void OnBack(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        Form.Visibility = Visibility.Collapsed;
        Choices.Visibility = Visibility.Visible;
        Ui.Hide(Result);
    }

    private async void OnRun(object sender, RoutedEventArgs e)
    {
        if (model is null || busy) return;
        var name = NameBox.Text.Trim();
        if (Input.DeviceNameProblem(name) is { } nameProblem)
        {
            Ui.Show(Result, InfoBarSeverity.Warning, nameProblem);
            return;
        }
        string? code = null;
        if (joining)
        {
            code = Input.InviteCode(CodeBox.Text);
            if (code is null)
            {
                Ui.Show(Result, InfoBarSeverity.Warning,
                    "That does not look like an invite code. It starts with gd1- and is made of letters and digits.");
                return;
            }
        }

        SetBusy(true, joining ? "Contacting the inviting device..." : "Creating the drive...");
        Ui.Hide(Result);
        try
        {
            var output = joining
                ? await model.RunAsync(Commands.Join(model.Settings, code!, name), TimeSpan.FromSeconds(60))
                : await model.RunAsync(Commands.Init(model.Settings, name), TimeSpan.FromSeconds(30));
            if (!output.Succeeded)
            {
                Ui.Show(Result, InfoBarSeverity.Error,
                    DisplayText.Failure(joining ? "Could not join" : "Could not create the drive", output));
                return;
            }
            CodeBox.Text = "";
            Form.Visibility = Visibility.Collapsed;
            Choices.Visibility = Visibility.Visible;
            await model.RefreshAsync();
        }
        finally
        {
            SetBusy(false, "");
        }
    }

    private void SetBusy(bool on, string text)
    {
        busy = on;
        Busy.IsActive = on;
        BusyText.Text = text;
        RunButton.IsEnabled = !on;
        BackButton.IsEnabled = !on;
        NameBox.IsEnabled = !on;
        CodeBox.IsEnabled = !on;
    }
}
