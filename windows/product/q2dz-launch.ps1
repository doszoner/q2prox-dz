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

function ConvertTo-Q2DzCommandText {
  param([Parameter(Mandatory = $true)][string]$Text)
  if ([Text.Encoding]::UTF8.GetByteCount($Text) -gt $Q2DZ_MAX_HUB_COMMAND_BYTES) {
    throw 'hub command exceeds 256 KiB'
  }
  return $Text
}

function ConvertTo-Q2DzIceCommand {
  param(
    [Parameter(Mandatory = $true)][UInt64]$Generation,
    [Parameter(Mandatory = $true)]$IceServers
  )
  if ($Generation -eq 0) { throw 'invalid ICE generation' }
  $json = ConvertTo-Json -InputObject ([array]@($IceServers)) -Depth 8 -Compress
  return ConvertTo-Q2DzCommandText (('Q2DZ/ROOMS/2 ICE generation={0} json={1}' -f $Generation,$json) + "`n")
}

function ConvertTo-Q2DzPutCommand {
  param([Parameter(Mandatory = $true)]$Target)
  if (-not (Test-Q2DzSafeToken $Target.RoomId) -or [string]::IsNullOrEmpty([string]$Target.Config)) {
    throw 'target has no safe room id'
  }
  return ConvertTo-Q2DzCommandText (('Q2DZ/ROOMS/2 PUT room={0} json={1}' -f $Target.RoomId,$Target.Config) + "`n")
}

function ConvertTo-Q2DzDropCommand {
  param([Parameter(Mandatory = $true)][string]$RoomId)
  if (-not (Test-Q2DzSafeToken $RoomId)) { throw 'target has no safe room id' }
  return ConvertTo-Q2DzCommandText (('Q2DZ/ROOMS/2 DROP room={0}' -f $RoomId) + "`n")
}

function Write-Q2DzUtf8 {
  param(
    [Parameter(Mandatory = $true)]$Writer,
    [Parameter(Mandatory = $true)][string]$Text
  )
  $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
  try {
    $Writer.BaseStream.Write($bytes, 0, $bytes.Length)
  } finally {
    [Array]::Clear($bytes, 0, $bytes.Length)
  }
}

function Write-Q2DzHeartbeat {
  param(
    [Parameter(Mandatory = $true)]$Process,
    [Parameter(Mandatory = $true)]$Writer,
    [Parameter(Mandatory = $true)]$Plan,
    [Parameter(Mandatory = $true)][UInt64]$Generation
  )
  if ($null -eq $Plan -or $null -eq $Plan.Targets -or $null -eq $Plan.IceServers) {
    throw 'hub heartbeat is incomplete'
  }
  $lines = [System.Collections.Generic.List[string]]::new()
  try {
    [void]$lines.Add((ConvertTo-Q2DzIceCommand $Generation $Plan.IceServers))
    foreach ($roomId in (Sort-Q2DzStrings @($Plan.Drops))) {
      [void]$lines.Add((ConvertTo-Q2DzDropCommand $roomId))
    }
    foreach ($roomId in (Sort-Q2DzStrings @($Plan.Targets.Keys))) {
      [void]$lines.Add((ConvertTo-Q2DzPutCommand $Plan.Targets[$roomId]))
    }
    foreach ($line in $lines) { Write-Q2DzUtf8 -Writer $Writer -Text $line }
    $Writer.BaseStream.Flush()
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
  } finally {
    $lines.Clear()
  }
}

function Get-Q2DzMonotonicSeconds {
  return [Diagnostics.Stopwatch]::GetTimestamp() / [double][Diagnostics.Stopwatch]::Frequency
}

function New-Q2DzRoomConfig {
  param(
    [Parameter(Mandatory = $true)]$Room,
    [Parameter(Mandatory = $true)]$LookupPayload,
    [Parameter(Mandatory = $true)]$RtcPayload,
    $IceServers = $null
  )
  if (-not (Test-Q2DzRoomRow $Room) -or ((Get-Q2DzRoomKind $Room) -ceq 'q2' -and -not $Room.roomReady)) {
    throw 'The selected room is no longer available'
  }
  if (-not (Test-Q2DzAliasPayload $LookupPayload)) { throw 'alias lookup has an invalid schema' }
  $alias = Get-Q2DzRoomAlias $Room
  if ($LookupPayload.aliases -cnotcontains $alias) { throw 'selected room alias is no longer registered' }
  if (-not (Test-Q2DzRtcPayload $RtcPayload)) { throw 'RTC configuration has an invalid schema' }
  $iceServers = [array]@(if ($null -eq $IceServers) {
      Get-Q2DzDedicatedIceServers $RtcPayload
    } else {
      @($IceServers)
    })
  $config = [ordered]@{
    version = 1
    roomId = $Room.room
    alias = $alias
    signalingUrl = $Q2DZ_SIGNALING_URL
    ordered = $Q2DZ_ORDERED
    iceServers = $iceServers
    map = $Room.createSettings.map
    game = $Q2DZ_GAME
  }
  $json = $config | ConvertTo-Json -Depth 8 -Compress
  if ((Get-Q2DzRoomKind $Room) -ceq 'q2ded' -and
      [Text.Encoding]::UTF8.GetByteCount($json) -gt $Q2DZ_MAX_GATEWAY_CONFIG_BYTES) {
    throw 'dedicated room configuration exceeds 65535 bytes'
  }
  return $json
}

function Get-Q2DzLiveTargets {
  param(
    [Parameter(Mandatory = $true)]$Targets,
    [Parameter(Mandatory = $true)][string]$Q2Path,
    [Parameter(Mandatory = $true)][string]$ContentCheckPath,
    [scriptblock]$FetchJson = { param($Endpoint) Get-Q2DzJson $Endpoint },
    [scriptblock]$RunContentCheck = $null
  )
  $catalog = & $FetchJson $Q2DZ_ROOM_ENDPOINT
  if (-not (Test-Q2DzRoomPayload $catalog) -and
      -not (Test-Q2DzEmptyRoomCatalog $catalog)) {
    try { Get-Q2DzReadyRooms $catalog | Out-Null } catch { throw }
    throw 'room catalog has an invalid schema'
  }
  $lookup = & $FetchJson $Q2DZ_LOOKUP_ENDPOINT
  $readyRows = @(Get-Q2DzReadyRooms $catalog)
  $registeredReady = @(Select-Q2DzRegisteredRooms -Rows $readyRows -LookupPayload $lookup)
  $rtcRows = @(& $FetchJson $Q2DZ_RTC_ENDPOINT)
  $iceServers = @(Get-Q2DzDedicatedIceServers $rtcRows)
  $check = if ($RunContentCheck) { $RunContentCheck } else {
    {
      param($Path, $Map, $Root)
      & $Path --root $Root --game $Q2DZ_GAME --map $Map | Out-Null
      return ($LASTEXITCODE -eq 0)
    }
  }
  $makeConfig = {
    param($row)
    New-Q2DzRoomConfig -Room $row -LookupPayload $lookup -RtcPayload $rtcRows -IceServers $iceServers
  }
  $mapAvailable = {
    param($row)
    $root = Split-Path -Parent $Q2Path
    return [bool](& $check $ContentCheckPath ([string]$row.createSettings.map) $root)
  }
  $updated = Get-Q2DzTargetPlan -Targets $Targets -Rows $registeredReady -MakeConfig $makeConfig -MapAvailable $mapAvailable
  $updated.IceServers = @($iceServers)
  return $updated
}

function Invoke-Q2DzHub {
  param(
    [Parameter(Mandatory = $true)]$InitialTargets,
    [Parameter(Mandatory = $true)][string]$Q2Path,
    [Parameter(Mandatory = $true)][string]$CoordinatorPath,
    [string[]]$CoordinatorArguments = @(),
    [scriptblock]$StartCoordinator = $null,
    [scriptblock]$Refresh = $null,
    [double]$PollIntervalSeconds = $HUB_POLL_SECONDS
  )
  $initialPlan = if ($InitialTargets.PSObject.Properties.Name -contains 'Targets' -and
      $InitialTargets.PSObject.Properties.Name -contains 'IceServers') { $InitialTargets } else { $null }
  $targets = if ($initialPlan) { Copy-Q2DzTargetMap $initialPlan.Targets } else { Copy-Q2DzTargetMap $InitialTargets }
  $initialIce = if ($initialPlan) { @($initialPlan.IceServers) } else {
    $candidate = $null
    foreach ($roomId in (Sort-Q2DzStrings @($targets.Keys))) {
      try { $candidate = (([string]$targets[$roomId].Config) | ConvertFrom-Json -ErrorAction Stop).iceServers } catch { $candidate = $null }
      if ($candidate) { break }
    }
    @($candidate)
  }
  $owner = if ($StartCoordinator) {
    & $StartCoordinator $CoordinatorPath $CoordinatorArguments
  } else {
    Start-Q2DzCoordinator -Path $CoordinatorPath -Arguments $CoordinatorArguments
  }
  $process = $owner.Process
  $writer = $owner.Writer
  $exitCode = $null
  try {
    if (@($initialIce).Count -gt 0) {
      $code = Write-Q2DzHeartbeat -Process $process -Writer $writer `
        -Plan ([pscustomobject]@{ Targets = $targets; IceServers = @($initialIce); Drops = @() }) -Generation 1
      if ($null -ne $code) { $exitCode = $code; return $code }
    }
    $refreshFn = if ($Refresh) { $Refresh } else { throw 'hub refresh function is required' }
    $generation = [UInt64]1
    $deadline = (Get-Q2DzMonotonicSeconds) + [Math]::Max($PollIntervalSeconds, 0.001)
    while ($true) {
      while ($true) {
        if ($process.HasExited) { $exitCode = [int]$process.ExitCode; return $exitCode }
        if ($process.WaitForExit(1000)) { $exitCode = [int]$process.ExitCode; return $exitCode }
        if ((Get-Q2DzMonotonicSeconds) -ge $deadline) { break }
      }
      try {
        $updated = & $refreshFn $targets
        if ($null -eq $updated -or $null -eq $updated.Targets -or $null -eq $updated.IceServers) {
          throw 'hub refresh returned an incomplete heartbeat'
        }
      } catch {
        $deadline = (Get-Q2DzMonotonicSeconds) + [Math]::Max($PollIntervalSeconds, 0.001)
        continue
      }
      $targets = Copy-Q2DzTargetMap $updated.Targets
      $generation++
      $code = Write-Q2DzHeartbeat -Process $process -Writer $writer -Plan $updated -Generation $generation
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

  $targets = New-Q2DzTargetMap
  $initial = $null
  try {
    $initial = Get-Q2DzLiveTargets -Targets $targets -Q2Path $q2 -ContentCheckPath $contentCheck
    $targets = $initial.Targets
  } catch {
    $initial = $null
  }
  $refresh = {
    param($currentTargets)
    Get-Q2DzLiveTargets -Targets $currentTargets -Q2Path $q2 -ContentCheckPath $contentCheck
  }
  $initialValue = if ($initial) { $initial } else { $targets }
  return (Invoke-Q2DzHub -InitialTargets $initialValue -Q2Path $q2 -CoordinatorPath $coordinator -CoordinatorArguments @(
    '--gateway', $gateway,
    '--q2', $q2,
    '--session-root', (Join-Path $state 'sessions')
  ) -Refresh $refresh)
}

if ($MyInvocation.InvocationName -ne '.') {
  exit (Start-Q2DzLauncher)
}
