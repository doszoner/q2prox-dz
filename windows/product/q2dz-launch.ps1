$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type -AssemblyName System.Drawing
}

$app = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $app 'catalog.ps1')
$HUB_POLL_SECONDS = 15.0

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

function Start-Q2DzCoordinator {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string[]]$Arguments = @()
  )
  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = $Path
  $startInfo.Arguments = (($Arguments | ForEach-Object { Quote-Q2DzArgument $_ }) -join ' ')
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true
  $startInfo.RedirectStandardInput = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $startInfo
  $started = $false
  try {
    if (-not $process.Start()) { throw 'Could not start q2dz-session.exe.' }
    $started = $true
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $writer = New-Object System.IO.StreamWriter($process.StandardInput.BaseStream, $encoding)
    $writer.NewLine = "`n"
    return [pscustomobject]@{ Process = $process; Writer = $writer }
  } catch {
    if ($started -and -not $process.HasExited) { $process.Kill() }
    $process.Dispose()
    throw
  }
}

function Wait-Q2DzCoordinator {
  param(
    [Parameter(Mandatory = $true)]$Process,
    [int]$TimeoutMilliseconds = 10000
  )
  if ($Process.HasExited) { return [int]$Process.ExitCode }
  if ($Process.WaitForExit($TimeoutMilliseconds)) { return [int]$Process.ExitCode }
  return $null
}

function Write-Q2DzHubSlots {
  param(
    [Parameter(Mandatory = $true)]$Process,
    [Parameter(Mandatory = $true)]$Writer,
    [Parameter(Mandatory = $true)][object[]]$Slots
  )
  if ($Slots.Count -ne 2) { throw 'hub requires exactly two slots' }
  try {
    for ($index = 0; $index -lt $Slots.Count; $index++) {
      $Writer.Write((ConvertTo-Q2DzHubCommand $index $Slots[$index]))
    }
    $Writer.Flush()
    if ($Process.HasExited) { return [int]$Process.ExitCode }
    return $null
  } catch {
    $exception = $_.Exception
    while ($exception.InnerException) { $exception = $exception.InnerException }
    $closedPipe = $exception -is [System.IO.IOException] -or
      $exception -is [System.ObjectDisposedException] -or
      ($exception -is [System.InvalidOperationException] -and $Process.HasExited)
    if (-not $closedPipe) { throw }
    $code = Wait-Q2DzCoordinator -Process $Process
    if ($null -eq $code) { throw 'coordinator closed stdin but did not exit' }
    return $code
  }
}

function Get-Q2DzMonotonicSeconds {
  return [Diagnostics.Stopwatch]::GetTimestamp() / [double][Diagnostics.Stopwatch]::Frequency
}

function New-Q2DzRoomConfig {
  param(
    [Parameter(Mandatory = $true)]$Room,
    [Parameter(Mandatory = $true)]$LookupPayload,
    [Parameter(Mandatory = $true)]$RtcPayload
  )
  if (-not (Test-Q2DzRoomRow $Room) -or -not $Room.roomReady -or
      $Room.players -ge $Room.createSettings.maxPlayers) {
    throw 'The selected room is no longer available'
  }
  if (-not (Test-Q2DzAliasPayload $LookupPayload)) { throw 'alias lookup has an invalid schema' }
  $alias = 'q2.{0}' -f ([string]$Room.owner)
  if ($LookupPayload.aliases -cnotcontains $alias) { throw 'selected room alias is no longer registered' }
  if (-not (Test-Q2DzRtcPayload $RtcPayload)) { throw 'RTC configuration has an invalid schema' }
  $matches = @($RtcPayload | Where-Object { $_.domain -ceq $Room.rtcServer })
  if ($matches.Count -ne 1) { throw 'fresh RTC configuration is unavailable' }
  $config = [ordered]@{
    version = 1
    roomId = $Room.room
    alias = $alias
    ordered = $Q2DZ_ORDERED
    iceServers = $matches[0].iceservers
    map = $Room.createSettings.map
    game = $Q2DZ_GAME
  }
  return ($config | ConvertTo-Json -Depth 8 -Compress)
}

function Get-Q2DzLiveHubSlots {
  param(
    [Parameter(Mandatory = $true)][object[]]$Slots,
    [Parameter(Mandatory = $true)][string]$Q2Path,
    [Parameter(Mandatory = $true)][string]$ContentCheckPath,
    [scriptblock]$FetchJson = { param($Endpoint) Get-Q2DzJson $Endpoint },
    [scriptblock]$RunContentCheck = $null
  )
  $catalog = & $FetchJson $Q2DZ_ROOM_ENDPOINT
  $lookup = & $FetchJson $Q2DZ_LOOKUP_ENDPOINT
  $readyRows = @(Get-Q2DzReadyRooms $catalog)
  $registeredReady = @(Select-Q2DzRegisteredRooms -Rows $readyRows -LookupPayload $lookup)
  $rtcState = [pscustomobject]@{ Fetched = $false; Rows = $null }
  $check = if ($RunContentCheck) { $RunContentCheck } else {
    {
      param($Path, $Map, $Root)
      & $Path --root $Root --game $Q2DZ_GAME --map $Map | Out-Null
      return ($LASTEXITCODE -eq 0)
    }
  }
  $makeConfig = {
    param($row)
    $root = Split-Path -Parent $Q2Path
    if (-not (& $check $ContentCheckPath ([string]$row.createSettings.map) $root)) {
      throw 'room map is not available locally'
    }
    if (-not $rtcState.Fetched) {
      $rtcState.Fetched = $true
      $rtcState.Rows = @(& $FetchJson $Q2DZ_RTC_ENDPOINT)
    }
    New-Q2DzRoomConfig -Room $row -LookupPayload $lookup -RtcPayload $rtcState.Rows
  }
  return @(Update-Q2DzHubSlots -Slots $Slots -Rows $registeredReady -MakeConfig $makeConfig)
}

function Invoke-Q2DzHub {
  param(
    [Parameter(Mandatory = $true)][object[]]$InitialSlots,
    [Parameter(Mandatory = $true)][string]$Q2Path,
    [Parameter(Mandatory = $true)][string]$CoordinatorPath,
    [string[]]$CoordinatorArguments = @(),
    [scriptblock]$StartCoordinator = $null,
    [scriptblock]$Refresh = $null,
    [double]$PollIntervalSeconds = $HUB_POLL_SECONDS
  )
  if ($InitialSlots.Count -ne 2) { throw 'hub requires exactly two slots' }
  $owner = if ($StartCoordinator) {
    & $StartCoordinator $CoordinatorPath $CoordinatorArguments
  } else {
    Start-Q2DzCoordinator -Path $CoordinatorPath -Arguments $CoordinatorArguments
  }
  $process = $owner.Process
  $writer = $owner.Writer
  $exitCode = $null
  try {
    $code = Write-Q2DzHubSlots -Process $process -Writer $writer -Slots $InitialSlots
    if ($null -ne $code) { $exitCode = $code; return $code }
    $slots = @($InitialSlots)
    $refreshFn = if ($Refresh) { $Refresh } else { throw 'hub refresh function is required' }
    $deadline = (Get-Q2DzMonotonicSeconds) + [Math]::Max($PollIntervalSeconds, 0.001)
    while ($true) {
      while ($true) {
        if ($process.HasExited) { $exitCode = [int]$process.ExitCode; return $exitCode }
        if ($process.WaitForExit(1000)) { $exitCode = [int]$process.ExitCode; return $exitCode }
        if ((Get-Q2DzMonotonicSeconds) -ge $deadline) { break }
      }
      try {
        $updated = @(& $refreshFn $slots)
        if ($updated.Count -ne 2) { throw 'hub refresh returned an invalid slot count' }
      } catch {
        $deadline = (Get-Q2DzMonotonicSeconds) + [Math]::Max($PollIntervalSeconds, 0.001)
        continue
      }
      $slots = $updated
      $code = Write-Q2DzHubSlots -Process $process -Writer $writer -Slots $slots
      if ($null -ne $code) { $exitCode = $code; return $code }
      $deadline = (Get-Q2DzMonotonicSeconds) + [Math]::Max($PollIntervalSeconds, 0.001)
    }
  } finally {
    $disposeError = $null
    if ($writer) {
      try {
        $writer.Dispose()
      } catch {
        $exception = $_.Exception
        while ($exception.InnerException) { $exception = $exception.InnerException }
        $closedPipe = $process.HasExited -and (
          $exception -is [System.IO.IOException] -or
          $exception -is [System.ObjectDisposedException] -or
          $exception -is [System.InvalidOperationException]
        )
        if (-not $closedPipe) { $disposeError = $exception }
      }
    }
    if ($null -eq $exitCode) {
      try { $exitCode = Wait-Q2DzCoordinator -Process $process -TimeoutMilliseconds 10000 }
      catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
    }
    if ($null -eq $exitCode) {
      try {
        if ($process.PSObject.Methods.Name -contains 'CloseMainWindow') { [void]$process.CloseMainWindow() }
        else { [void]$process.Kill() }
      } catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
      try { $exitCode = Wait-Q2DzCoordinator -Process $process -TimeoutMilliseconds 5000 }
      catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
    }
    if ($null -eq $exitCode) {
      try { [void]$process.Kill() }
      catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
      try { $exitCode = Wait-Q2DzCoordinator -Process $process -TimeoutMilliseconds 5000 }
      catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
    }
    if ($process.PSObject.Methods.Name -contains 'Dispose') {
      try { $process.Dispose() }
      catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
    }
    if ($null -ne $disposeError) { throw $disposeError }
  }
}

function Start-Q2DzLauncher {
  # The launcher has no room picker: Q2PRO-X's Custom browser is the only room list.
  $state = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone\state'
  $q2PathFile = Join-Path $state 'q2-path.txt'
  $coordinator = Join-Path $app 'q2dz-session.exe'
  $contentCheck = Join-Path $app 'q2dz-content-check.exe'
  $gateway = Join-Path $app 'gateway\q2-doszone-webview2-gateway.exe'
  foreach ($path in $coordinator,$contentCheck,$gateway) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Installation is incomplete: $path" }
  }
  if (Get-Process 'Q2PRO-X' -ErrorAction SilentlyContinue) {
    [Windows.Forms.MessageBox]::Show('Close the running Q2PRO-X before joining a Dos.Zone room.','Q2PRO-X - Dos.Zone') | Out-Null
    return 4
  }
  $q2 = if (Test-Path $q2PathFile) { (Get-Content $q2PathFile -Raw).Trim() } else { '' }
  if (-not $q2 -or -not (Test-Path -LiteralPath $q2 -PathType Leaf)) {
    $picker = New-Object Windows.Forms.OpenFileDialog
    $picker.Title = 'Select your installed Q2PRO-X.exe'
    $picker.Filter = 'Q2PRO-X|Q2PRO-X.exe'
    if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { return 1 }
    $q2 = $picker.FileName
    New-Item -ItemType Directory -Force $state | Out-Null
    Set-Content -LiteralPath $q2PathFile -Value $q2 -Encoding UTF8
  }

  $slots = @((New-Q2DzHubSlot), (New-Q2DzHubSlot))
  try {
    $slots = @(Get-Q2DzLiveHubSlots -Slots $slots -Q2Path $q2 -ContentCheckPath $contentCheck)
  } catch {
    $slots = @((New-Q2DzHubSlot), (New-Q2DzHubSlot))
  }
  $refresh = {
    param($currentSlots)
    Get-Q2DzLiveHubSlots -Slots $currentSlots -Q2Path $q2 -ContentCheckPath $contentCheck
  }
  return (Invoke-Q2DzHub -InitialSlots $slots -Q2Path $q2 -CoordinatorPath $coordinator -CoordinatorArguments @(
    '--gateway', $gateway,
    '--q2', $q2,
    '--session-root', (Join-Path $state 'sessions')
  ) -Refresh $refresh)
}

if ($MyInvocation.InvocationName -ne '.') {
  exit (Start-Q2DzLauncher)
}
