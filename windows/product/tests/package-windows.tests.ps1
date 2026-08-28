[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$product = (Resolve-Path "$PSScriptRoot/..").Path
$repoRoot = (Resolve-Path (Join-Path $product '../..')).Path
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'VERSION') -Raw).Trim()
$packager = Join-Path $product 'package-windows.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("q2dz-package-test-" + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $work 'payload'
$output = Join-Path $work 'output'
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
)
$runtime = @($allowed)
$licenseFiles = @(Get-ChildItem -LiteralPath (Join-Path $product 'licenses') -File | ForEach-Object {
    "licenses/$($_.Name)"
} | Sort-Object)

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERTION FAILED: $message" }
}

function Relative-Files([string]$root) {
    @(Get-ChildItem -LiteralPath $root -File -Recurse | ForEach-Object {
        $_.FullName.Substring($root.Length + 1).Replace('\', '/')
    } | Sort-Object)
}

$releaseBuilder = Get-Content -LiteralPath (Join-Path $product 'build-payload.ps1') -Raw
foreach ($required in @(
    @("Join-Path `$product 'tests/catalog.tests.ps1'", 'Catalog contract tests failed ($LASTEXITCODE).'),
    @("Join-Path `$product 'tests/install-windows.tests.ps1'", 'Install contract tests failed ($LASTEXITCODE).'),
    @("Join-Path `$product 'tests/launcher-stdin.tests.ps1'", 'Launcher stdin contract tests failed ($LASTEXITCODE).')
)) {
    Assert ($releaseBuilder.Contains($required[0])) "release chain does not invoke $($required[0])"
    Assert ($releaseBuilder.Contains($required[1])) "release chain does not fail closed for $($required[0])"
}

try {
    foreach ($relative in $allowed) {
        $path = Join-Path $payload $relative
        New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, "fixture:$relative", [Text.UTF8Encoding]::new($false))
    }
    [IO.File]::WriteAllText((Join-Path $payload 'VERSION'), $version, [Text.UTF8Encoding]::new($false))

    & $packager -PayloadDir $payload -OutputDir $output

    $portableZip = Join-Path $output "q2prox-dz-$version-windows-x64-portable.zip"
    $setupZip = Join-Path $output "q2prox-dz-$version-windows-x64-setup.zip"
    Assert (Test-Path -LiteralPath $portableZip -PathType Leaf) 'portable ZIP is missing'
    Assert (Test-Path -LiteralPath $setupZip -PathType Leaf) 'setup ZIP is missing'

    $portable = Join-Path $work 'portable'
    $setup = Join-Path $work 'setup'
    Expand-Archive -LiteralPath $portableZip -DestinationPath $portable
    Expand-Archive -LiteralPath $setupZip -DestinationPath $setup

    $shipped = @($runtime + $licenseFiles | Sort-Object)
    $portableExpected = @($shipped + @('README.txt', 'Run.cmd', 'SHA256SUMS.txt') | Sort-Object)
    $setupExpected = @($shipped | ForEach-Object { "payload/$_" }) + @(
        'Install.cmd', 'README.txt', 'SHA256SUMS.txt', 'install.ps1', 'uninstall.ps1', 'launch-installed.ps1'
    ) | Sort-Object
    Assert ((Compare-Object $portableExpected (Relative-Files $portable)).Count -eq 0) 'portable layout differs'
    Assert ((Compare-Object $setupExpected (Relative-Files $setup)).Count -eq 0) 'setup layout differs'

    foreach ($relative in $shipped) {
        $portableHash = (Get-FileHash -Algorithm SHA256 (Join-Path $portable $relative)).Hash
        $setupHash = (Get-FileHash -Algorithm SHA256 (Join-Path $setup "payload/$relative")).Hash
        Assert ($portableHash -eq $setupHash) "payload differs: $relative"
    }

    [IO.File]::WriteAllText((Join-Path $payload 'unexpected.log'), 'must not ship')
    $rejected = $false
    try { & $packager -PayloadDir $payload -OutputDir (Join-Path $work 'rejected') }
    catch { $rejected = $true }
    Assert $rejected 'unexpected payload file was accepted'

    Write-Host 'windows dual-package contract: PASS'
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
