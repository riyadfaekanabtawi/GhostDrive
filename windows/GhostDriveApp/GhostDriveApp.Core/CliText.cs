using System.Globalization;
using System.Text.RegularExpressions;

namespace GhostDriveApp.Core;

// What `ghostdrive invite` prints (cli/ghostdrive.cpp, cmd_invite):
//
//   On the other device, run:
//
//     ghostdrive join gd1-XXXXXXXX-XXXXXXXX-...
//   The code works once, for 10 minutes. Anyone who has it can join, so send it privately.
public sealed record Invite(string Code, int? Minutes)
{
    private static readonly Regex MinutesPattern =
        new(@"for (\d+) minutes?", RegexOptions.CultureInvariant);

    public static Invite? Parse(string? text)
    {
        if (string.IsNullOrEmpty(text)) return null;
        var code = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault(IsCode);
        if (code is null) return null;
        var m = MinutesPattern.Match(text);
        int? minutes = m.Success &&
                       int.TryParse(m.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture,
                           out var n)
            ? n
            : null;
        return new Invite(code, minutes);
    }

    // "gd1-" and dash-separated groups of base32 (core/src/bytes.cpp, daemon/src/invite.cpp).
    public static bool IsCode(string word)
    {
        if (!word.StartsWith("gd1-", StringComparison.Ordinal)) return false;
        var groups = word[4..].Split('-');
        return groups.Length > 0 && groups.All(g => g.Length > 0 && g.All(IsBase32));
    }

    private static bool IsBase32(char c) => c is >= 'A' and <= 'Z' or >= '2' and <= '7';
}

// `ghostdrive autostart status` on Windows prints "login task: <name>" or "login task: not
// set up" (macOS and Linux print "login item:").
public static class AutostartText
{
    public static bool? IsSetUp(string? text)
    {
        if (string.IsNullOrEmpty(text)) return null;
        foreach (var raw in text.Split('\n', '\r'))
        {
            var line = raw.Trim();
            foreach (var label in new[] { "login task:", "login item:" })
            {
                if (line.StartsWith(label, StringComparison.Ordinal))
                {
                    return line[label.Length..].Trim() != "not set up";
                }
            }
        }
        return null;
    }
}

public sealed record RendezvousSetting(string Address, string Fingerprint)
{
    // `ghostdrive rendezvous` prints "none" or "HOST:PORT FINGERPRINT".
    public static RendezvousSetting? Parse(string? text)
    {
        var line = text?.Trim();
        if (string.IsNullOrEmpty(line) || line == "none") return null;
        var space = line.IndexOf(' ');
        if (space <= 0) return null;
        return new RendezvousSetting(DisplayText.Clean(line[..space]),
            DisplayText.Clean(line[(space + 1)..].Trim(), 128));
    }
}

// Input typed into the app, checked before it reaches a command line.
public static class Input
{
    public const int MaxNameLength = 64;

    // Null when the name is usable; otherwise why not.
    public static string? DeviceNameProblem(string? name)
    {
        var n = name?.Trim() ?? "";
        if (n.Length == 0) return "Enter a name for this device.";
        if (new StringInfo(n).LengthInTextElements > MaxNameLength)
        {
            return $"Use at most {MaxNameLength} characters.";
        }
        if (DisplayText.Clean(n, int.MaxValue) != n)
        {
            return "The name contains characters that cannot be shown, such as control characters.";
        }
        return null;
    }

    // The invite code from whatever was pasted: the code alone, the whole `ghostdrive join`
    // line, or the full output of `ghostdrive invite`. Lowercase is accepted, as the CLI does.
    public static string? InviteCode(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        foreach (var word in text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
        {
            var upper = word.Trim().ToUpperInvariant();
            if (upper.StartsWith("GD1-", StringComparison.Ordinal) && Invite.IsCode("gd1-" + upper[4..]))
            {
                return "gd1-" + upper[4..];
            }
        }
        return null;
    }

    // HOST:PORT, with an optional [IPv6] host. The CLI checks it again.
    public static string? RendezvousAddressProblem(string? address)
    {
        var a = address?.Trim() ?? "";
        if (a.Length == 0) return "Enter the server as host:port, for example rv.example.org:7374.";
        if (a.StartsWith('-') || a.Any(char.IsWhiteSpace) || DisplayText.Clean(a, int.MaxValue) != a)
        {
            return "The server address cannot contain spaces or control characters.";
        }
        var colon = a.LastIndexOf(':');
        if (colon <= 0 || colon == a.Length - 1 ||
            !ushort.TryParse(a[(colon + 1)..], NumberStyles.None, CultureInfo.InvariantCulture,
                out var port) || port == 0)
        {
            return "Add the port after a colon, for example rv.example.org:7374.";
        }
        return null;
    }

    // The fingerprint the server printed, without the dashes and spaces it is shown with (the
    // CLI drops them too). Null when it is not base32.
    public static string? Fingerprint(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) return null;
        var compact = new string(text.Where(c => !char.IsWhiteSpace(c) && c != '-').ToArray())
            .ToUpperInvariant();
        if (compact.Length == 0 || !compact.All(c => c is >= 'A' and <= 'Z' or >= '2' and <= '7'))
        {
            return null;
        }
        return compact;
    }

    // A drive letter ("X:") or a full folder path; null when usable.
    public static string? MountLocationProblem(string? location)
    {
        var m = location?.Trim() ?? "";
        if (m.Length == 0) return null;  // the default
        if (MountLocation.IsDriveLetter(m)) return null;
        if (!MountLocation.IsFullPath(m))
        {
            return @"Use a drive letter such as G: or a full folder path such as C:\Users\me\GhostDrive.";
        }
        if (DisplayText.Clean(m, int.MaxValue) != m) return "The path contains control characters.";
        return null;
    }

    public static string? DataFolderProblem(string? folder)
    {
        var f = folder?.Trim() ?? "";
        if (f.Length == 0) return null;
        if (!MountLocation.IsFullPath(f)) return @"Use a full folder path such as D:\GhostDriveData.";
        if (DisplayText.Clean(f, int.MaxValue) != f) return "The path contains control characters.";
        return null;
    }
}

public static class MountLocation
{
    public static bool IsDriveLetter(string text) =>
        text.Length == 2 && text[1] == ':' && char.IsAsciiLetter(text[0]);

    // "C:\..." or a UNC path "\\server\share\...". Written out rather than Path.IsPathRooted
    // so the result is the same on every OS the tests run on.
    public static bool IsFullPath(string text) =>
        (text.Length >= 3 && char.IsAsciiLetter(text[0]) && text[1] == ':' &&
         (text[2] == '\\' || text[2] == '/')) ||
        (text.Length >= 5 && text.StartsWith(@"\\", StringComparison.Ordinal) &&
         text[2] != '\\' && text[2] != '?' && text[2] != '.');

    // What Explorer should open for a mountpoint the daemon reports: "X:" means the drive's
    // root, not the current folder on drive X.
    public static string ExplorerTarget(string mountpoint) =>
        IsDriveLetter(mountpoint) ? mountpoint + "\\" : mountpoint;
}

// "9:41" for an invite's remaining validity.
public static class Countdown
{
    public static string Format(TimeSpan remaining)
    {
        if (remaining <= TimeSpan.Zero) return "expired";
        var seconds = (int)Math.Ceiling(remaining.TotalSeconds);
        return string.Create(CultureInfo.InvariantCulture, $"{seconds / 60}:{seconds % 60:00}");
    }
}
