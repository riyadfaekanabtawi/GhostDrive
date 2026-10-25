using System.ComponentModel;
using System.Diagnostics;
using System.Text;

namespace GhostDriveApp.Core;

public sealed record CliOutput(int ExitCode, string Stdout, string Stderr, bool TimedOut = false)
{
    public bool Succeeded => !TimedOut && ExitCode == 0;
}

public sealed class CliLaunchException : Exception
{
    public CliLaunchException(string message, Exception? inner = null) : base(message, inner) { }
}

// Runs a program with an argument list: no shell, no console window, and stdin closed. Every
// argument goes through ProcessStartInfo.ArgumentList, which quotes it for
// CommandLineToArgvW and the C runtime, so names with spaces or quotes arrive intact.
public static class CliRunner
{
    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);

    public static ProcessStartInfo StartInfo(string program, IEnumerable<string> arguments,
        bool redirect = true)
    {
        var info = new ProcessStartInfo
        {
            FileName = program,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = redirect,
            RedirectStandardOutput = redirect,
            RedirectStandardError = redirect,
            WorkingDirectory = Path.GetDirectoryName(program) ?? "",
        };
        if (redirect)
        {
            // The ghostdrive programs write UTF-8 (their manifest sets the UTF-8 code page).
            // Invalid bytes become U+FFFD here, before any parsing.
            info.StandardOutputEncoding = Utf8;
            info.StandardErrorEncoding = Utf8;
        }
        foreach (var a in arguments) info.ArgumentList.Add(a);
        return info;
    }

    public static async Task<CliOutput> RunAsync(string program, IReadOnlyList<string> arguments,
        TimeSpan timeout, CancellationToken cancel = default)
    {
        using var process = new Process { StartInfo = StartInfo(program, arguments) };
        try
        {
            if (!process.Start()) throw new CliLaunchException($"Could not start {program}.");
        }
        catch (Win32Exception e)
        {
            throw new CliLaunchException($"Could not start {program}: {e.Message}", e);
        }
        process.StandardInput.Close();
        // Both pipes are read at once so neither can fill up and stall the program.
        var stdout = process.StandardOutput.ReadToEndAsync(CancellationToken.None);
        var stderr = process.StandardError.ReadToEndAsync(CancellationToken.None);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        deadline.CancelAfter(timeout);
        var timedOut = false;
        try
        {
            await process.WaitForExitAsync(deadline.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            timedOut = true;
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
                // It exited in the meantime.
            }
            await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
        }
        var output = new CliOutput(timedOut ? -1 : process.ExitCode,
            await stdout.ConfigureAwait(false), await stderr.ConfigureAwait(false), timedOut);
        cancel.ThrowIfCancellationRequested();
        return output;
    }
}
