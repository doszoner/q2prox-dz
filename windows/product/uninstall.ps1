[CmdletBinding()]
param([switch]$ResetState)

$ErrorActionPreference = 'Stop'
$root = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone'
$versions = Join-Path $root 'versions'
$state = Join-Path $root 'state'
$startMenu = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'

function Test-Reparse([System.IO.FileSystemInfo]$Item) {
    return (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Get-NormalizedPath([string]$Path) {
    return ([IO.Path]::GetFullPath($Path)).TrimEnd('\').ToLowerInvariant()
}

function Get-PowerShellPath {
    return (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe')
}

function Get-ShortcutArguments([string]$Target) {
    return "-NoProfile -ExecutionPolicy Bypass -File `"$Target`""
}

function Assert-SafeDirectoryPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $rootPath = [IO.Path]::GetPathRoot($full)
    $current = $rootPath
    $remainder = $full.Substring($rootPath.Length).Trim('\')
    if ([string]::IsNullOrEmpty($remainder)) { return }
    foreach ($part in $remainder.Split('\')) {
        $current = Join-Path $current $part
        if (-not (Test-Path -LiteralPath $current)) { break }
        $item = Get-Item -LiteralPath $current -Force
        if (Test-Reparse $item) { throw "Refusing Start Menu reparse point: $current" }
        if (-not $item.PSIsContainer) { throw "Start Menu path component is not a directory: $current" }
    }
}

function Assert-SafeTree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (Test-Reparse $item) { throw "Refusing reparse point: $Path" }
    if ($item.PSIsContainer) {
        foreach ($child in $item.GetFileSystemInfos()) {
            if (Test-Reparse $child) { throw "Refusing reparse point: $($child.FullName)" }
            if ($child.PSIsContainer) { Assert-SafeTree $child.FullName }
        }
    }
}

function Test-OwnedMarker([string]$Path) {
    $markerPath = Join-Path $Path '.q2dz-owned'
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { return $false }
    $markerItem = Get-Item -LiteralPath $markerPath -Force
    if (Test-Reparse $markerItem) { throw "Refusing reparse point: $markerPath" }
    try { $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json } catch { return $false }
    if ($marker.schema -ne 1 -or $marker.product -ne 'q2prox-dz' -or [string]::IsNullOrWhiteSpace([string]$marker.version) -or [string]::IsNullOrWhiteSpace([string]$marker.payloadDigest)) { return $false }
    if (@($marker.files).Count -eq 0) { return $false }
    foreach ($entry in @($marker.files)) {
        if ([string]$entry.path -notmatch '^[^\\/]+(?:/[^\\/]+)*$' -or [string]$entry.sha256 -notmatch '^[0-9a-fA-F]{64}$') { return $false }
        $file = Join-Path $Path ([string]$entry.path -replace '/', '\')
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $false }
        $item = Get-Item -LiteralPath $file -Force
        if (Test-Reparse $item) { throw "Refusing reparse point: $file" }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() -ne ([string]$entry.sha256).ToLowerInvariant()) { return $false }
    }
    $actual = @(Get-ChildItem -LiteralPath $Path -File -Recurse -Force | ForEach-Object { $_.FullName.Substring($Path.Length + 1).Replace('\', '/') } | Where-Object { $_ -ne '.q2dz-owned' } | Sort-Object)
    $expected = @($marker.files | ForEach-Object { [string]$_.path } | Sort-Object)
    return ((Compare-Object $expected $actual).Count -eq 0)
}

function Remove-SafeFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (Test-Reparse $item) { throw "Refusing reparse point: $Path" }
    if ($item.PSIsContainer) { throw "Refusing directory where file was expected: $Path" }
    Remove-Item -LiteralPath $Path -Force
}

function Remove-OwnedShortcut([string]$Name, [string]$StableTarget) {
    $link = Join-Path $startMenu $Name
    if (-not (Test-Path -LiteralPath $link)) { return }
    $item = Get-Item -LiteralPath $link -Force
    if (Test-Reparse $item) { throw "Refusing shortcut reparse point: $link" }
    if ($item.PSIsContainer) { throw "Refusing directory where shortcut was expected: $link" }
    $shell = New-Object -ComObject WScript.Shell
    try { $shortcut = $shell.CreateShortcut($link) } catch { return }
    try {
        $target = Get-NormalizedPath ([string]$shortcut.TargetPath)
        $expected = Get-NormalizedPath (Get-PowerShellPath)
    } catch { return }
    $args = [string]$shortcut.Arguments
    if ($target -ne $expected -or $args -cne (Get-ShortcutArguments $StableTarget)) { return }
    Remove-Item -LiteralPath $link -Force
}

if (-not (Test-Path -LiteralPath $root)) { Write-Host 'Q2PRO-X - Dos.Zone add-on is not installed.'; exit 0 }
Assert-SafeTree $root
Assert-SafeDirectoryPath $startMenu

# Preflight all owned trees before any mutation. This makes a junction/symlink a hard stop.
if (Test-Path -LiteralPath $versions) {
    Assert-SafeTree $versions
}
if ($ResetState -and (Test-Path -LiteralPath $state)) { Assert-SafeTree $state }
foreach ($owned in @('current.json', 'launch.ps1', 'uninstall.ps1')) {
    $path = Join-Path $root $owned
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (Test-Reparse $item -or $item.PSIsContainer) { throw "Refusing unsafe owned path: $path" }
    }
}

Remove-OwnedShortcut 'Q2PRO-X - Dos.Zone.lnk' (Join-Path $root 'launch.ps1')
Remove-OwnedShortcut 'Uninstall Q2PRO-X - Dos.Zone.lnk' (Join-Path $root 'uninstall.ps1')

if (Test-Path -LiteralPath $versions) {
    foreach ($entry in @(Get-ChildItem -LiteralPath $versions -Force)) {
        if ($entry.PSIsContainer -and $entry.Name -like '.staging-*' -and (Test-OwnedMarker $entry.FullName)) {
            Remove-Item -LiteralPath $entry.FullName -Recurse -Force
        } elseif ($entry.PSIsContainer -and (Test-OwnedMarker $entry.FullName)) {
            Remove-Item -LiteralPath $entry.FullName -Recurse -Force
        }
    }
    if (@(Get-ChildItem -LiteralPath $versions -Force -ErrorAction SilentlyContinue).Count -eq 0) { Remove-Item -LiteralPath $versions -Force }
}
Remove-SafeFile (Join-Path $root 'current.json')
Remove-SafeFile (Join-Path $root 'launch.ps1')
Remove-SafeFile (Join-Path $root 'uninstall.ps1')

if ($ResetState) {
    Remove-SafeFile (Join-Path $state 'q2-path.txt')
    if ((Test-Path -LiteralPath $state -PathType Container) -and (@(Get-ChildItem -LiteralPath $state -Force).Count -eq 0)) { Remove-Item -LiteralPath $state -Force }
}

Write-Host $(if ($ResetState) { 'Removed the Dos.Zone add-on and reset its saved Q2PRO-X path.' } else { 'Removed the Dos.Zone add-on. Saved Q2PRO-X path and unknown files were preserved.' })
