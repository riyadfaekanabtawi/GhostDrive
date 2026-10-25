using GhostDriveApp.Core;

namespace GhostDriveApp.Core.Tests;

// Runs real programs: cmd.exe and ping on Windows, /bin/sh on Linux and macOS.
public class CliRunnerTests
{
    [Fact]
    public void StartInfoUsesNoShellAndNoWindow()
    {
        var info = CliRunner.StartInfo(Path.Combine("dir", "ghostdrive.exe"), new[] { "join", "a b", "c\"d" });
        Assert.False(info.UseShellExecute);
        Assert.True(info.CreateNoWindow);
        Assert.True(info.RedirectStandardInput);
        Assert.Equal(new[] { "join", "a b", "c\"d" }, info.ArgumentList);
        Assert.Equal("", info.Arguments);
        Assert.Equal("dir", info.WorkingDirectory);
    }

    [Fact]
    public async Task ArgumentsArriveIntact()
    {
        string[] args = { "a b", "c\"d", "e\\\\\"f", "", "-x" };
        CliOutput output;
        if (OperatingSystem.IsWindows())
        {
            // cmd.exe has its own parsing; check only that output and the exit code come back.
            output = await CliRunner.RunAsync("cmd.exe", new[] { "/c", "echo hello& exit 3" },
                TimeSpan.FromSeconds(30));
            Assert.Equal(3, output.ExitCode);
            Assert.Equal("hello", output.Stdout.Trim());
            return;
        }
        output = await CliRunner.RunAsync("/bin/sh",
            new[] { "-c", "for a in \"$@\"; do printf '[%s]' \"$a\"; done; echo err >&2; exit 4", "sh" }
                .Concat(args).ToList(),
            TimeSpan.FromSeconds(30));
        Assert.Equal(4, output.ExitCode);
        Assert.Equal("[a b][c\"d][e\\\\\"f][][-x]", output.Stdout);
        Assert.Equal("err\n", output.Stderr);
        Assert.False(output.Succeeded);
    }

    [Fact]
    public async Task ReadsUtf8Output()
    {
        if (OperatingSystem.IsWindows()) return;  // cmd.exe writes the console code page
        var output = await CliRunner.RunAsync("/bin/sh", new[] { "-c", "printf 'caf\\303\\251 \\377'" },
            TimeSpan.FromSeconds(30));
        Assert.Equal("café �", output.Stdout);
    }

    [Fact]
    public async Task SlowProgramsAreStopped()
    {
        var started = DateTime.UtcNow;
        var output = OperatingSystem.IsWindows()
            ? await CliRunner.RunAsync("ping.exe", new[] { "-n", "60", "127.0.0.1" }, TimeSpan.FromSeconds(1))
            : await CliRunner.RunAsync("/bin/sh", new[] { "-c", "sleep 60" }, TimeSpan.FromSeconds(1));
        Assert.True(output.TimedOut);
        Assert.False(output.Succeeded);
        Assert.True(DateTime.UtcNow - started < TimeSpan.FromSeconds(30));
    }

    [Fact]
    public async Task AMissingProgramIsALaunchError()
    {
        var missing = Path.Combine(Path.GetTempPath(), "gdapp-missing-" + Guid.NewGuid(), "ghostdrive.exe");
        await Assert.ThrowsAsync<CliLaunchException>(
            () => CliRunner.RunAsync(missing, Array.Empty<string>(), TimeSpan.FromSeconds(5)));
    }
}
