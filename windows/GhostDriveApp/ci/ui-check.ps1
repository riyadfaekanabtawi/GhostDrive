# Starts the built app against throwaway daemons and saves screenshots of its pages (CI job
# windows-app). Two drives' devices run with --no-mount in temporary data folders, so the
# Devices page has a device to show. Fails if the app does not start or exits on its own.
param(
    [Parameter(Mandatory)] [string] $App,        # folder with GhostDriveApp.exe
    [Parameter(Mandatory)] [string] $Cli,        # ghostdrive.exe
    [Parameter(Mandatory)] [string] $Daemon,     # ghostdrived.exe
    [Parameter(Mandatory)] [string] $OutDir      # screenshots go here
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$work = Join-Path $env:RUNNER_TEMP "gdapp-ui-$PID"
$a = Join-Path $work 'desk'
$b = Join-Path $work 'laptop'
New-Item -ItemType Directory -Force $work | Out-Null

# Native programs' stderr must not count as a PowerShell error here; the exit code decides.
function Invoke-Cli {
    $ErrorActionPreference = 'Continue'
    $out = & $Cli @args 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "ghostdrive $($args -join ' ') failed ($LASTEXITCODE): $out" }
    return $out
}

function Start-Daemon([string] $data, [int] $port, [string] $name) {
    Start-Process -FilePath $Daemon -ArgumentList @('--no-mount', '--data', "`"$data`"", '--port', $port) `
        -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $OutDir "$name-daemon.out.txt") `
        -RedirectStandardError (Join-Path $OutDir "$name-daemon.err.txt")
}

function Wait-Status([string] $data, [scriptblock] $until, [int] $seconds) {
    $ErrorActionPreference = 'Continue'
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $json = & $Cli status --json --data $data 2>$null | Out-String
        if ($LASTEXITCODE -eq 0) {
            $s = $json | ConvertFrom-Json
            if (& $until $s) { return $s }
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
'@

# PrintWindow with PW_RENDERFULLCONTENT (2) captures DirectComposition content such as WinUI's,
# even when another window covers it. A copy of the screen is the fallback.
function Save-Window([System.Diagnostics.Process] $p, [string] $file) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    $r = New-Object Win+RECT
    if ($h -ne [IntPtr]::Zero -and [Win]::GetWindowRect($h, [ref] $r) -and $r.Right -gt $r.Left) {
        [Win]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 500
        $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $dc = $g.GetHdc()
        $ok = [Win]::PrintWindow($h, $dc, 2)
        $g.ReleaseHdc($dc)
        if (-not $ok) { $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size) }
        $g.Dispose()
    } else {
        Write-Warning 'No main window handle; saving the whole screen'
        $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
        $g.Dispose()
    }
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$daemons = @()
try {
    Invoke-Cli init --data $a --name ci-desk | Write-Host
    $daemons += Start-Daemon $a 47373 'desk'
    if (-not (Wait-Status $a { $true } 30)) { throw 'the first daemon did not answer' }

    $invite = Invoke-Cli invite --data $a
    $code = [regex]::Match($invite, 'gd1-[A-Z2-7-]+').Value
    if (-not $code) { throw "no invite code in: $invite" }
    Invoke-Cli join $code --data $b --name ci-laptop --port 47374 | Write-Host
    $daemons += Start-Daemon $b 47374 'laptop'
    $s = Wait-Status $a { param($s) @($s.peers | Where-Object online).Count -gt 0 } 60
    if ($s) { Write-Host 'ci-laptop is online' } else { Write-Warning 'ci-laptop did not come online' }

    # The app's settings: a data folder and the ghostdrive.exe from the build tree.
    $settingsDir = Join-Path $env:LOCALAPPDATA 'GhostDriveApp'
    New-Item -ItemType Directory -Force $settingsDir | Out-Null
    function Set-AppData([string] $data) {
        @{ DataFolder = $data; MountLocation = ''; CliPath = $Cli } | ConvertTo-Json |
            Set-Content -Encoding utf8 (Join-Path $settingsDir 'settings.json')
    }

    # Starts the app on a page, optionally clicks a button by its name through UI Automation,
    # saves a screenshot and closes the app.
    function Show-Page([string] $page, [string] $shot, [string] $click = '') {
        $p = Start-Process -FilePath (Join-Path $App 'GhostDriveApp.exe') -ArgumentList '--page', $page -PassThru
        Start-Sleep -Seconds 12
        if ($p.HasExited) { throw "GhostDriveApp exited on its own with code $($p.ExitCode) ($shot)" }
        if ($click) {
            try {
                $p.Refresh()
                $root = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
                $cond = New-Object System.Windows.Automation.PropertyCondition(
                    [System.Windows.Automation.AutomationElement]::NameProperty, $click)
                $button = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
                $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                Start-Sleep -Seconds 4
            } catch {
                Write-Warning "could not click '$click': $_"
            }
        }
        Save-Window $p (Join-Path $OutDir "$shot.png")
        Write-Host "saved $shot.png"
        if (-not $p.CloseMainWindow() -or -not $p.WaitForExit(10000)) {
            Write-Warning "GhostDriveApp did not close; stopping it"
            $p.Kill()
        }
        if ($p.ExitCode -ne 0 -and $p.ExitCode -ne -1) { Write-Warning "GhostDriveApp exit code $($p.ExitCode)" }
    }

    Set-AppData $a
    Show-Page home home
    Show-Page devices devices
    Show-Page add add-invite 'Create invite code'
    Show-Page settings settings

    # A data folder without a drive shows the first-run choices.
    Set-AppData (Join-Path $work 'empty')
    Show-Page home first-run
} finally {
    $ErrorActionPreference = 'Continue'
    foreach ($d in $a, $b) { & $Cli stop --data $d 2>$null | Out-Null }
    Start-Sleep -Seconds 2
    $daemons | Where-Object { -not $_.HasExited } | ForEach-Object { $_.Kill() }
}
