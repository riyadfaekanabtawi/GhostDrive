using System.Buffers;
using System.Globalization;
using System.Text;

namespace GhostDriveApp.Core;

// Text from the daemon or the CLI made safe to show. Device names come from other devices, and
// the app trusts no string it did not write itself. Control characters, format characters
// (every bidirectional mark, embedding, override and isolate, and the zero-width characters),
// line and paragraph separators, private-use characters and unpaired surrogates become "?";
// long text is cut short.
public static class DisplayText
{
    public const int MaxLength = 64;
    private const string CliPrefix = "ghostdrive: ";

    public static bool IsUnsafe(Rune rune) => Rune.GetUnicodeCategory(rune) switch
    {
        UnicodeCategory.Control or UnicodeCategory.Format or UnicodeCategory.LineSeparator
            or UnicodeCategory.ParagraphSeparator or UnicodeCategory.Surrogate
            or UnicodeCategory.PrivateUse => true,
        _ => false,
    };

    public static string Clean(string? text, int maxLength = MaxLength)
    {
        if (string.IsNullOrEmpty(text)) return "";
        var sb = new StringBuilder(text.Length);
        var rest = text.AsSpan();
        while (!rest.IsEmpty)
        {
            var status = Rune.DecodeFromUtf16(rest, out var rune, out var consumed);
            if (status != OperationStatus.Done || IsUnsafe(rune))
            {
                sb.Append('?');
            }
            else
            {
                sb.Append(rune.ToString());
            }
            rest = rest[Math.Max(consumed, 1)..];
        }
        return Truncate(sb.ToString(), maxLength);
    }

    // Cuts at a whole user-perceived character, so an accent is not separated from its letter.
    private static string Truncate(string text, int maxLength)
    {
        if (maxLength < 4) maxLength = 4;
        var info = new StringInfo(text);
        if (info.LengthInTextElements <= maxLength) return text;
        return info.SubstringByTextElements(0, maxLength - 3) + "...";
    }

    // Up to `maxLines` non-empty lines of CLI output, each cleaned, with the "ghostdrive: "
    // prefix the CLI puts on errors removed.
    public static IReadOnlyList<string> Lines(string? text, int maxLines = 4, int maxLength = 160)
    {
        if (string.IsNullOrEmpty(text)) return Array.Empty<string>();
        return text.Split('\n', '\r')
            .Select(line => line.Trim())
            .Where(line => line.Length > 0)
            .Select(line => line.StartsWith(CliPrefix, StringComparison.Ordinal)
                ? line[CliPrefix.Length..]
                : line)
            .Select(Plain)
            .Take(maxLines)
            .Select(line => Clean(line, maxLength))
            .ToList();
    }

    // What to show when a command failed: its error output (or its normal output when it wrote
    // no errors), after an optional heading.
    public static string Failure(string? heading, CliOutput output)
    {
        if (output.TimedOut)
        {
            return (heading is null ? "" : heading + ": ")
                + "ghostdrive did not finish in time.";
        }
        var lines = Lines(string.IsNullOrWhiteSpace(output.Stderr) ? output.Stdout : output.Stderr)
            .ToList();
        if (lines.Count == 0) lines.Add($"ghostdrive stopped with exit code {output.ExitCode}.");
        if (heading is not null) lines.Insert(0, heading + ":");
        return string.Join(Environment.NewLine, lines);
    }

    // The CLI talks to a terminal user; in the app the same advice reads differently.
    private static string Plain(string line)
    {
        line = line.Replace("Start it with `ghostdrived`.",
            "GhostDrive is not running; start it from Home.", StringComparison.Ordinal);
        if (line.StartsWith("error: ", StringComparison.Ordinal)) line = line["error: ".Length..];
        return line.Replace("`", "", StringComparison.Ordinal);
    }
}
