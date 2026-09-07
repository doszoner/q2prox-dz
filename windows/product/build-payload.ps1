[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$WebView2Package,
  [Parameter(Mandatory = $true)][string]$WebRtcNetSourceDir,
  [Parameter(Mandatory = $true)][string]$WebRtcNetReleaseDir,
  [string]$OutputDir = "$PSScriptRoot/../../dist"
)

$ErrorActionPreference = 'Stop'

function Resolve-FileSystemPath([string]$Path) {
  $resolved = Resolve-Path -LiteralPath $Path
  if ($resolved.Provider.Name -ne 'FileSystem') { throw "Expected a filesystem path: $Path" }
  return $resolved.ProviderPath
}

$product = Resolve-FileSystemPath $PSScriptRoot
$repoRoot = Resolve-FileSystemPath (Join-Path $product '../..')
$package = Resolve-FileSystemPath $WebView2Package
$webrtcSource = Resolve-FileSystemPath $WebRtcNetSourceDir
$webrtcRelease = Resolve-FileSystemPath $WebRtcNetReleaseDir
$output = [IO.Path]::GetFullPath($OutputDir)
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^[0-9A-Za-z][0-9A-Za-z.-]*$') { throw 'Invalid root VERSION.' }

function Invoke-Checked([string]$File, [object[]]$Arguments) {
  & $File @Arguments
  if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File" }
}

function Get-PythonCommand {
  $launcher = Get-Command py.exe -ErrorAction SilentlyContinue
  if ($launcher) {
    try {
      & $launcher.Source -3 --version *> $null
      if ($LASTEXITCODE -eq 0) { return @($launcher.Source, '-3') }
    } catch {
      # Some Windows images ship py.exe but have no registered Python runtime.
      # Fall back to python.exe instead of making release builds depend on the
      # launcher registry state.
    }
  }
  $python = Get-Command python.exe -ErrorAction SilentlyContinue
  if ($python) {
    try {
      & $python.Source --version *> $null
      if ($LASTEXITCODE -eq 0) { return @($python.Source) }
    } catch {
    }
  }
  throw 'Python 3 is required to verify deps.lock.json.'
}

function Invoke-Python([string[]]$Arguments) {
  # Keep a one-element command array from collapsing to a string; `$python[0]`
  # must be the executable path, not its first character (`C` on Windows).
  $python = @(Get-PythonCommand)
  $launcherArgs = @()
  if ($python.Count -gt 1) { $launcherArgs = @($python[1..($python.Count - 1)]) }
  Invoke-Checked $python[0] @($launcherArgs + $Arguments)
}

function Copy-PayloadFile([string]$Source, [string]$Relative, [string]$Payload) {
  if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Release input is missing: $Source" }
  $destination = Join-Path $Payload ($Relative -replace '/', '\')
  New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
  Copy-Item -LiteralPath $Source -Destination $destination -Force
}

$existing = if (Test-Path -LiteralPath $output) { @(Get-ChildItem -LiteralPath $output -Force) } else { @() }
if ($existing.Count -ne 0) {
  throw "OutputDir must be empty for a release build: $output"
}
New-Item -ItemType Directory -Force $output | Out-Null

$work = Join-Path ([IO.Path]::GetTempPath()) ("q2dz-release-" + [Guid]::NewGuid().ToString('N'))
$succeeded = $false
try {
  New-Item -ItemType Directory -Force $work | Out-Null
  $verifier = Join-Path $repoRoot 'tools/verify-dependencies.py'
  $lock = Join-Path $repoRoot 'deps.lock.json'

  # Verify the immutable package before extracting any SDK files.
  Invoke-Python @($verifier, '--lock', $lock, '--webview2-package', $package)

  $sdk = Join-Path $work 'webview2-sdk'
  # Windows PowerShell 5.1 validates the .zip suffix before opening an archive.
  $packageZip = Join-Path $work 'webview2-package.zip'
  Copy-Item -LiteralPath $package -Destination $packageZip
  Expand-Archive -LiteralPath $packageZip -DestinationPath $sdk -Force
  $loader = Join-Path $sdk 'build/native/x64/WebView2Loader.dll'
  foreach ($required in @(
    (Join-Path $sdk 'build/native/include/WebView2.h'),
    $loader,
    (Join-Path $sdk 'build/native/x64/WebView2Loader.dll.lib')
  )) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Extracted WebView2 SDK is incomplete: $required" }
  }

  Invoke-Python @(
    $verifier, '--lock', $lock, '--webrtc-source', $webrtcSource,
    '--windows-release', $webrtcRelease, '--webview2-package', $package,
    '--webview2-loader', $loader
  )

  $coordinatorBuild = Join-Path $work 'build/coordinator'
  $gatewayBuild = Join-Path $work 'build/gateway'
  $buildRelease = Join-Path $repoRoot 'windows/coordinator/tools/build-release.ps1'
  & $buildRelease -BuildDir $coordinatorBuild -Generator 'Visual Studio 17 2022' -Architecture x64
  if ($LASTEXITCODE -ne 0) { throw "Coordinator build failed ($LASTEXITCODE)." }
  $lifecycleMatrix = Join-Path $repoRoot 'windows/coordinator/tools/run-lifecycle-matrix.ps1'
  & $lifecycleMatrix -BuildDir (Join-Path $coordinatorBuild 'Release')

  Invoke-Checked 'cmake' @(
    '-S', (Join-Path $repoRoot 'windows/gateway'), '-B', $gatewayBuild,
    '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DBUILD_TESTING=ON', "-DWEBVIEW2_SDK_DIR=$sdk", "-DWEBRTC_NET_RELEASE_DIR=$webrtcRelease"
  )
  Invoke-Checked 'cmake' @('--build', $gatewayBuild, '--config', 'Release')
  Invoke-Checked 'ctest' @('--test-dir', $gatewayBuild, '-C', 'Release', '--output-on-failure')

  $node = Get-Command node.exe -ErrorAction SilentlyContinue
  if (-not $node) { throw 'Node.js is required for the Windows release contract tests.' }
  $previousWebRtcSource = $env:WEBRTC_NET_SOURCE_DIR
  try {
    $env:WEBRTC_NET_SOURCE_DIR = $webrtcSource
    Invoke-Checked $node.Source @('--test', (Join-Path $repoRoot 'windows/gateway/tests/transport_protocol.test.mjs'))
  } finally {
    if ($null -eq $previousWebRtcSource) { Remove-Item Env:WEBRTC_NET_SOURCE_DIR -ErrorAction SilentlyContinue }
    else { $env:WEBRTC_NET_SOURCE_DIR = $previousWebRtcSource }
  }

  $catalogContract = Join-Path $product 'tests/catalog.tests.ps1'
  & $catalogContract
  if ($LASTEXITCODE -ne 0) { throw "Catalog contract tests failed ($LASTEXITCODE)." }

  $installContract = Join-Path $product 'tests/install-windows.tests.ps1'
  & $installContract
  if ($LASTEXITCODE -ne 0) { throw "Install contract tests failed ($LASTEXITCODE)." }

  $launcherStdinContract = Join-Path $product 'tests/launcher-stdin.tests.ps1'
  & $launcherStdinContract
  if ($LASTEXITCODE -ne 0) { throw "Launcher stdin contract tests failed ($LASTEXITCODE)." }

  $repro = Join-Path $repoRoot 'windows/coordinator/tools/check-reproducible.ps1'
  & $repro -Out (Join-Path $work 'repro') -SourceRoot $repoRoot -WebView2SdkDir $sdk -WebRtcNetReleaseDir $webrtcRelease
  if ($LASTEXITCODE -ne 0) { throw "Full-candidate /Brepro check failed ($LASTEXITCODE)." }

  $payload = Join-Path $work 'payload'
  New-Item -ItemType Directory -Force $payload | Out-Null
  Copy-PayloadFile (Join-Path $repoRoot 'VERSION') 'VERSION' $payload
  Copy-PayloadFile (Join-Path $product 'catalog.ps1') 'catalog.ps1' $payload
  Copy-PayloadFile (Join-Path $coordinatorBuild 'Release/q2dz-content-check.exe') 'q2dz-content-check.exe' $payload
  Copy-PayloadFile (Join-Path $coordinatorBuild 'Release/q2dz-session.exe') 'q2dz-session.exe' $payload
  Copy-PayloadFile (Join-Path $product 'q2dz-launch.ps1') 'q2dz-launch.ps1' $payload
  Copy-PayloadFile (Join-Path $gatewayBuild 'Release/q2-doszone-webview2-gateway.exe') 'gateway/q2-doszone-webview2-gateway.exe' $payload
  Copy-PayloadFile (Join-Path $gatewayBuild 'Release/WebView2Loader.dll') 'gateway/WebView2Loader.dll' $payload
  foreach ($asset in @('managed-readiness-probe.mjs', 'transport.html', 'transport_protocol.mjs', 'webrtcnet.mjs', 'webrtcnet.wasm')) {
    Copy-PayloadFile (Join-Path $gatewayBuild ("Release/assets/$asset")) ("gateway/assets/$asset") $payload
  }

  $packager = Join-Path $product 'package-windows.ps1'
  & $packager -PayloadDir $payload -OutputDir $output
  if ($LASTEXITCODE -ne 0) { throw "Windows package failed ($LASTEXITCODE)." }

  $portableZip = Join-Path $output "q2prox-dz-$version-windows-x64-portable.zip"
  $setupZip = Join-Path $output "q2prox-dz-$version-windows-x64-setup.zip"
  $smoke = Join-Path $product 'tests/release-smoke.tests.ps1'
  & $smoke -PortableZip $portableZip -SetupZip $setupZip -ExpectedVersion $version -RunNegativeTests
  if ($LASTEXITCODE -ne 0) { throw "Release smoke failed ($LASTEXITCODE)." }

  $expected = @(
    "$([IO.Path]::GetFileName($portableZip))",
    "$([IO.Path]::GetFileName($portableZip)).sha256",
    "$([IO.Path]::GetFileName($setupZip))",
    "$([IO.Path]::GetFileName($setupZip)).sha256"
  ) | Sort-Object
  $actual = @(Get-ChildItem -LiteralPath $output -File | ForEach-Object { $_.Name } | Sort-Object)
  $difference = @(Compare-Object $expected $actual)
  if ($difference.Count -ne 0) { throw "OutputDir contains files other than the two release ZIPs and hashes." }
  Write-Host "WINDOWS RELEASE PASS: $output"
  $succeeded = $true
} finally {
  if ($succeeded -and (Test-Path -LiteralPath $work)) {
    Remove-Item -LiteralPath $work -Recurse -Force
  } else {
    Write-Warning "Release work directory retained for diagnosis: $work"
  }
}
