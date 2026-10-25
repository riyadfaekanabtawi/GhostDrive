using GhostDriveApp.Core;

namespace GhostDriveApp.Core.Tests;

public class DriveStatusTests
{
    // As daemon/src/status_json.hpp writes it.
    private const string Sample =
        "{\"device\":\"desk\",\"id\":\"AAAA-BBBB\",\"mountpoint\":\"G:\",\"port\":7373," +
        "\"rendezvous\":{\"address\":\"rv.example.org:7374\",\"connected\":true," +
        "\"observed\":\"203.0.113.5:7373\"},\"removed_by\":null,\"discovery_error\":null," +
        "\"peers\":[{\"name\":\"laptop\",\"id\":\"CCCC-DDDD\",\"address\":\"192.0.2.1:7373\"," +
        "\"online\":true,\"relayed\":false},{\"name\":\"phone\",\"id\":\"EEEE\",\"address\":null," +
        "\"online\":true,\"relayed\":true},{\"name\":\"old\",\"id\":\"FFFF\",\"address\":null," +
        "\"online\":false,\"relayed\":false}]}\n";

    [Fact]
    public void ParsesTheDaemonsOutput()
    {
        var s = DriveStatus.Parse(Sample);
        Assert.NotNull(s);
        Assert.Equal("desk", s.Device);
        Assert.Equal("AAAA-BBBB", s.Id);
        Assert.Equal("G:", s.Mountpoint);
        Assert.Equal(7373, s.Port);
        Assert.Equal(new RendezvousStatus("rv.example.org:7374", true, "203.0.113.5:7373"), s.Rendezvous);
        Assert.Null(s.RemovedBy);
        Assert.Equal(new[] { PeerConnection.Direct, PeerConnection.Relayed, PeerConnection.Offline },
            s.Peers.Select(p => p.Connection));
        Assert.Equal(new[] { "Online", "Online, relayed", "Offline" }, s.Peers.Select(p => p.ConnectionText));
        Assert.Null(s.Peers[1].Address);
    }

    [Fact]
    public void NullsAndAbsentFields()
    {
        var s = DriveStatus.Parse(
            "{\"device\":\"d\",\"id\":\"I\",\"mountpoint\":null,\"port\":1,\"rendezvous\":null," +
            "\"removed_by\":\"laptop\",\"discovery_error\":\"no route\",\"peers\":[]}");
        Assert.NotNull(s);
        Assert.Null(s.Mountpoint);
        Assert.Null(s.Rendezvous);
        Assert.Equal("laptop", s.RemovedBy);
        Assert.Equal("no route", s.DiscoveryError);
        Assert.Empty(s.Peers);
    }

    [Theory]
    [InlineData("")]
    [InlineData("ghostdrive: cannot connect")]
    [InlineData("[]")]
    [InlineData("{\"device\":\"d\",\"id\":\"I\"}")]
    [InlineData("{\"device\":1,\"id\":\"I\",\"peers\":[]}")]
    [InlineData("{\"device\":\"d\",\"id\":\"I\",\"peers\":[{\"name\":\"x\"}]}")]
    public void RejectsOtherText(string text)
    {
        Assert.Null(DriveStatus.Parse(text));
    }

    [Fact]
    public void CleansNamesFromOtherDevices()
    {
        // A right-to-left override, a newline, and an escape character in names.
        var s = DriveStatus.Parse(
            "{\"device\":\"me\\u202e\",\"id\":\"I\",\"peers\":[{\"name\":\"evil\\nname\\u001b[31m\"," +
            "\"id\":\"X\",\"online\":true,\"relayed\":false}]}");
        Assert.NotNull(s);
        Assert.Equal("me?", s.Device);
        Assert.Equal("evil?name?[31m", s.Peers[0].Name);
    }

    [Fact]
    public void SameAsComparesThePeerLists()
    {
        var a = DriveStatus.Parse(Sample)!;
        var b = DriveStatus.Parse(Sample)!;
        Assert.True(a.SameAs(b));
        Assert.False(a.SameAs(b with { Peers = b.Peers.Take(1).ToList() }));
        Assert.False(a.SameAs(b with { Device = "other" }));
        Assert.False(a.SameAs(null));
    }
}

public class DisplayTextTests
{
    [Theory]
    [InlineData("plain", "plain")]
    [InlineData("tab\there", "tab?here")]
    [InlineData("a\u200Bb", "a?b")]                  // zero-width space
    [InlineData("\u2066x\u2069", "?x?")]             // isolates
    [InlineData("\u200E\u200F\u061C", "???")]        // LRM, RLM, Arabic letter mark
    [InlineData("line\u2028sep\u2029", "line?sep?")]
    [InlineData("\uE000", "?")]                      // private use
    [InlineData("caf\u00e9 \U00010348x", "caf\u00e9 \U00010348x")]  // ordinary non-ASCII stays
    public void ReplacesInvisibleAndControlCharacters(string input, string expected)
    {
        Assert.Equal(expected, DisplayText.Clean(input));
    }

    [Fact]
    public void ReplacesUnpairedSurrogates()
    {
        Assert.Equal("a?b", DisplayText.Clean("a\uD800b"));
        Assert.Equal("?", DisplayText.Clean("\uDC00"));
    }

    [Fact]
    public void CutsLongText()
    {
        var cut = DisplayText.Clean(new string('x', 100), 10);
        Assert.Equal("xxxxxxx...", cut);
        // A letter with a combining accent counts as one character and is not split.
        var accented = string.Concat(Enumerable.Repeat("e\u0301", 20));
        Assert.Equal(string.Concat(Enumerable.Repeat("e\u0301", 7)) + "...", DisplayText.Clean(accented, 10));
    }

    [Fact]
    public void LinesDropThePrefixAndTerminalAdvice()
    {
        var lines = DisplayText.Lines(
            "ghostdrive: cannot connect to the daemon\r\nStart it with `ghostdrived`.\n\n");
        Assert.Equal(new[] { "cannot connect to the daemon", "GhostDrive is not running; start it from Home." },
            lines);
        Assert.Equal(new[] { "no device called x in this drive" },
            DisplayText.Lines("error: no device called x in this drive\n"));
        Assert.Equal(2, DisplayText.Lines("1\n2\n3\n", maxLines: 2).Count);
    }

    [Fact]
    public void FailurePrefersErrorOutput()
    {
        Assert.Equal("Could not join:" + Environment.NewLine + "bad code",
            DisplayText.Failure("Could not join", new CliOutput(1, "Contacting...\n", "ghostdrive: bad code\n")));
        Assert.Equal("out", DisplayText.Failure(null, new CliOutput(1, "out\n", "")));
        Assert.Equal("ghostdrive stopped with exit code 3.", DisplayText.Failure(null, new CliOutput(3, "", "")));
        Assert.Equal("X: ghostdrive did not finish in time.",
            DisplayText.Failure("X", new CliOutput(-1, "", "", TimedOut: true)));
    }
}

public class CliTextTests
{
    private const string InviteOutput =
        "On the other device, run:\n\n  ghostdrive join gd1-ABCDEFGH-IJKLMNOP-QRSTUV23\n" +
        "The code works once, for 10 minutes. Anyone who has it can join, so send it privately.\n";

    [Fact]
    public void ParsesInviteOutput()
    {
        Assert.Equal(new Invite("gd1-ABCDEFGH-IJKLMNOP-QRSTUV23", 10), Invite.Parse(InviteOutput));
        Assert.Equal(new Invite("gd1-AB", null), Invite.Parse("gd1-AB"));
        Assert.Null(Invite.Parse("ghostdrive join\n"));
        Assert.Null(Invite.Parse("gd1-ab01"));
        Assert.Null(Invite.Parse("gd1--AB"));
        Assert.Null(Invite.Parse(null));
    }

    [Theory]
    [InlineData("gd1-ABCD-EFGH", "gd1-ABCD-EFGH")]
    [InlineData("  gd1-abcd-efgh\n", "gd1-ABCD-EFGH")]
    [InlineData("ghostdrive join gd1-ABCD-EFGH", "gd1-ABCD-EFGH")]
    [InlineData(InviteOutput, "gd1-ABCDEFGH-IJKLMNOP-QRSTUV23")]
    [InlineData("hello", null)]
    [InlineData("gd1-AB01", null)]
    [InlineData("", null)]
    public void FindsTheInviteCodeInPastedText(string pasted, string? expected)
    {
        Assert.Equal(expected, Input.InviteCode(pasted));
    }

    [Fact]
    public void ReadsAutostartStatus()
    {
        Assert.True(AutostartText.IsSetUp(
            "login task: GhostDrive-0123abcd\nrunning:    yes\ndaemon:     C:\\gd\\ghostdrived.exe\n"));
        Assert.False(AutostartText.IsSetUp("login task: not set up\nrunning:    no\n"));
        Assert.False(AutostartText.IsSetUp("login item: not set up\n"));
        Assert.Null(AutostartText.IsSetUp("something else"));
        Assert.Null(AutostartText.IsSetUp(""));
    }

    [Fact]
    public void ReadsTheRendezvousSetting()
    {
        Assert.Null(RendezvousSetting.Parse("none\n"));
        Assert.Null(RendezvousSetting.Parse(""));
        Assert.Equal(new RendezvousSetting("rv.example.org:7374", "AAAA-BBBB"),
            RendezvousSetting.Parse("rv.example.org:7374 AAAA-BBBB\n"));
    }

    [Theory]
    [InlineData("desk", null)]
    [InlineData("  Riyad's PC  ", null)]
    [InlineData("", "Enter a name for this device.")]
    [InlineData("   ", "Enter a name for this device.")]
    [InlineData("a\u202eb", "The name contains characters that cannot be shown, such as control characters.")]
    public void ChecksDeviceNames(string name, string? problem)
    {
        Assert.Equal(problem, Input.DeviceNameProblem(name));
    }

    [Fact]
    public void LongDeviceNamesAreRefused()
    {
        Assert.Null(Input.DeviceNameProblem(new string('n', 64)));
        Assert.NotNull(Input.DeviceNameProblem(new string('n', 65)));
    }

    [Theory]
    [InlineData("rv.example.org:7374", true)]
    [InlineData("[2001:db8::1]:7374", true)]
    [InlineData("rv.example.org", false)]
    [InlineData("rv.example.org:", false)]
    [InlineData("rv.example.org:70000", false)]
    [InlineData("rv.example.org:0", false)]
    [InlineData("-x:1", false)]
    [InlineData("a b:1", false)]
    [InlineData("", false)]
    public void ChecksRendezvousAddresses(string address, bool ok)
    {
        Assert.Equal(ok, Input.RendezvousAddressProblem(address) is null);
    }

    [Fact]
    public void NormalisesFingerprints()
    {
        Assert.Equal("AAAABBBB", Input.Fingerprint(" aaaa-bbbb "));
        Assert.Equal("AAAABBBB", Input.Fingerprint("AAAA BBBB"));
        Assert.Null(Input.Fingerprint("AAAA-0000"));
        Assert.Null(Input.Fingerprint(" - "));
    }

    [Theory]
    [InlineData("", true)]
    [InlineData("G:", true)]
    [InlineData("g:", true)]
    [InlineData(@"C:\Users\me\GhostDrive", true)]
    [InlineData(@"\\server\share\gd", true)]
    [InlineData("GhostDrive", false)]
    [InlineData(@"C:relative", false)]
    [InlineData(@"\\?\C:\x", false)]
    [InlineData("1:", false)]
    public void ChecksMountLocations(string location, bool ok)
    {
        Assert.Equal(ok, Input.MountLocationProblem(location) is null);
    }

    [Fact]
    public void ExplorerOpensTheRootOfADriveLetter()
    {
        Assert.Equal(@"G:\", MountLocation.ExplorerTarget("G:"));
        Assert.Equal(@"C:\Users\me\GhostDrive", MountLocation.ExplorerTarget(@"C:\Users\me\GhostDrive"));
    }

    [Theory]
    [InlineData(600, "10:00")]
    [InlineData(599.2, "10:00")]
    [InlineData(61, "1:01")]
    [InlineData(1, "0:01")]
    [InlineData(0, "expired")]
    [InlineData(-5, "expired")]
    public void FormatsTheCountdown(double seconds, string expected)
    {
        Assert.Equal(expected, Countdown.Format(TimeSpan.FromSeconds(seconds)));
    }
}

public class LocationsTests
{
    private static Func<string, bool> Exists(params string[] files) => f => files.Contains(f);

    [Fact]
    public void FindsTheCliNextToTheApp()
    {
        var next = Path.Combine("app", "ghostdrive.exe");
        Assert.Equal(new Located(next, null), Locations.Cli(new AppSettings(), "app", Exists(next)));
        var missing = Locations.Cli(new AppSettings(), "app", Exists());
        Assert.Null(missing.Path);
        Assert.Contains("next to the app", missing.Problem);
    }

    [Fact]
    public void FindsTheCliAboveAnAppSubfolder()
    {
        // The release zip layout: ghostdrive.exe, and the app in app\.
        var zip = Path.Combine("gd", "ghostdrive.exe");
        var appDir = Path.Combine("gd", "app") + Path.DirectorySeparatorChar;  // as AppContext.BaseDirectory
        Assert.Equal(zip, Locations.Cli(new AppSettings(), appDir, Exists(zip)).Path);
        // One next to the app comes first.
        var next = Path.Combine("gd", "app", "ghostdrive.exe");
        Assert.Equal(next, Locations.Cli(new AppSettings(), appDir, Exists(zip, next)).Path);
        // Only one level up.
        Assert.Null(Locations.Cli(new AppSettings(), Path.Combine("gd", "app", "sub"), Exists(zip)).Path);
    }

    [Fact]
    public void TheSettingWins()
    {
        var settings = new AppSettings { CliPath = @"C:\tools\ghostdrive.exe" };
        var next = Path.Combine("app", "ghostdrive.exe");
        Assert.Equal(@"C:\tools\ghostdrive.exe",
            Locations.Cli(settings, "app", Exists(@"C:\tools\ghostdrive.exe", next)).Path);
        // A setting that points nowhere is reported, not silently replaced.
        var gone = Locations.Cli(settings, "app", Exists(next));
        Assert.Null(gone.Path);
        Assert.Contains("does not exist", gone.Problem);
        Assert.Contains("full path",
            Locations.Cli(new AppSettings { CliPath = "ghostdrive.exe" }, "app", Exists()).Problem);
    }

    [Fact]
    public void TheDaemonIsNextToTheCli()
    {
        var cli = Path.Combine("dir", "ghostdrive.exe");
        var daemon = Path.Combine("dir", "ghostdrived.exe");
        Assert.Equal(daemon, Locations.Daemon(cli, Exists(daemon)).Path);
        Assert.NotNull(Locations.Daemon(cli, Exists()).Problem);
        var built = Path.Combine("build", "daemon", "ghostdrived.exe");
        Assert.Equal(built, Locations.Daemon(Path.Combine("build", "cli", "ghostdrive.exe"), Exists(built)).Path);
    }

    [Fact]
    public void DefaultFoldersMatchTheCli()
    {
        string? Env(string name) => name switch
        {
            "LOCALAPPDATA" => "L",
            "USERPROFILE" => "U",
            _ => null,
        };
        Assert.Equal(Path.Combine("L", "GhostDrive"), Locations.DataFolder(new AppSettings(), Env));
        Assert.Equal(Path.Combine("U", "GhostDrive"), Locations.Mount(new AppSettings(), Env));
        Assert.Equal("D:\\x", Locations.DataFolder(new AppSettings { DataFolder = "D:\\x" }, Env));
        Assert.Equal("G:", Locations.Mount(new AppSettings { MountLocation = " G: " }, Env));
        Assert.Null(Locations.DataFolder(new AppSettings(), _ => null));
    }

    [Fact]
    public void ADriveIsItsDriveFile()
    {
        Assert.True(Locations.HasDrive("d", Exists(Path.Combine("d", "drive"))));
        Assert.False(Locations.HasDrive("d", Exists(Path.Combine("d", "device.key"))));
        Assert.False(Locations.HasDrive(null, Exists()));
    }
}
