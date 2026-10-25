using System.Text.Json;
using System.Text.Json.Serialization;

namespace GhostDriveApp.Core;

// The app's own settings. Each value is empty for the default: the CLI's data folder and
// mountpoint, and ghostdrive.exe next to the app. Stored as JSON in
// %LOCALAPPDATA%\GhostDriveApp\settings.json, outside the GhostDrive data folder.
public sealed record AppSettings
{
    public string DataFolder { get; init; } = "";
    public string MountLocation { get; init; } = "";
    public string CliPath { get; init; } = "";

    // Trimmed values, or null for the default.
    [JsonIgnore] public string? DataPath => Value(DataFolder);
    [JsonIgnore] public string? MountPath => Value(MountLocation);
    [JsonIgnore] public string? CliOverride => Value(CliPath);

    private static string? Value(string? text)
    {
        var t = text?.Trim();
        return string.IsNullOrEmpty(t) ? null : t;
    }

    public static string DefaultFile(Func<string, string?> environment)
    {
        var local = environment("LOCALAPPDATA");
        if (string.IsNullOrEmpty(local))
        {
            local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        }
        return Path.Combine(local, "GhostDriveApp", "settings.json");
    }

    // A missing or unreadable file gives the defaults: settings are conveniences, and the app
    // must start without them.
    public static AppSettings Load(string file)
    {
        try
        {
            if (!File.Exists(file)) return new AppSettings();
            var parsed = JsonSerializer.Deserialize(File.ReadAllText(file), SettingsJson.Default.AppSettings);
            return parsed is null
                ? new AppSettings()
                : parsed with
                {
                    DataFolder = parsed.DataFolder ?? "",
                    MountLocation = parsed.MountLocation ?? "",
                    CliPath = parsed.CliPath ?? "",
                };
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException)
        {
            return new AppSettings();
        }
    }

    // Written to a temporary file and moved into place, so a crash cannot leave half a file.
    public void Save(string file)
    {
        var dir = Path.GetDirectoryName(file);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var temp = file + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(this, SettingsJson.Default.AppSettings));
        File.Move(temp, file, overwrite: true);
    }
}

// Source-generated serialization, so the trimmed app needs no reflection.
[JsonSerializable(typeof(AppSettings))]
[JsonSourceGenerationOptions(WriteIndented = true)]
internal sealed partial class SettingsJson : JsonSerializerContext
{
}
