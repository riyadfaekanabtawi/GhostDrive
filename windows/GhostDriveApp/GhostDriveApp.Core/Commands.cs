namespace GhostDriveApp.Core;

// The argument lists the app passes to ghostdrive.exe and ghostdrived.exe (cli/ghostdrive.cpp,
// daemon/ghostdrived.cpp), built from the settings in one place so the exact commands are
// tested. Without a data folder or mount location setting the programs use their defaults.
public static class Commands
{
    // How long an invite stays valid; passed explicitly so the app knows when it expires.
    public const int InviteSeconds = 600;

    public static IReadOnlyList<string> Status(AppSettings s) => WithData(s, "status", "--json");

    public static IReadOnlyList<string> Init(AppSettings s, string name) =>
        WithData(s, "init", "--name", name);

    public static IReadOnlyList<string> Join(AppSettings s, string code, string name) =>
        WithData(s, "join", Positional(code), "--name", name);

    public static IReadOnlyList<string> Invite(AppSettings s, int seconds = InviteSeconds) =>
        WithData(s, "invite", "--ttl", seconds.ToString(System.Globalization.CultureInfo.InvariantCulture));

    // By ID: two devices can share a name, and an ID names exactly one. Internal: only
    // RemoveFlow, after a confirmation, builds this command.
    internal static IReadOnlyList<string> Remove(AppSettings s, string id) =>
        WithData(s, "remove", Positional(id));

    public static IReadOnlyList<string> Stop(AppSettings s) => WithData(s, "stop");

    public static IReadOnlyList<string> RendezvousShow(AppSettings s) => WithData(s, "rendezvous");

    public static IReadOnlyList<string> RendezvousSet(AppSettings s, string address,
        string fingerprint) =>
        WithData(s, "rendezvous", Positional(address), Positional(fingerprint));

    public static IReadOnlyList<string> AutostartOn(AppSettings s)
    {
        var args = WithData(s, "autostart", "on");
        if (s.MountPath is { } mount) args.AddRange(new[] { "--mount", mount });
        return args;
    }

    public static IReadOnlyList<string> AutostartOff(AppSettings s) =>
        WithData(s, "autostart", "off");

    public static IReadOnlyList<string> AutostartStatus(AppSettings s) =>
        WithData(s, "autostart", "status");

    // ghostdrived.exe, started by Start GhostDrive.
    public static IReadOnlyList<string> Daemon(AppSettings s)
    {
        var args = new List<string>();
        if (s.DataPath is { } data) args.AddRange(new[] { "--data", data });
        if (s.MountPath is { } mount) args.AddRange(new[] { "--mount", mount });
        return args;
    }

    private static List<string> WithData(AppSettings s, params string[] args)
    {
        var list = new List<string>(args);
        if (s.DataPath is { } data) list.AddRange(new[] { "--data", data });
        return list;
    }

    // The CLI reads any argument starting with "-" as an option, so a positional value like
    // that would be misread; callers validate input first (Input), and this is the backstop.
    private static string Positional(string value)
    {
        if (value.Length == 0 || value[0] == '-')
        {
            throw new ArgumentException("A value may not be empty or start with '-'.",
                nameof(value));
        }
        return value;
    }
}
