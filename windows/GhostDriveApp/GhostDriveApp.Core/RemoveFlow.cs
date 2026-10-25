namespace GhostDriveApp.Core;

public enum DialogAnswer
{
    Cancelled,
    Confirmed,
}

// Removing a device is permanent, so the remove command exists only on the confirmed path:
// Commands.Remove is internal, and this is its one caller. The command is built and run only
// after the dialog answers Confirmed; Escape, closing the dialog and Cancel all answer
// Cancelled (the default), and so does a dialog that fails.
public static class RemoveFlow
{
    public sealed record Prompt(string Title, string Text, string ConfirmText, string CancelText)
    {
        // Enter must not remove: the default button is Cancel.
        public bool ConfirmIsDefault => false;
    }

    public sealed record Outcome(bool Ran, bool Succeeded, string Message, bool HasHint)
    {
        public static readonly Outcome Cancelled = new(false, false, "", false);
    }

    public static Prompt PromptFor(PeerStatus peer) => new(
        $"Remove {peer.Name}?",
        $"{peer.Name} will be taken out of the drive for good, on every device. Use this for a lost or stolen " +
        "device. To use it again, it needs a new identity and a new invite.",
        "Remove",
        "Cancel");

    public static async Task<Outcome> RunAsync(PeerStatus peer, AppSettings settings,
        Func<Prompt, Task<DialogAnswer>> confirm, Func<IReadOnlyList<string>, Task<CliOutput>> run)
    {
        DialogAnswer answer;
        try
        {
            answer = await confirm(PromptFor(peer));
        }
        catch (Exception)
        {
            return Outcome.Cancelled;
        }
        if (answer != DialogAnswer.Confirmed) return Outcome.Cancelled;

        var output = await run(Commands.Remove(settings, peer.Id));
        if (!output.Succeeded)
        {
            return new Outcome(true, false, DisplayText.Failure(null, output), false);
        }
        // The CLI's own text, including any devices the removed one added.
        var lines = DisplayText.Lines(output.Stdout, maxLines: 12, maxLength: 300);
        return new Outcome(true, true, string.Join(Environment.NewLine, lines), lines.Count > 1);
    }
}
