[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Split-Path -Parent $MyInvocation.MyCommand.Path)).Path

function Test-Reparse([System.IO.FileSystemInfo]$Item) {
    return (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
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

$rootItem = Get-Item -LiteralPath $root -Force
if (Test-Reparse $rootItem) { throw 'The installed add-on root is a reparse point.' }
$pointerPath = Join-Path $root 'current.json'
if (-not (Test-Path -LiteralPath $pointerPath -PathType Leaf)) { throw 'The Dos.Zone add-on has no active release.' }
$pointerItem = Get-Item -LiteralPath $pointerPath -Force
if (Test-Reparse $pointerItem) { throw 'The installed release pointer is a reparse point.' }
$pointer = Get-Content -LiteralPath $pointerPath -Raw | ConvertFrom-Json
$pointerDigest = [string]$pointer.payloadDigest
$pointerVersion = [string]$pointer.version
if ($pointer.schema -ne 1 -or $pointer.product -ne 'q2prox-dz' -or
    $pointerVersion -notmatch '^[0-9A-Za-z][0-9A-Za-z.-]*$' -or
    $pointerDigest -notmatch '^[0-9a-fA-F]{64}$') {
    throw 'The installed release pointer is invalid.'
}
$expectedDirectory = "versions/$pointerVersion-$pointerDigest"
if ([string]$pointer.directory -cne $expectedDirectory -or [string]$pointer.directory -notmatch '^versions/[0-9A-Za-z][0-9A-Za-z.-]*-[0-9a-fA-F]{64}$') {
    throw 'The installed release pointer directory is invalid.'
}

$versionsRoot = Join-Path $root 'versions'
$versionsItem = Get-Item -LiteralPath $versionsRoot -Force -ErrorAction Stop
if (Test-Reparse $versionsItem -or -not $versionsItem.PSIsContainer) { throw 'The installed versions directory is unsafe.' }
$versionRoot = Join-Path $root ($expectedDirectory -replace '/', '\')
Assert-SafeTree $versionRoot
if (-not (Test-Path -LiteralPath $versionRoot -PathType Container)) { throw 'The active release directory is missing.' }
$markerPath = Join-Path $versionRoot '.q2dz-owned'
if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { throw 'The active release ownership marker is missing.' }
$markerItem = Get-Item -LiteralPath $markerPath -Force
if (Test-Reparse $markerItem) { throw 'The active release ownership marker is a reparse point.' }
$marker = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
if ($marker.schema -ne 1 -or $marker.product -ne 'q2prox-dz' -or
    [string]$marker.version -cne $pointerVersion -or [string]$marker.payloadDigest -cne $pointerDigest) {
    throw 'The active release ownership marker does not match current.json.'
}

$markerHashes = @{}
foreach ($entry in @($marker.files)) {
    $path = [string]$entry.path
    $hash = [string]$entry.sha256
    if ($path -notmatch '^[^\\/]+(?:/[^\\/]+)*$' -or $hash -notmatch '^[0-9a-fA-F]{64}$' -or $markerHashes.ContainsKey($path)) {
        throw 'The active release ownership marker file list is invalid.'
    }
    $markerHashes[$path] = $hash.ToLowerInvariant()
    $file = Join-Path $versionRoot ($path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "The active release file is missing: $path" }
    $fileItem = Get-Item -LiteralPath $file -Force
    if (Test-Reparse $fileItem) { throw "The active release file is a reparse point: $path" }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() -ne $hash.ToLowerInvariant()) {
        throw "The active release file hash is invalid: $path"
    }
}
$actualFiles = @(Get-RelativeFiles $versionRoot | Where-Object { $_ -ne '.q2dz-owned' })
if ((Compare-Object (@($markerHashes.Keys) | Sort-Object) $actualFiles).Count -ne 0) {
    throw 'The active release ownership marker file list does not match the release directory.'
}

$launcherRelative = 'q2dz-launch.ps1'
if (-not $markerHashes.ContainsKey($launcherRelative)) { throw 'The active release launcher is not owned by the marker.' }
$launcher = Join-Path $versionRoot $launcherRelative
$launcherItem = Get-Item -LiteralPath $launcher -Force
if (Test-Reparse $launcherItem) { throw 'The active release launcher is a reparse point.' }
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $launcher).Hash.ToLowerInvariant() -ne $markerHashes[$launcherRelative]) {
    throw 'The active release launcher hash is invalid.'
}
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
& $powershell -NoProfile -ExecutionPolicy Bypass -File $launcher @args
exit $LASTEXITCODE
