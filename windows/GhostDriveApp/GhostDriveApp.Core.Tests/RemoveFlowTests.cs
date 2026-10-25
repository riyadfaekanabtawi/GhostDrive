using GhostDriveApp.Core;

namespace GhostDriveApp.Core.Tests;

public class RemoveFlowTests
{
    private static readonly PeerStatus Laptop = new("laptop", "AAAA-BBBB", null, true, false);
    private static readonly AppSettings Settings = new() { DataFolder = @"D:\gd" };

    private sealed class Runner
    {
        public List<IReadOnlyList<string>> Calls { get; } = new();
        public CliOutput Output { get; set; } = new(0, "Removed laptop (AAAA-BBBB). It can no longer connect.\n", "");

        public Task<CliOutput> Run(IReadOnlyList<string> args)
        {
            Calls.Add(args);
            return Task.FromResult(Output);
        }
    }

    [Fact]
    public async Task CancelRunsNothing()
    {
        var runner = new Runner();
        var outcome = await RemoveFlow.RunAsync(Laptop, Settings,
            _ => Task.FromResult(DialogAnswer.Cancelled), runner.Run);
        Assert.Empty(runner.Calls);
        Assert.False(outcome.Ran);
        Assert.Same(RemoveFlow.Outcome.Cancelled, outcome);
    }

    [Fact]
    public async Task ADefaultAnswerIsCancel()
    {
        // Escape and closing the dialog give no button; the UI maps that to default(DialogAnswer).
        Assert.Equal(DialogAnswer.Cancelled, default(DialogAnswer));
        var runner = new Runner();
        await RemoveFlow.RunAsync(Laptop, Settings, _ => Task.FromResult(default(DialogAnswer)), runner.Run);
        Assert.Empty(runner.Calls);
    }

    [Fact]
    public async Task AFailingDialogRunsNothing()
    {
        var runner = new Runner();
        var outcome = await RemoveFlow.RunAsync(Laptop, Settings,
            _ => Task.FromException<DialogAnswer>(new InvalidOperationException("no XamlRoot")), runner.Run);
        Assert.Empty(runner.Calls);
        Assert.False(outcome.Ran);
    }

    [Fact]
    public async Task ConfirmRunsRemoveByIdOnce()
    {
        var runner = new Runner();
        RemoveFlow.Prompt? shown = null;
        var outcome = await RemoveFlow.RunAsync(Laptop, Settings, p =>
        {
            shown = p;
            Assert.Empty(runner.Calls);  // nothing runs while the dialog is open
            return Task.FromResult(DialogAnswer.Confirmed);
        }, runner.Run);
        var call = Assert.Single(runner.Calls);
        Assert.Equal(new[] { "remove", "AAAA-BBBB", "--data", @"D:\gd" }, call);
        Assert.True(outcome.Ran);
        Assert.True(outcome.Succeeded);
        Assert.False(outcome.HasHint);
        Assert.NotNull(shown);
        Assert.Equal("Remove laptop?", shown.Title);
        Assert.Equal("Cancel", shown.CancelText);
        Assert.False(shown.ConfirmIsDefault);
    }

    [Fact]
    public async Task TheHintAboutAddedDevicesIsKept()
    {
        var runner = new Runner
        {
            Output = new CliOutput(0,
                "Removed laptop (AAAA-BBBB). It can no longer connect to this drive's devices.\n" +
                "phone (CCCC) was added by it. If you do not recognise it, remove it too: ghostdrive remove CCCC\n",
                ""),
        };
        var outcome = await RemoveFlow.RunAsync(Laptop, Settings, _ => Task.FromResult(DialogAnswer.Confirmed), runner.Run);
        Assert.True(outcome.HasHint);
        Assert.Contains("phone (CCCC) was added by it", outcome.Message);
    }

    [Fact]
    public async Task FailuresShowTheCliError()
    {
        var runner = new Runner { Output = new CliOutput(1, "", "ghostdrive: error: no device called laptop in this drive\n") };
        var outcome = await RemoveFlow.RunAsync(Laptop, Settings, _ => Task.FromResult(DialogAnswer.Confirmed), runner.Run);
        Assert.True(outcome.Ran);
        Assert.False(outcome.Succeeded);
        Assert.Equal("no device called laptop in this drive", outcome.Message);
    }
}
