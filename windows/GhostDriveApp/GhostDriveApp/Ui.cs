using System.Diagnostics;
using GhostDriveApp.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GhostDriveApp;

internal static class Ui
{
    public static Brush Brush(string key) => (Brush)Application.Current.Resources[key];

    public static Brush StateBrush(bool good, bool warn = false) => Brush(
        good ? "SystemFillColorSuccessBrush" : warn ? "SystemFillColorCautionBrush" : "SystemFillColorCriticalBrush");

    public static Visibility Visible(bool on) => on ? Visibility.Visible : Visibility.Collapsed;

    // A closed InfoBar is also collapsed, so it takes no StackPanel spacing.
    public static void Show(InfoBar bar, InfoBarSeverity severity, string? message, string title = "")
    {
        bar.Severity = severity;
        bar.Title = title;
        bar.Message = message ?? "";
        bar.IsOpen = !string.IsNullOrEmpty(message) || !string.IsNullOrEmpty(title);
        bar.Visibility = Visible(bar.IsOpen);
        bar.Closed -= OnClosed;
        bar.Closed += OnClosed;
    }

    public static void Hide(InfoBar bar)
    {
        bar.IsOpen = false;
        bar.Visibility = Visibility.Collapsed;
    }

    private static void OnClosed(InfoBar bar, InfoBarClosedEventArgs args) =>
        bar.Visibility = Visibility.Collapsed;

    // Opens a folder or drive in Explorer, without a shell: explorer.exe with one argument.
    public static void OpenInExplorer(string path)
    {
        var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        var info = CliRunner.StartInfo(Path.Combine(windows, "explorer.exe"), new[] { path },
            redirect: false);
        using var _ = Process.Start(info);
    }

    public static async Task<bool> ConfirmAsync(XamlRoot root, string title, string text,
        string confirm)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = root,
            Title = title,
            Content = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap },
            PrimaryButtonText = confirm,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }
}
