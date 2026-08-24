[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PayloadDir,
    [string]$OutputDir = "$PSScriptRoot/../../dist"
)

$ErrorActionPreference = 'Stop'
$product = (Resolve-Path $PSScriptRoot).Path
$repoRoot = (Resolve-Path (Join-Path $product '../..')).Path
$payload = (Resolve-Path $PayloadDir).Path
$allowed = @(
    'VERSION',
    'catalog.ps1',
    'q2dz-content-check.exe',
    'q2dz-launch.ps1',
    'q2dz-session.exe',
    'gateway/WebView2Loader.dll',
    'gateway/q2-doszone-webview2-gateway.exe',
    'gateway/assets/managed-readiness-probe.mjs',
    'gateway/assets/transport.html',
    'gateway/assets/transport_protocol.mjs',
    'gateway/assets/webrtcnet.mjs',
    'gateway/assets/webrtcnet.wasm'
) | Sort-Object
$runtime = @($allowed)
$licenses = Join-Path $product 'licenses'

function Relative-Files([string]$root) {
    @(Get-ChildItem -LiteralPath $root -File -Recurse | ForEach-Object {
        $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    } | Sort-Object)
}

function Copy-Relative([string]$relative, [string]$destination) {
    $target = Join-Path $destination $relative
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $payload $relative) -Destination $target
}

function Write-Manifest([string]$root) {
    $lines = Relative-Files $root | Where-Object { $_ -ne 'SHA256SUMS.txt' } | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 (Join-Path $root $_)).Hash.ToLowerInvariant()
        "$hash  $_"
    }
    [IO.File]::WriteAllLines((Join-Path $root 'SHA256SUMS.txt'), $lines, [Text.UTF8Encoding]::new($false))
}

$actual = Relative-Files $payload
$difference = @(Compare-Object $allowed $actual)
if ($difference.Count -ne 0) {
    throw "Payload does not match the release allowlist:`n$($difference | Out-String)"
}

$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^[0-9A-Za-z][0-9A-Za-z.-]*$') { throw 'Invalid root VERSION.' }
$payloadVersion = (Get-Content -LiteralPath (Join-Path $payload 'VERSION') -Raw).Trim()
if ($payloadVersion -ne $version) { throw 'Payload VERSION does not match root VERSION.' }

$output = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $output | Out-Null
$work = Join-Path ([IO.Path]::GetTempPath()) ("q2dz-package-" + [Guid]::NewGuid().ToString('N'))
$portable = Join-Path $work 'portable'
$setup = Join-Path $work 'setup'

try {
    New-Item -ItemType Directory -Force $portable, (Join-Path $setup 'payload') | Out-Null
    foreach ($relative in $runtime) {
        Copy-Relative $relative $portable
        Copy-Relative $relative (Join-Path $setup 'payload')
    }
    Copy-Item -LiteralPath $licenses -Destination (Join-Path $portable 'licenses') -Recurse
    Copy-Item -LiteralPath $licenses -Destination (Join-Path $setup 'payload/licenses') -Recurse

    Copy-Item -LiteralPath (Join-Path $product 'Run-Portable.cmd') -Destination (Join-Path $portable 'Run.cmd')
    Copy-Item -LiteralPath (Join-Path $product 'README-PORTABLE.txt') -Destination (Join-Path $portable 'README.txt')
    foreach ($name in @('Install.cmd', 'install.ps1', 'uninstall.ps1', 'launch-installed.ps1')) {
        Copy-Item -LiteralPath (Join-Path $product $name) -Destination (Join-Path $setup $name)
    }
    Copy-Item -LiteralPath (Join-Path $product 'README.txt') -Destination (Join-Path $setup 'README.txt')

    Write-Manifest $portable
    Write-Manifest $setup

    $portableZip = Join-Path $output "q2prox-dz-$version-windows-x64-portable.zip"
    $setupZip = Join-Path $output "q2prox-dz-$version-windows-x64-setup.zip"
    Remove-Item -LiteralPath $portableZip, $setupZip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path "$portable/*" -DestinationPath $portableZip -CompressionLevel Optimal
    Compress-Archive -Path "$setup/*" -DestinationPath $setupZip -CompressionLevel Optimal

    foreach ($archive in @($portableZip, $setupZip)) {
        $hash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
        [IO.File]::WriteAllText("$archive.sha256", "$hash  $([IO.Path]::GetFileName($archive))`n", [Text.UTF8Encoding]::new($false))
        Write-Host "$hash  $archive"
    }
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
