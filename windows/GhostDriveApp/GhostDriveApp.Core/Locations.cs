namespace GhostDriveApp.Core;

public sealed record Located(string? Path, string? Problem);

// Where the programs and the data folder are. File checks are passed in, so the rules are
// tested without touching the disk.
public static class Locations
{
    public const string CliName = "ghostdrive.exe";
    public const string DaemonName = "ghostdrived.exe";

    // The ghostdrive.exe set in Settings, or else the one next to the app, or else the one in
    // the app folder's parent (the app in an app\ subfolder of the release zip). No PATH
    // search: a ghostdrive.exe found there could be anyone's.
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
        // AppContext.BaseDirectory ends with a separator, which GetDirectoryName would only strip.
        var dir = System.IO.Path.TrimEndingDirectorySeparator(appDirectory);
        var nextToApp = System.IO.Path.Combine(dir, CliName);
        if (fileExists(nextToApp)) return new Located(nextToApp, null);
        var parent = System.IO.Path.GetDirectoryName(dir);
        if (!string.IsNullOrEmpty(parent))
        {
            var inParent = System.IO.Path.Combine(parent, CliName);
            if (fileExists(inParent)) return new Located(inParent, null);
        }
        return new Located(null,
            $"{CliName} was not found next to the app or in the folder above it. Keep the app in the GhostDrive folder, or set the tool's location in Settings.");
    }

    // ghostdrived.exe next to the ghostdrive.exe in use, or else in a build tree's daemon
    // folder beside its cli folder: the rule `ghostdrive autostart on` follows (daemon_path).
    public static Located Daemon(string cliPath, Func<string, bool> fileExists)
    {
        var dir = System.IO.Path.GetDirectoryName(cliPath) ?? "";
        var daemon = System.IO.Path.Combine(dir, DaemonName);
        if (fileExists(daemon)) return new Located(daemon, null);
        var parent = System.IO.Path.GetDirectoryName(dir);
        if (!string.IsNullOrEmpty(parent))
        {
            var built = System.IO.Path.Combine(parent, "daemon", DaemonName);
            if (fileExists(built)) return new Located(built, null);
        }
        return new Located(null, $"{DaemonName} was not found next to {CliName}.");
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
