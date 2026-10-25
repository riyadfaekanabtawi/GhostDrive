namespace GhostDriveApp.Core;

public sealed record Located(string? Path, string? Problem);

// Where the programs and the data folder are. File checks are passed in, so the rules are
// tested without touching the disk.
public static class Locations
{
    public const string CliName = "ghostdrive.exe";
    public const string DaemonName = "ghostdrived.exe";

    // The ghostdrive.exe set in Settings, or else the one next to the app (the release zip
    // layout). No PATH search: a ghostdrive.exe found there could be anyone's.
    public static Located Cli(AppSettings settings, string appDirectory, Func<string, bool> fileExists)
    {
        if (settings.CliOverride is { } configured)
        {
            if (!MountLocation.IsFullPath(configured) && !System.IO.Path.IsPathFullyQualified(configured))
            {
                return new Located(null, "The ghostdrive tool set in Settings must be a full path.");
            }
            return fileExists(configured)
                ? new Located(configured, null)
                : new Located(null, $"The ghostdrive tool set in Settings does not exist: {DisplayText.Clean(configured, 160)}");
        }
        var nextToApp = System.IO.Path.Combine(appDirectory, CliName);
        return fileExists(nextToApp)
            ? new Located(nextToApp, null)
            : new Located(null,
                $"{CliName} was not found next to the app. Keep the app in the GhostDrive folder, or set the tool's location in Settings.");
    }

    // ghostdrived.exe next to the ghostdrive.exe in use, as `ghostdrive autostart on` expects.
    public static Located Daemon(string cliPath, Func<string, bool> fileExists)
    {
        var dir = System.IO.Path.GetDirectoryName(cliPath) ?? "";
        var daemon = System.IO.Path.Combine(dir, DaemonName);
        return fileExists(daemon)
            ? new Located(daemon, null)
            : new Located(null, $"{DaemonName} was not found next to {CliName}.");
    }

    // The CLI's default (daemon/src/drive.cpp, default_data_dir): %LOCALAPPDATA%\GhostDrive.
    public static string? DataFolder(AppSettings settings, Func<string, string?> environment)
    {
        if (settings.DataPath is { } data) return data;
        var local = environment("LOCALAPPDATA");
        return string.IsNullOrEmpty(local) ? null : System.IO.Path.Combine(local, "GhostDrive");
    }

    // The CLI's default mountpoint: %USERPROFILE%\GhostDrive.
    public static string? Mount(AppSettings settings, Func<string, string?> environment)
    {
        if (settings.MountPath is { } mount) return mount;
        var home = environment("USERPROFILE");
        return string.IsNullOrEmpty(home) ? null : System.IO.Path.Combine(home, "GhostDrive");
    }

    // A drive exists once `init` or `join` wrote the folder's "drive" file (Drive::exists).
    // Only its existence is checked; the app never opens files in the data folder.
    public static bool HasDrive(string? dataFolder, Func<string, bool> fileExists) =>
        dataFolder is not null && fileExists(System.IO.Path.Combine(dataFolder, "drive"));
}
