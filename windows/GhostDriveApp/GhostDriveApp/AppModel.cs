using GhostDriveApp.Core;
using Microsoft.UI.Dispatching;

namespace GhostDriveApp;

// The app's state: settings, the last status, and every ghostdrive command, run off the UI
// thread. The app talks to the daemon only through ghostdrive.exe, so the control socket and
// the data folder rules stay in one place (the CLI), and it never reads the data folder's
// files: key material is the CLI's and the daemon's business.
public sealed class AppModel
{
    private static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(3);
    private readonly string settingsFile;
    private readonly DispatcherQueueTimer timer;
    private bool refreshing;
    private bool hadDriveFile;
    private DaemonLauncher? launcher;

    public AppModel(DispatcherQueue dispatcher)
    {
        settingsFile = AppSettings.DefaultFile(Environment.GetEnvironmentVariable);
        Settings = AppSettings.Load(settingsFile);
        timer = dispatcher.CreateTimer();
        timer.Interval = PollInterval;
        timer.Tick += async (_, _) => await RefreshAsync();
    }

    public AppSettings Settings { get; private set; }

    // The last `status --json`, or null when the daemon did not answer.
    public DriveStatus? Status { get; private set; }

    public bool Running => Status is not null;

    // Why the last status check failed, in plain words, or null.
    public string? StatusProblem { get; private set; }

    public bool CheckedOnce { get; private set; }

    public event Action? Changed;

    public Located Cli => Locations.Cli(Settings, AppContext.BaseDirectory, File.Exists);

    public string? DataFolder => Locations.DataFolder(Settings, Environment.GetEnvironmentVariable);

    public string? MountLocation => Locations.Mount(Settings, Environment.GetEnvironmentVariable);

    public bool HasDrive => Running || Locations.HasDrive(DataFolder, File.Exists);

    public void StartPolling()
    {
        timer.Start();
        _ = RefreshAsync();
    }

    public void SaveSettings(AppSettings settings)
    {
        settings.Save(settingsFile);
        Settings = settings;
        Status = null;
        Changed?.Invoke();
        _ = RefreshAsync();
    }

    // Runs ghostdrive.exe. Never throws for a missing or unstartable tool: that comes back as
    // a failed CliOutput whose error text says what is wrong.
    public async Task<CliOutput> RunAsync(IReadOnlyList<string> arguments, TimeSpan? timeout = null)
    {
        var cli = Cli;
        if (cli.Path is null) return new CliOutput(-1, "", cli.Problem ?? "ghostdrive.exe was not found.");
        try
        {
            return await Task.Run(() =>
                CliRunner.RunAsync(cli.Path, arguments, timeout ?? TimeSpan.FromSeconds(20)));
        }
        catch (CliLaunchException e)
        {
            return new CliOutput(-1, "", e.Message);
        }
    }

    public async Task RefreshAsync()
    {
        if (refreshing) return;
        refreshing = true;
        try
        {
            var output = await RunAsync(Commands.Status(Settings), TimeSpan.FromSeconds(10));
            var status = output.Succeeded ? DriveStatus.Parse(output.Stdout) : null;
            var problem = status is not null
                ? null
                : output.Succeeded
                    ? "ghostdrive status printed something unexpected."
                    : DisplayText.Failure(null, output);
            var driveFile = Locations.HasDrive(DataFolder, File.Exists);
            var changed = !CheckedOnce || problem != StatusProblem || driveFile != hadDriveFile ||
                          (status is null ? Status is not null : !status.SameAs(Status));
            hadDriveFile = driveFile;
            Status = status;
            StatusProblem = problem;
            CheckedOnce = true;
            if (changed) Changed?.Invoke();
        }
        finally
        {
            refreshing = false;
        }
    }

    // Starts ghostdrived.exe (next to ghostdrive.exe) for this session, without a console
    // window, and waits until it answers. Returns null on success, or what went wrong.
    public async Task<string?> StartDaemonAsync()
    {
        var cli = Cli;
        if (cli.Path is null) return cli.Problem;
        var daemon = Locations.Daemon(cli.Path, File.Exists);
        if (daemon.Path is null) return daemon.Problem;
        if (launcher is { HasExited: false })
        {
            await RefreshAsync();
            if (Running) return null;
        }
        try
        {
            launcher = DaemonLauncher.Start(daemon.Path, Commands.Daemon(Settings));
        }
        catch (CliLaunchException e)
        {
            return e.Message;
        }
        var result = await launcher.WaitUntilReadyAsync(async () =>
        {
            await RefreshAsync();
            return Running;
        }, TimeSpan.FromSeconds(20));
        await RefreshAsync();
        return result;
    }

    public async Task<string?> StopDaemonAsync()
    {
        var output = await RunAsync(Commands.Stop(Settings), TimeSpan.FromSeconds(40));
        // The daemon answers before it has unmounted; wait until it no longer answers.
        for (var i = 0; i < 40 && output.Succeeded; ++i)
        {
            await RefreshAsync();
            if (!Running) break;
            await Task.Delay(500);
        }
        await RefreshAsync();
        return output.Succeeded ? null : DisplayText.Failure("Could not stop GhostDrive", output);
    }
}
