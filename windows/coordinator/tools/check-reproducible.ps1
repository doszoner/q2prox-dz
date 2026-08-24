[CmdletBinding()]
param(
  [string]$Out = "$PSScriptRoot/../out/repro",
  [Parameter(Mandatory = $true)][string]$WebView2SdkDir,
  [Parameter(Mandatory = $true)][string]$WebRtcNetReleaseDir,
  [string]$SourceRoot = ""
)
$ErrorActionPreference = 'Stop'
$repoRoot = if ($SourceRoot) { (Resolve-Path $SourceRoot).Path } else { (Resolve-Path "$PSScriptRoot/../../..").Path }
$coordinatorRoot = Join-Path $repoRoot 'windows/coordinator'
$gatewayRoot = Join-Path $repoRoot 'windows/gateway'
$sdk = (Resolve-Path $WebView2SdkDir).Path
$webrtc = (Resolve-Path $WebRtcNetReleaseDir).Path

function Invoke-Checked([string]$File, [object[]]$Arguments) {
  & $File @Arguments
  if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File" }
}

foreach ($required in @(
  (Join-Path $sdk 'build/native/include/WebView2.h'),
  (Join-Path $sdk 'build/native/x64/WebView2Loader.dll'),
  (Join-Path $sdk 'build/native/x64/WebView2Loader.dll.lib'),
  (Join-Path $webrtc 'webrtcnet.mjs'),
  (Join-Path $webrtc 'webrtcnet.wasm')
)) {
  if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing reproducibility input: $required" }
}
Remove-Item -Recurse -Force $Out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Out | Out-Null
$artifacts = @(
  'coordinator/Release/q2dz-session.exe',
  'coordinator/Release/q2dz-content-check.exe',
  'gateway/Release/q2-doszone-webview2-gateway.exe',
  'gateway/Release/WebView2Loader.dll'
)
$hashes = @{}
foreach ($n in 1, 2) {
  $buildRoot = Join-Path $Out "build$n"
  $coordinatorBuild = Join-Path $buildRoot 'coordinator'
  $gatewayBuild = Join-Path $buildRoot 'gateway'
  Invoke-Checked 'cmake' @(
    '-S', $coordinatorRoot, '-B', $coordinatorBuild,
    '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DQ2DZ_BUILD_TESTS=OFF'
  )
  Invoke-Checked 'cmake' @('--build', $coordinatorBuild, '--config', 'Release')
  Invoke-Checked 'cmake' @(
    '-S', $gatewayRoot, '-B', $gatewayBuild,
    '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DBUILD_TESTING=OFF',
    "-DWEBVIEW2_SDK_DIR=$sdk", "-DWEBRTC_NET_RELEASE_DIR=$webrtc"
  )
  Invoke-Checked 'cmake' @('--build', $gatewayBuild, '--config', 'Release')
  foreach ($relative in $artifacts) {
    $binary = Join-Path $buildRoot ($relative -replace '/', '\')
    if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) { throw "Reproducibility artifact missing: $binary" }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $binary).Hash.ToLowerInvariant()
    if (-not $hashes.ContainsKey($relative)) { $hashes[$relative] = @() }
    $hashes[$relative] += $hash
  }
}
$lines = @()
foreach ($relative in $artifacts) {
  $pair = $hashes[$relative]
  $lines += "$relative`nbuild1=$($pair[0])`nbuild2=$($pair[1])"
  if ($pair[0] -ne $pair[1]) { throw "/Brepro gate failed: $relative hashes differ" }
}
$lines | Set-Content (Join-Path $Out 'SHA256.txt') -Encoding ascii
Write-Host 'BREPRO PASS: coordinator and gateway exact binaries match'
