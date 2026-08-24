[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$setup = (Resolve-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path
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

function Assert-OwnedShortcut([string]$Path, [string]$Target, [string]$LegacyTarget) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (Test-Reparse $item) { throw "Refusing shortcut reparse point: $Path" }
    if ($item.PSIsContainer) { throw "Refusing directory where shortcut was expected: $Path" }
    $shell = New-Object -ComObject WScript.Shell
    try { $shortcut = $shell.CreateShortcut($Path) } catch { throw "Refusing unrelated shortcut: $Path" }
    $actualTarget = Get-NormalizedPath ([string]$shortcut.TargetPath)
    $actualArguments = [string]$shortcut.Arguments
    $expectedTarget = Get-NormalizedPath (Get-PowerShellPath)
    $owned = @(
        @{ Target = $Target; Arguments = (Get-ShortcutArguments $Target) },
        @{ Target = $LegacyTarget; Arguments = (Get-ShortcutArguments $LegacyTarget) }
    ) | Where-Object {
        $_.Target -and $actualTarget -eq $expectedTarget -and
        $actualArguments -ceq $_.Arguments
    }
    if (-not $owned) {
        throw "Refusing unrelated shortcut: $Path"
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

function Get-RelativeFiles([string]$Path) {
    @(Get-ChildItem -LiteralPath $Path -File -Recurse -Force | ForEach-Object {
        $_.FullName.Substring($Path.Length + 1).Replace('\', '/')
    } | Sort-Object)
}

function Assert-Manifest([string]$Path) {
    $manifestPath = Join-Path $Path 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Setup SHA256SUMS.txt is missing.' }
    $entries = @{}
    foreach ($line in @(Get-Content -LiteralPath $manifestPath)) {
        if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw 'Setup manifest contains an invalid line.' }
        $hash = $Matches[1].ToLowerInvariant()
        $relative = $Matches[2]
        if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)' -or $relative -match '^[A-Za-z]:') {
            throw "Unsafe setup manifest path: $relative"
        }
        $file = Join-Path $Path ($relative -replace '/', '\')
        if ($entries.ContainsKey($relative) -or -not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Invalid setup manifest file: $relative" }
        $item = Get-Item -LiteralPath $file -Force
        if (Test-Reparse $item) { throw "Setup contains a reparse point: $relative" }
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()
        if ($actual -ne $hash) { throw "Setup manifest hash mismatch: $relative" }
        $entries[$relative] = $hash
    }
    $actualFiles = @(Get-RelativeFiles $Path | Where-Object { $_ -ne 'SHA256SUMS.txt' })
    if ((Compare-Object $actualFiles (@($entries.Keys) | Sort-Object)).Count -ne 0) { throw 'Setup manifest file set differs from extracted files.' }
    return $entries
}

function Write-AtomicText([string]$Path, [string]$Text) {
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force $directory | Out-Null
    $temp = Join-Path $directory ('.' + [IO.Path]::GetFileName($Path) + '.tmp-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($temp, $Text, (New-Object Text.UTF8Encoding($false)))
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $existing = Get-Item -LiteralPath $Path -Force
            if (Test-Reparse $existing) { throw "Refusing reparse point: $Path" }
            $backup = Join-Path $directory ('.' + [IO.Path]::GetFileName($Path) + '.bak-' + [Guid]::NewGuid().ToString('N'))
            try { [IO.File]::Replace($temp, $Path, $backup, $true) }
            finally { if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force } }
        } else {
            [IO.File]::Move($temp, $Path)
        }
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
    }
}

function Copy-AtomicFile([string]$Source, [string]$Destination) {
    $sourceItem = Get-Item -LiteralPath $Source -Force
    if (-not $sourceItem -or $sourceItem.PSIsContainer -or (Test-Reparse $sourceItem)) { throw "Invalid setup file: $Source" }
    $directory = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force $directory | Out-Null
    $temp = Join-Path $directory ('.' + [IO.Path]::GetFileName($Destination) + '.tmp-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::Copy($Source, $temp, $true)
        if (Test-Path -LiteralPath $Destination -PathType Leaf) {
            $existing = Get-Item -LiteralPath $Destination -Force
            if (Test-Reparse $existing) { throw "Refusing reparse point: $Destination" }
            $backup = Join-Path $directory ('.' + [IO.Path]::GetFileName($Destination) + '.bak-' + [Guid]::NewGuid().ToString('N'))
            try { [IO.File]::Replace($temp, $Destination, $backup, $true) }
            finally { if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Force } }
        } else {
            [IO.File]::Move($temp, $Destination)
        }
    } finally {
        if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force }
    }
}

function Get-PayloadEntries($manifest) {
    @($manifest.Keys | Where-Object { $_ -like 'payload/*' } | ForEach-Object {
        [pscustomobject]@{ Relative = $_.Substring(8); Hash = $manifest[$_] }
    } | Sort-Object Relative)
}

function Get-PayloadDigest($entries) {
    $text = (($entries | ForEach-Object { "$($_.Hash)  $($_.Relative)" }) -join "`n") + "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash([Text.UTF8Encoding]::new($false).GetBytes($text)) | ForEach-Object { $_.ToString('x2') }) -join '') }
    finally { $sha.Dispose() }
}

function Test-OwnedVersion([string]$Path, [string]$Version, [string]$Digest, $ExpectedEntries) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    Assert-SafeTree $Path
    $markerPath = Join-Path $Path '.q2dz-owned'
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { return $false }
    $marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($marker.schema -ne 1 -or $marker.product -ne 'q2prox-dz' -or $marker.version -ne $Version -or $marker.payloadDigest -ne $Digest) { return $false }
    $expected = @($ExpectedEntries | ForEach-Object { $_.Relative })
    $markerHashes = @{}
    foreach ($entry in @($marker.files)) {
        if ([string]$entry.path -notmatch '^[^\\/]+(?:/[^\\/]+)*$' -or [string]$entry.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $markerHashes.ContainsKey([string]$entry.path)) { return $false }
        $markerHashes[[string]$entry.path] = ([string]$entry.sha256).ToLowerInvariant()
    }
    $listed = @($marker.files | ForEach-Object { $_.path })
    if ((Compare-Object $expected ($listed | Sort-Object)).Count -ne 0) { return $false }
    foreach ($entry in $ExpectedEntries) {
        if ($markerHashes[$entry.Relative] -ne $entry.Hash.ToLowerInvariant()) { return $false }
        $file = Join-Path $Path ($entry.Relative -replace '/', '\')
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { return $false }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() -ne $entry.Hash) { return $false }
    }
    $actual = @(Get-RelativeFiles $Path | Where-Object { $_ -ne '.q2dz-owned' })
    return ((Compare-Object $expected $actual).Count -eq 0)
}

function Migrate-LegacyPath {
    $legacy = Join-Path $root 'q2-path.txt'
    $new = Join-Path $state 'q2-path.txt'
    if (-not (Test-Path -LiteralPath $legacy -PathType Leaf)) { return }
    $legacyItem = Get-Item -LiteralPath $legacy -Force
    if (Test-Reparse $legacyItem) { throw 'Refusing legacy q2-path.txt reparse point.' }
    if (Test-Path -LiteralPath $new) {
        $newItem = Get-Item -LiteralPath $new -Force
        if (Test-Reparse $newItem) { throw 'Refusing state/q2-path.txt reparse point.' }
        return
    }
    New-Item -ItemType Directory -Force $state | Out-Null
    [IO.File]::Copy($legacy, $new)
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $legacy).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $new).Hash) { throw 'Legacy q2-path migration verification failed.' }
    Remove-Item -LiteralPath $legacy -Force
}

Assert-SafeTree $setup
Assert-SafeDirectoryPath $startMenu
if (-not (Test-Path -LiteralPath $env:LOCALAPPDATA -PathType Container)) { New-Item -ItemType Directory -Force $env:LOCALAPPDATA | Out-Null }
if (Test-Path -LiteralPath $root) { Assert-SafeTree $root } else { New-Item -ItemType Directory -Force $root | Out-Null }
Migrate-LegacyPath
$manifest = Assert-Manifest $setup
$payloadEntries = Get-PayloadEntries $manifest
if (@($payloadEntries | Where-Object { $_.Relative -eq 'VERSION' }).Count -ne 1) { throw 'Payload VERSION is missing.' }
$version = (Get-Content -LiteralPath (Join-Path $setup 'payload/VERSION') -Raw).Trim()
if ($version -notmatch '^[0-9A-Za-z][0-9A-Za-z.-]*$') { throw 'Invalid payload VERSION.' }
$digest = Get-PayloadDigest $payloadEntries
$finalName = "$version-$digest"
$final = Join-Path $versions $finalName
New-Item -ItemType Directory -Force $versions | Out-Null
Assert-SafeTree $versions
$stage = Join-Path $versions ('.staging-' + [Guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force $stage | Out-Null
    foreach ($entry in $payloadEntries) {
        $source = Join-Path $setup ('payload/' + $entry.Relative -replace '/', '\')
        $destination = Join-Path $stage ($entry.Relative -replace '/', '\')
        Copy-AtomicFile $source $destination
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash.ToLowerInvariant() -ne $entry.Hash) { throw "Copied payload hash mismatch: $($entry.Relative)" }
    }
    $marker = [ordered]@{
        schema = 1; product = 'q2prox-dz'; version = $version; payloadDigest = $digest
        files = @($payloadEntries | ForEach-Object { [ordered]@{ path = $_.Relative; sha256 = $_.Hash } })
    }
    Write-AtomicText (Join-Path $stage '.q2dz-owned') ($marker | ConvertTo-Json -Depth 5)
    if ($env:Q2DZ_TEST_INTERRUPT_AFTER_STAGE -eq '1') { throw 'TEST_INTERRUPT_AFTER_STAGE' }
    if (Test-Path -LiteralPath $final) {
        if (-not (Test-OwnedVersion $final $version $digest $payloadEntries)) { throw "Existing release directory is not a complete owned version: $finalName" }
        Remove-Item -LiteralPath $stage -Recurse -Force
    } else {
        [IO.Directory]::Move($stage, $final)
    }
    foreach ($stable in @(
        @{ Source = 'launch-installed.ps1'; Destination = 'launch.ps1' },
        @{ Source = 'uninstall.ps1'; Destination = 'uninstall.ps1' }
    )) {
        $destination = Join-Path $root $stable.Destination
        Copy-AtomicFile (Join-Path $setup $stable.Source) $destination
        $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash.ToLowerInvariant()
        if ($actual -ne $manifest[$stable.Source]) { throw "Copied stable script hash mismatch: $($stable.Source)" }
    }
    New-Item -ItemType Directory -Force $startMenu | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $shortcutSpecs = @(
        @{ Name = 'Q2PRO-X - Dos.Zone.lnk'; Target = (Join-Path $root 'launch.ps1'); Legacy = (Join-Path $root 'app\q2dz-launch.ps1'); Work = $root },
        @{ Name = 'Uninstall Q2PRO-X - Dos.Zone.lnk'; Target = (Join-Path $root 'uninstall.ps1'); Legacy = (Join-Path $root 'app\uninstall.ps1'); Work = $env:TEMP }
    )
    foreach ($spec in $shortcutSpecs) {
        $link = Join-Path $startMenu $spec.Name
        Assert-OwnedShortcut $link $spec.Target $spec.Legacy
    }
    foreach ($spec in $shortcutSpecs) {
        $link = Join-Path $startMenu $spec.Name
        if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
        $shortcut = $shell.CreateShortcut($link)
        $shortcut.TargetPath = Get-PowerShellPath
        $shortcut.Arguments = Get-ShortcutArguments $spec.Target
        $shortcut.WorkingDirectory = $spec.Work
        $shortcut.Save()
    }
    $pointer = [ordered]@{ schema = 1; product = 'q2prox-dz'; version = $version; directory = "versions/$finalName"; payloadDigest = $digest }
    Write-AtomicText (Join-Path $root 'current.json') ($pointer | ConvertTo-Json -Depth 4)
    Write-Host "Installed Q2PRO-X - Dos.Zone $version for the current user."
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
