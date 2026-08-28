$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$app = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $app 'catalog.ps1')
$state = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone\state'
$q2PathFile = Join-Path $state 'q2-path.txt'
$coordinator = Join-Path $app 'q2dz-session.exe'
$contentCheck = Join-Path $app 'q2dz-content-check.exe'
$gateway = Join-Path $app 'gateway\q2-doszone-webview2-gateway.exe'
$hubMaxContentChecks = 8

function Quote-Q2DzArgument([string]$Value) {
  if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
  $builder = New-Object System.Text.StringBuilder
  [void]$builder.Append('"')
  $slashes = 0
  foreach ($character in $Value.ToCharArray()) {
    if ($character -eq '\') { $slashes++; continue }
    if ($character -eq '"') {
      if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2)) -join '') }
      [void]$builder.Append('\')
      [void]$builder.Append('"')
      $slashes = 0
      continue
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes) -join '') ; $slashes = 0 }
    [void]$builder.Append($character)
  }
  if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2)) -join '') }
  [void]$builder.Append('"')
  return $builder.ToString()
}

function Invoke-Q2DzCoordinator {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string[]]$Arguments,
    [Parameter(Mandatory = $true)][byte[]]$InputBytes
  )
  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = $Path
  $startInfo.Arguments = (($Arguments | ForEach-Object { Quote-Q2DzArgument $_ }) -join ' ')
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true
  $startInfo.RedirectStandardInput = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $startInfo
  try {
    if (-not $process.Start()) { throw 'Could not start q2dz-session.exe.' }
    $stream = $process.StandardInput.BaseStream
    try {
      $stream.Write($InputBytes, 0, $InputBytes.Length)
      $stream.Flush()
    } finally {
      $stream.Close()
    }
    $process.WaitForExit()
    return $process.ExitCode
  } finally {
    $process.Dispose()
  }
}

foreach ($path in $coordinator,$contentCheck,$gateway) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Installation is incomplete: $path" }
}

if (Get-Process 'Q2PRO-X' -ErrorAction SilentlyContinue) {
  [Windows.Forms.MessageBox]::Show('Close the running Q2PRO-X before joining a Dos.Zone room.','Q2PRO-X - Dos.Zone') | Out-Null
  exit 4
}

$q2 = if (Test-Path $q2PathFile) { (Get-Content $q2PathFile -Raw).Trim() } else { '' }
if (-not $q2 -or -not (Test-Path -LiteralPath $q2 -PathType Leaf)) {
  $picker = New-Object Windows.Forms.OpenFileDialog
  $picker.Title = 'Select your installed Q2PRO-X.exe'
  $picker.Filter = 'Q2PRO-X|Q2PRO-X.exe'
  if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 1 }
  $q2 = $picker.FileName
  New-Item -ItemType Directory -Force $state | Out-Null
  Set-Content -LiteralPath $q2PathFile -Value $q2 -Encoding UTF8
}

$catalog = Get-Q2DzJson $Q2DZ_ROOM_ENDPOINT
$lookup = Get-Q2DzJson $Q2DZ_LOOKUP_ENDPOINT
$rooms = @(Get-Q2DzRegisteredRooms -Catalog $catalog -LookupPayload $lookup)
if ($rooms.Count -eq 0) {
  [Windows.Forms.MessageBox]::Show('There are no joinable Quake II rooms right now.','Q2PRO-X - Dos.Zone') | Out-Null
  exit 0
}

# The launcher has no room picker: Q2PRO-X is the room browser. Keep the
# snapshot bounded and deterministic; the coordinator enforces the same cap.
$configs = @()
$missingMaps = @()
$rtcRows = Get-Q2DzJson $Q2DZ_RTC_ENDPOINT
if ($rtcRows -isnot [array]) { $rtcRows = @($rtcRows) }
$checked = 0
foreach ($candidate in $rooms) {
  if ($configs.Count -ge 2 -or $checked -ge $hubMaxContentChecks) { break }
  $checked++
  try {
    Test-Q2DzSelectedRoom -Room $candidate -LookupPayload $lookup -RtcPayload $rtcRows | Out-Null
    $alias = 'q2.{0}' -f ([string]$candidate.owner)
    $rtc = @($rtcRows | Where-Object { $_.domain -eq $candidate.rtcServer })[0]
    $map = [string]$candidate.createSettings.map
    & $contentCheck --root (Split-Path -Parent $q2) --game $Q2DZ_GAME --map $map | Out-Null
    if ($LASTEXITCODE -ne 0) { $missingMaps += $map; continue }
    $configs += (@{
      version = 1; roomId = $candidate.room; alias = $alias; ordered = $Q2DZ_ORDERED
      iceServers = $rtc.iceservers
    } | ConvertTo-Json -Depth 8 -Compress)
  } catch {
    continue
  }
}
if ($configs.Count -eq 0) {
  $detail = if ($missingMaps.Count -gt 0) { " Missing map(s): $($missingMaps -join ', ')." } else { '' }
  [Windows.Forms.MessageBox]::Show("No compatible Dos.Zone room can be started with this Q2PRO-X installation.$detail",'Q2PRO-X - Dos.Zone') | Out-Null
  exit 3
}

$hubInput = $configs -join [Environment]::NewLine
$hubBytes = $null
try {
  $hubBytes = [Text.UTF8Encoding]::new($false).GetBytes($hubInput)
  $code = Invoke-Q2DzCoordinator -Path $coordinator -Arguments @(
    '--gateway', $gateway,
    '--q2', $q2,
    '--session-root', (Join-Path $state 'sessions')
  ) -InputBytes $hubBytes
} finally {
  if ($hubBytes) { [Array]::Clear($hubBytes, 0, $hubBytes.Length) }
  $hubBytes = $null
}
$configs = $null; $rtcRows = $null; $rtc = $null; $hubInput = $null; [GC]::Collect()
exit $code
