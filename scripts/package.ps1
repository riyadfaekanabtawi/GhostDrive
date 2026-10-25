# Builds the windows-release preset and packs the installed programs into
#   build\windows-release\ghostdrive-<version>-windows-x86_64.zip (plus a .sha256 file).
# The zip holds one folder with ghostdrive.exe, ghostdrived.exe, ghostdrive-rendezvous.exe,
# LICENSE and README.md. The programs link the vcpkg libraries and the C/C++ runtime statically
# (triplet x64-windows-static-release), so they need no DLLs beyond Windows itself; WinFsp's
# DLL is delay-loaded and only needed for mounting.
# Everything, including vcpkg's build trees and downloads, is written under build\windows-release.
#
# This is a PowerShell script rather than package.sh under Git Bash: Git Bash has no zip tool,
# the Visual Studio build environment (vcvars64.bat) is set up from cmd or PowerShell, and
# PowerShell is on every Windows machine. It needs PowerShell 7 (pwsh), Visual Studio 2022 with
# its C++ and clang-cl components, CMake, Ninja, and the WinFsp SDK for the mount.
#
# Usage: pwsh scripts/package.ps1 [extra cmake configure arguments]
# for example -DVCPKG_INSTALLED_DIR=... to reuse a vcpkg installed tree.
#Requires -Version 7
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\windows-release'
New-Item -ItemType Directory -Force -Path $build | Out-Null

# Ninja needs the compiler environment that vcvars64.bat sets up (INCLUDE, LIB, PATH).
if (-not $env:VCToolsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'package.ps1: Visual Studio with the C++ tools was not found' }
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

# vcpkg keeps its build trees and package staging in the vcpkg checkout unless told otherwise.
# Downloads go there too unless the caller already chose a place.
if (-not $env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $build 'vcpkg\downloads' }
New-Item -ItemType Directory -Force -Path $env:VCPKG_DOWNLOADS | Out-Null

Push-Location $root
try {
    # The programs and the license files go into the top of the package folder, as is usual on
    # Windows, rather than into bin\ and share\doc\ghostdrive. Quoted, because PowerShell splits
    # an unquoted argument such as -DX=. at the dot.
    cmake --preset windows-release -DGHOSTDRIVE_BUILD_TESTS=OFF `
        "-DCMAKE_INSTALL_BINDIR:STRING=." "-DCMAKE_INSTALL_DOCDIR:STRING=." `
        "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$build\vcpkg\buildtrees;--x-packages-root=$build\vcpkg\packages" `
        @args
    cmake --build --preset windows-release
} finally {
    Pop-Location
}

$cache = Get-Content (Join-Path $build 'CMakeCache.txt')
$version = ($cache | Select-String -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$').Matches.Groups[1].Value
if (-not $version) { throw "package.ps1: no project version in $build\CMakeCache.txt" }

$name = "ghostdrive-$version-windows-x86_64"
$stage = Join-Path $build 'package'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
cmake --install $build --prefix (Join-Path $stage $name)

$zip = Join-Path $build "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Entry names use forward slashes on .NET Core, so any unzip tool reads the folder correctly.
[System.IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $stage $name), $zip,
    [System.IO.Compression.CompressionLevel]::Optimal, $true)

# The same format as `shasum -a 256` and `sha256sum`, so `sha256sum -c` checks it too.
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n")
Remove-Item -Recurse -Force $stage
Write-Output $zip
