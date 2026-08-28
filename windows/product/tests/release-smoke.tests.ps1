[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$PortableZip,
  [Parameter(Mandatory = $true)][string]$SetupZip,
  [Parameter(Mandatory = $true)][string]$ExpectedVersion,
  [switch]$RunNegativeTests
)

$ErrorActionPreference = 'Stop'
$product = (Resolve-Path "$PSScriptRoot/..").Path
$licenses = @(Get-ChildItem -LiteralPath (Join-Path $product 'licenses') -File | ForEach-Object { "licenses/$($_.Name)" } | Sort-Object)
$runtime = @(
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

function Assert([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Relative-Files([string]$Root) {
  @(Get-ChildItem -LiteralPath $Root -File -Recurse | ForEach-Object {
    $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
  } | Sort-Object)
}

function Write-Manifest([string]$Root) {
  $lines = @(Relative-Files $Root | Where-Object { $_ -ne 'SHA256SUMS.txt' } | ForEach-Object {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Root ($_ -replace '/', '\'))).Hash.ToLowerInvariant()
    "$hash  $_"
  })
  [IO.File]::WriteAllLines((Join-Path $Root 'SHA256SUMS.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}

function Test-Manifest([string]$Root) {
  $manifest = Join-Path $Root 'SHA256SUMS.txt'
  Assert (Test-Path -LiteralPath $manifest -PathType Leaf) 'SHA256SUMS.txt is missing'
  $listed = @()
  foreach ($line in @(Get-Content -LiteralPath $manifest)) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  (.+)$') { throw "Invalid manifest line." }
    $hash = $Matches[1].ToLowerInvariant()
    $relative = $Matches[2]
    if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or $relative -eq 'SHA256SUMS.txt') { throw 'Unsafe manifest path.' }
    $path = Join-Path $Root ($relative -replace '/', '\')
    Assert (Test-Path -LiteralPath $path -PathType Leaf) "Manifest file is missing: $relative"
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    Assert ($actual -eq $hash) "Manifest hash mismatch: $relative"
    $listed += $relative
  }
  $expected = @(Relative-Files $Root | Where-Object { $_ -ne 'SHA256SUMS.txt' })
  Assert ((Compare-Object $expected ($listed | Sort-Object)).Count -eq 0) 'Manifest file set differs from extracted files.'
}

function Test-PE([string]$Path) {
  $bytes = [IO.File]::ReadAllBytes($Path)
  Assert ($bytes.Length -ge 64) "PE file is too short: $Path"
  Assert ($bytes[0] -eq 0x4d -and $bytes[1] -eq 0x5a) "PE DOS signature is missing: $Path"
  $offset = [BitConverter]::ToInt32($bytes, 0x3c)
  Assert ($offset -ge 64 -and $offset + 4 -le $bytes.Length) "PE header offset is invalid: $Path"
  Assert ($bytes[$offset] -eq 0x50 -and $bytes[$offset + 1] -eq 0x45 -and $bytes[$offset + 2] -eq 0 -and $bytes[$offset + 3] -eq 0) "PE signature is missing: $Path"
}

function Test-ZipSidecar([string]$Zip) {
  $sidecar = "$Zip.sha256"
  Assert (Test-Path -LiteralPath $sidecar -PathType Leaf) "ZIP hash is missing: $sidecar"
  $line = (Get-Content -LiteralPath $sidecar -Raw).Trim()
  $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash.ToLowerInvariant()
  Assert ($line -eq "$actual  $([IO.Path]::GetFileName($Zip))") "ZIP hash sidecar mismatch: $Zip"
}

function Test-Zip([string]$Zip, [string]$Kind, [string]$Version, [string]$Root, [switch]$Setup) {
  Test-ZipSidecar $Zip
  $extract = Join-Path $Root $Kind
  Expand-Archive -LiteralPath $Zip -DestinationPath $extract -Force
  $expected = if (-not $Setup) {
    @($runtime + $licenses + @('README.txt', 'Run.cmd', 'SHA256SUMS.txt') | Sort-Object)
  } else {
    @(
      @($runtime + $licenses | ForEach-Object { "payload/$_" })
      'Install.cmd'
      'README.txt'
      'SHA256SUMS.txt'
      'install.ps1'
      'uninstall.ps1'
      'launch-installed.ps1'
    ) | Sort-Object
  }
  Assert ((Compare-Object $expected (Relative-Files $extract)).Count -eq 0) "$Kind layout differs"
  $versionPath = if (-not $Setup) { Join-Path $extract 'VERSION' } else { Join-Path $extract 'payload/VERSION' }
  Assert ((Get-Content -LiteralPath $versionPath -Raw).Trim() -eq $Version) "$Kind version differs"
  Test-Manifest $extract
  $prefix = if (-not $Setup) { $extract } else { Join-Path $extract 'payload' }
  foreach ($relative in @('q2dz-session.exe', 'q2dz-content-check.exe', 'gateway/WebView2Loader.dll', 'gateway/q2-doszone-webview2-gateway.exe')) {
    Test-PE (Join-Path $prefix ($relative -replace '/', '\'))
  }
  foreach ($relative in @('gateway/assets/webrtcnet.mjs', 'gateway/assets/webrtcnet.wasm')) {
    $path = Join-Path $prefix ($relative -replace '/', '\')
    Assert ((Get-Item -LiteralPath $path).Length -gt 0) "WebRTC asset is empty: $relative"
  }
  return $extract
}

function New-Zip([string]$Directory, [string]$Zip) {
  Compress-Archive -Path (Join-Path $Directory '*') -DestinationPath $Zip -CompressionLevel Optimal -Force
  $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash.ToLowerInvariant()
  [IO.File]::WriteAllText("$Zip.sha256", "$hash  $([IO.Path]::GetFileName($Zip))`n", (New-Object Text.UTF8Encoding($false)))
}

function Expect-Failure([string]$Name, [scriptblock]$Action) {
  $failed = $false
  try { & $Action } catch { $failed = $true }
  Assert $failed "$Name was accepted"
  Write-Host "negative smoke: $Name rejected"
}

function Test-LauncherModes([string]$Root) {
  $launcher = Get-Content -LiteralPath (Join-Path $Root 'q2dz-launch.ps1') -Raw
  Assert ($launcher.Contains('The launcher has no room picker')) 'packaged launcher does not start the Custom browser directly'
  Assert (-not $launcher.Contains('ListBox')) 'packaged launcher contains a duplicate room chooser'
  Assert (-not $launcher.Contains('--hub')) 'packaged launcher passes an obsolete coordinator mode flag'
  Assert ($launcher.Contains("'--gateway', " + '$gateway')) 'packaged launcher does not invoke the hub coordinator'
  Assert ($launcher.Contains('$rooms')) 'packaged launcher does not build a room snapshot'
  Assert ($launcher.Contains('$hubInput')) 'packaged launcher does not pipe snapshot to coordinator'
  Assert ($launcher.Contains('[Environment]::NewLine')) 'packaged launcher does not delimit hub room configurations'
  Assert ($launcher.Contains('RedirectStandardInput')) 'packaged launcher does not use coordinator stdin'
  Assert ($launcher.Contains('StandardInput.BaseStream')) 'packaged launcher does not write exact stdin bytes'
  Assert ($launcher.Contains('UTF8Encoding')) 'packaged launcher does not encode stdin as UTF-8'
}

$portable = (Resolve-Path $PortableZip).Path
$setup = (Resolve-Path $SetupZip).Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("q2dz-smoke-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
try {
  $portableRoot = Test-Zip $portable 'portable' $ExpectedVersion $work
  [void](Test-Zip $setup 'setup' $ExpectedVersion $work -Setup)
  Test-LauncherModes $portableRoot

  if ($RunNegativeTests) {
    $fixture = Join-Path $work 'fixture-text'
    Copy-Item -LiteralPath $portableRoot -Destination $fixture -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $fixture 'q2dz-session.exe'), 'fixture text', (New-Object Text.UTF8Encoding($false)))
    Write-Manifest $fixture
    $fixtureZip = Join-Path $work 'fixture-text.zip'
    New-Zip $fixture $fixtureZip
    Expect-Failure 'fixture text named .exe' { Test-Zip $fixtureZip 'fixture-text-extract' $ExpectedVersion $work }

    $altered = Join-Path $work 'altered-byte'
    Copy-Item -LiteralPath $portableRoot -Destination $altered -Recurse -Force
    $alteredBytes = [IO.File]::ReadAllBytes((Join-Path $altered 'q2dz-session.exe'))
    # Keep both PE signatures intact; the package manifest must catch the mutation.
    $alterOffset = 64
    Assert ($alteredBytes.Length -gt $alterOffset) 'PE fixture is too short for provenance mutation'
    $alteredBytes[$alterOffset] = [byte]($alteredBytes[$alterOffset] -bxor 1)
    [IO.File]::WriteAllBytes((Join-Path $altered 'q2dz-session.exe'), $alteredBytes)
    $alteredZip = Join-Path $work 'altered-byte.zip'
    New-Zip $altered $alteredZip
    Expect-Failure 'altered payload byte' { Test-Zip $alteredZip 'altered-byte-extract' $ExpectedVersion $work }

    foreach ($missing in @('gateway/WebView2Loader.dll', 'gateway/assets/webrtcnet.wasm')) {
      $missingName = if ($missing -like '*WebView2*') { 'missing-webview2' } else { 'missing-webrtc' }
      $missingRoot = Join-Path $work $missingName
      Copy-Item -LiteralPath $portableRoot -Destination $missingRoot -Recurse -Force
      Remove-Item -LiteralPath (Join-Path $missingRoot ($missing -replace '/', '\')) -Force
      Write-Manifest $missingRoot
      $missingZip = Join-Path $work ((Split-Path $missingRoot -Leaf) + '.zip')
      New-Zip $missingRoot $missingZip
      Expect-Failure "missing $missing" { Test-Zip $missingZip (Split-Path $missingRoot -Leaf) $ExpectedVersion $work }
    }

    $wrongVersion = Join-Path $work 'wrong-version'
    Copy-Item -LiteralPath $portableRoot -Destination $wrongVersion -Recurse -Force
    [IO.File]::WriteAllText((Join-Path $wrongVersion 'VERSION'), '0.0.0', (New-Object Text.UTF8Encoding($false)))
    Write-Manifest $wrongVersion
    $wrongZip = Join-Path $work 'wrong-version.zip'
    New-Zip $wrongVersion $wrongZip
    Expect-Failure 'mismatched VERSION' { Test-Zip $wrongZip 'wrong-version-extract' $ExpectedVersion $work }
  }
  Write-Host 'release real-payload smoke: PASS'
} finally {
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
