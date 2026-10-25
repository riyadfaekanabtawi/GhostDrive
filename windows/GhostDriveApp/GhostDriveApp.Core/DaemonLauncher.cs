using System.ComponentModel;
using System.Diagnostics;
using System.Text;

namespace GhostDriveApp.Core;

// Starts ghostdrived.exe without a console window. It keeps running after the app exits and
// is stopped with `ghostdrive stop`. Its output is collected (up to a limit) so that if it
// exits while starting, for example because the mount failed, its error can be shown.
public sealed class DaemonLauncher : IDisposable
{
    private const int MaxOutput = 16 * 1024;
    private readonly Process process;
    private readonly StringBuilder output = new();

    private DaemonLauncher(Process process)
    {
        this.process = process;
    }

    public static DaemonLauncher Start(string daemon, IReadOnlyList<string> arguments)
    {
        var process = new Process { StartInfo = CliRunner.StartInfo(daemon, arguments) };
        var launcher = new DaemonLauncher(process);
        process.OutputDataReceived += (_, e) => launcher.Append(e.Data);
        process.ErrorDataReceived += (_, e) => launcher.Append(e.Data);
        try
        {
            if (!process.Start()) throw new CliLaunchException($"Could not start {daemon}.");
        }
        catch (Win32Exception e)
        {
            process.Dispose();
            throw new CliLaunchException($"Could not start {daemon}: {e.Message}", e);
        }
        process.StandardInput.Close();
        // Read for as long as the app runs, so a full pipe never blocks the daemon. Once the
        // app exits, the daemon's writes to the closed pipes fail and are ignored.
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        return launcher;
    }

    private void Append(string? line)
    {
        if (line is null) return;
        lock (output)
        {
            if (output.Length < MaxOutput) output.AppendLine(line);
        }
    }

    public string Output
    {
        get
        {
            lock (output) return output.ToString();
        }
    }

    public bool HasExited => process.HasExited;

    public int? ExitCode => process.HasExited ? process.ExitCode : null;

    // Waits until `ready` reports the daemon answers, or it exits, or the time runs out.
    // Returns null when it is running, or else what went wrong. It continues on the caller's
    // context (no ConfigureAwait(false)), so `ready` may update the UI.
    public async Task<string?> WaitUntilReadyAsync(Func<Task<bool>> ready, TimeSpan timeout,
        CancellationToken cancel = default)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (process.HasExited)
            {
                // Let the readers take the last lines.
                await process.WaitForExitAsync(cancel);
                var lines = DisplayText.Lines(Output, maxLines: 6);
                var text = lines.Count > 0
                    ? string.Join(Environment.NewLine, lines)
                    : $"ghostdrived stopped with exit code {process.ExitCode}.";
                return "GhostDrive could not start:" + Environment.NewLine + text;
            }
            if (await ready()) return null;
            await Task.Delay(500, cancel);
        }
        return "GhostDrive is still starting. Check again in a moment.";
    }

    public void Dispose() => process.Dispose();
}
