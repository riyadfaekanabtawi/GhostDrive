using GhostDriveApp.Core;

namespace GhostDriveApp.Core.Tests;

public class CommandsTests
{
    private static readonly AppSettings Defaults = new();

    private static readonly AppSettings Custom = new()
    {
        DataFolder = @"  D:\gd data ",
        MountLocation = "G:",
        CliPath = @"C:\Tools\ghostdrive.exe",
    };

    [Fact]
    public void DefaultsLeaveTheDataFolderToTheCli()
    {
        Assert.Equal(new[] { "status", "--json" }, Commands.Status(Defaults));
        Assert.Equal(new[] { "stop" }, Commands.Stop(Defaults));
        Assert.Equal(new[] { "autostart", "on" }, Commands.AutostartOn(Defaults));
        Assert.Empty(Commands.Daemon(Defaults));
    }

    [Fact]
    public void EveryCommandGetsTheDataFolder()
    {
        string[] data = { "--data", @"D:\gd data" };
        Assert.Equal(new[] { "status", "--json" }.Concat(data), Commands.Status(Custom));
        Assert.Equal(new[] { "init", "--name", "My PC" }.Concat(data), Commands.Init(Custom, "My PC"));
        Assert.Equal(new[] { "join", "gd1-ABCD", "--name", "-odd" }.Concat(data),
            Commands.Join(Custom, "gd1-ABCD", "-odd"));
        Assert.Equal(new[] { "invite", "--ttl", "600" }.Concat(data), Commands.Invite(Custom));
        Assert.Equal(new[] { "remove", "AAAA-BBBB" }.Concat(data), Commands.Remove(Custom, "AAAA-BBBB"));
        Assert.Equal(new[] { "stop" }.Concat(data), Commands.Stop(Custom));
        Assert.Equal(new[] { "rendezvous" }.Concat(data), Commands.RendezvousShow(Custom));
        Assert.Equal(new[] { "rendezvous", "rv.example.org:7374", "ABCD" }.Concat(data),
            Commands.RendezvousSet(Custom, "rv.example.org:7374", "ABCD"));
        Assert.Equal(new[] { "autostart", "off" }.Concat(data), Commands.AutostartOff(Custom));
        Assert.Equal(new[] { "autostart", "status" }.Concat(data), Commands.AutostartStatus(Custom));
    }

    [Fact]
    public void StartingPassesTheMountLocation()
    {
        Assert.Equal(new[] { "autostart", "on", "--data", @"D:\gd data", "--mount", "G:" },
            Commands.AutostartOn(Custom));
        Assert.Equal(new[] { "--data", @"D:\gd data", "--mount", "G:" }, Commands.Daemon(Custom));
    }

    [Fact]
    public void PositionalValuesThatLookLikeOptionsAreRefused()
    {
        Assert.Throws<ArgumentException>(() => Commands.Remove(Defaults, "--data"));
        Assert.Throws<ArgumentException>(() => Commands.Join(Defaults, "", "x"));
        Assert.Throws<ArgumentException>(() => Commands.RendezvousSet(Defaults, "-h", "ABCD"));
    }
}

public class SettingsTests : IDisposable
{
    private readonly string dir = Path.Combine(Path.GetTempPath(), "gdapp-tests-" + Guid.NewGuid());

    public void Dispose()
    {
        if (Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
    }

    [Fact]
    public void RoundTrips()
    {
        var file = Path.Combine(dir, "sub", "settings.json");
        var s = new AppSettings { DataFolder = "D:\\d", MountLocation = "G:", CliPath = "" };
        s.Save(file);
        Assert.Equal(s, AppSettings.Load(file));
        Assert.False(File.Exists(file + ".tmp"));
    }

    [Fact]
    public void MissingOrBrokenFilesGiveDefaults()
    {
        Assert.Equal(new AppSettings(), AppSettings.Load(Path.Combine(dir, "none.json")));
        Directory.CreateDirectory(dir);
        var file = Path.Combine(dir, "bad.json");
        File.WriteAllText(file, "{ not json");
        Assert.Equal(new AppSettings(), AppSettings.Load(file));
        File.WriteAllText(file, "{\"DataFolder\": null}");
        Assert.Equal(new AppSettings(), AppSettings.Load(file));
    }

    [Fact]
    public void BlankValuesMeanTheDefault()
    {
        var s = new AppSettings { DataFolder = "   ", MountLocation = "\t", CliPath = " C:\\x.exe " };
        Assert.Null(s.DataPath);
        Assert.Null(s.MountPath);
        Assert.Equal("C:\\x.exe", s.CliOverride);
    }

    [Fact]
    public void TheFileLivesUnderLocalAppData()
    {
        Assert.Equal(Path.Combine("L", "GhostDriveApp", "settings.json"),
            AppSettings.DefaultFile(name => name == "LOCALAPPDATA" ? "L" : null));
    }
}
