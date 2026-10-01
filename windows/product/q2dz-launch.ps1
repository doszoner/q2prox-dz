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
  $startInfo.RedirectStandardError = $true
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $startInfo
  $started = $false
  $stderrReader = $null
  try {
    if (-not $process.Start()) { throw 'Could not start q2dz-session.exe.' }
    $started = $true
    $stderrQueue = [System.Collections.Concurrent.ConcurrentQueue[string]]::new()
    # The reader only enqueues lines; the launcher main loop drains and writes.
    $stderrReader = [PowerShell]::Create()
    [void]$stderrReader.AddScript({
      param($ChildProcess, $Queue)
      try {
        while (($line = $ChildProcess.StandardError.ReadLine()) -ne $null) {
          $Queue.Enqueue($line)
        }
      } catch {}
    }).AddArgument($process).AddArgument($stderrQueue)
    $stderrHandle = $stderrReader.BeginInvoke()
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $writer = New-Object System.IO.StreamWriter($process.StandardInput.BaseStream, $encoding)
    $writer.NewLine = "`n"
    return [pscustomobject]@{
      Process = $process; Writer = $writer; Queue = $stderrQueue
      StderrReader = $stderrReader; StderrHandle = $stderrHandle
    }
  } catch {
    if ($null -ne $stderrReader) {
      try { $stderrReader.Stop() } catch {}
      try { $stderrReader.Dispose() } catch {}
    }
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
    Write-Q2DzCatalogEvent ('HEARTBEAT generation={0} puts={1} drops={2}' -f `
      $Generation,$Plan.Targets.Count,@($Plan.Drops).Count)
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

# The child-stderr allow-list filter mirrors contains_sensitive_key +
# sanitize_log_line in linux/src/common.cpp; keep both lists identical.
$Q2DZ_SENSITIVE_SUBSTRINGS = @(
  'credential', 'password', 'secret', 'token', 'authorization', 'ice',
  'candidate', 'sdp', 'turn:', 'turns:', 'username', 'fingerprint',
  'local-description', 'remote-description', 'session-description',
  'peer id', 'peer_id', 'peerid', 'alias=', 'alias "', 'alias lookup',
  'aliaserror'
)
$Q2DZ_SDP_FIELD_LETTERS = 'vosiuepcbtrzkam'
$Q2DZ_EVENT_FIELD_PATTERN = '^[A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9._:-]{1,64}$'
$Q2DZ_ROOM_FIELD_PATTERN = '^room=[A-Za-z0-9._-]{1,255}$'

function Test-Q2DzSensitiveChildLine {
  param([AllowEmptyString()][string]$Line)
  $lower = $Line.ToLowerInvariant()
  foreach ($key in $Q2DZ_SENSITIVE_SUBSTRINGS) {
    if ($lower.Contains($key)) { return $true }
  }
  return ($Line.Length -ge 2 -and $Line[1] -eq '=' -and
          $Q2DZ_SDP_FIELD_LETTERS.IndexOf([char]::ToLowerInvariant($Line[0])) -ge 0)
}

$Q2DZ_EVENT_KEYS = @{
  'TARGET' = @('room','id','state','reason','recovery','retry','published_ready')
  'SESSION start' = @('build','session','instance','gateway_restarts')
  'SESSION stop' = @('reason','rc')
  'GATEWAY event' = @('event','reason','restarts')
  'GATEWAY stats' = @(
    'timerTicks','timerMaxGapMs','waitCalls','aliasPollIterations','probeSendCount',
    'probeAckCount','dcToNativeDatagrams','dcToNativeBytes','dcToNativeMaxGapMs',
    'nativeToDcDatagrams','nativeToDcBytes','nativeToDcMaxGapMs','udpFromQ2Datagrams',
    'udpFromQ2Bytes','udpToQ2Datagrams','udpToQ2Bytes','postToWebDatagrams',
    'webToNativeDatagrams','udpToQ2MaxGapMs','postToWebMaxGapMs'
  )
  'GATEWAY error' = @('reason')
  'GATEWAY stop' = @(
    'udp_to_web','web_to_udp','targets','max_udp_to_transport_datagram',
    'max_transport_to_udp_datagram','oversized_udp_drops','oversized_transport_drops',
    'unknown_peer_drops','second_source_drops','transport_not_ready_drops',
    'invalid_cadence_drops'
  )
}

function Test-Q2DzEventFieldValid {
  param([Parameter(Mandatory = $true)][string]$Field, [Parameter(Mandatory = $true)][string[]]$AllowedKeys)
  $eq = $Field.IndexOf('=')
  if ($eq -lt 0) { return $false }
  $key = $Field.Substring(0, $eq)
  if ($AllowedKeys -cnotcontains $key) { return $false }
  if ($key -ceq 'room') { return [bool]($Field -cmatch $Q2DZ_ROOM_FIELD_PATTERN) }
  if (-not ($Field -cmatch $Q2DZ_EVENT_FIELD_PATTERN)) { return $false }
  return -not (Test-Q2DzSensitiveChildLine $Field)
}

function ConvertTo-Q2DzChildEvent {
  param([AllowEmptyString()][string]$Line)
  # Returns the log line for one raw child-stderr line: a pass-through for
  # allow-listed coordinator/gateway events, else an escaped CHILD event. A
  # line that fully matches the event grammar passes through untouched, even
  # if it contains a filter substring (e.g. room=room.q2ded.nice) — the
  # substring filter only applies to lines outside the grammar.
  $fullLine = $Line
  $tokens = $fullLine -split ' '
  $fields = @()
  $component = $null
  $spec = $null
  if ($tokens[0] -ceq 'TARGET') {
    $component = 'COORDINATOR'; $fields = @($tokens | Select-Object -Skip 1); $spec = 'TARGET'
  } elseif ($tokens[0] -ceq 'SESSION' -and $tokens.Count -ge 2 -and
            @('start','stop') -ccontains $tokens[1]) {
    $component = 'COORDINATOR'; $fields = @($tokens | Select-Object -Skip 2); $spec = "SESSION $($tokens[1])"
  } elseif ($tokens[0] -ceq 'GATEWAY' -and $tokens.Count -ge 2 -and
            $tokens[1] -cmatch '^event=') {
    $component = 'COORDINATOR'; $fields = @($tokens | Select-Object -Skip 1); $spec = 'GATEWAY event'
  } elseif ($tokens[0] -ceq 'GATEWAY' -and $tokens.Count -ge 2 -and
            @('stats','error','stop') -ccontains $tokens[1]) {
    $component = ''; $fields = @($tokens | Select-Object -Skip 2); $spec = "GATEWAY $($tokens[1])"
  }
  if ($null -ne $component -and $Q2DZ_EVENT_KEYS.ContainsKey($spec)) {
    $allowedKeys = $Q2DZ_EVENT_KEYS[$spec]
    $wellFormed = $true
    foreach ($field in $fields) {
      if (-not (Test-Q2DzEventFieldValid $field $allowedKeys)) { $wellFormed = $false; break }
    }
    if ($wellFormed) {
      if ($component -eq '') { return $fullLine }
      return ($component + ' ' + $fullLine)
    }
  }
  if (Test-Q2DzSensitiveChildLine $fullLine) { return 'LAUNCHER CHILD suppressed=1' }
  if ($Line.Length -gt 2048) { $Line = $Line.Substring(0, 2048) }
  $chars = $Line.ToCharArray()
  for ($i = 0; $i -lt $chars.Length; $i++) {
    $code = [int][char]$chars[$i]
    if (($code -lt 0x20 -and $chars[$i] -ne "`t") -or $code -eq 0x7F) { $chars[$i] = ' ' }
  }
  $escaped = -join $chars
  if ($escaped.Length -gt 512) { $escaped = $escaped.Substring(0, 512) }
  return ('LAUNCHER CHILD line=' + $escaped)
}

function Read-Q2DzChildQueue {
  param($Queue)
  if ($null -eq $Queue) { return }
  $line = $null
  while ($Queue.TryDequeue([ref]$line)) {
    if ($null -ne $line) { Write-Q2DzSessionLogLine (ConvertTo-Q2DzChildEvent $line) }
    $line = $null
  }
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

function Get-Q2DzRefreshErrorKind {
  param([Parameter(Mandatory = $true)]$Exception)
  # Maps a refresh failure to the spec's error= category without its message.
  if ($null -ne $Exception.Data -and $Exception.Data.Contains('q2dz.http')) { return 'http' }
  $message = [string]$Exception.Message
  if ($Exception -is [System.TimeoutException] -or
      $Exception -is [System.Threading.Tasks.TaskCanceledException] -or
      $message -match 'timed out|timeout') { return 'timeout' }
  if ($message -match 'catalog_capacity_exceeded') { return 'capacity' }
  if ($message -match 'content checker') { return 'content_check' }
  if ($message -match 'RTC|ICE') { return 'rtc' }
  if ($message -match 'non-success status') { return 'http' }
  if ($message -match 'invalid schema|duplicate') { return 'schema' }
  if ($message -match 'invalid JSON|invalid gzip|too large|encoding|empty JSON|valid UTF-8') { return 'json' }
  if ($message -match 'configuration|one line|one string') { return 'config' }
  return 'other'
}

function Get-Q2DzLiveTargets {
  param(
    [Parameter(Mandatory = $true)]$Targets,
    [Parameter(Mandatory = $true)][string]$Q2Path,
    [Parameter(Mandatory = $true)][string]$ContentCheckPath,
    [scriptblock]$FetchJson = { param($Endpoint) Get-Q2DzJson $Endpoint },
    [scriptblock]$RunContentCheck = $null
  )
  $timer = [Diagnostics.Stopwatch]::StartNew()
  $result = 'error'
  $stage = 'catalog'
  $errorKind = 'none'
  $httpStatus = 0
  $previousCount = $Targets.Count
  $readyCount = 0
  $registeredCount = 0
  $targetCount = $previousCount
  $dropCount = 0
  $putCount = 0
  try {
    $catalog = & $FetchJson $Q2DZ_ROOM_ENDPOINT
    if ($null -eq $catalog -or $null -eq $catalog.PSObject -or
        $catalog.PSObject.Properties.Name -notcontains 'rooms' -or
        $catalog.rooms -isnot [array]) {
      throw 'room catalog has an invalid schema'
    }
    # Tolerant row gate (D4/D5): identity is resolved before validity,
    # unidentifiable/invalid/duplicate-grouped rows are rejected one by
    # one, and the accepted rows alone drive selection and planning.
    $rejections = @(Get-Q2DzCatalogRowRejections $catalog)
    Write-Q2DzCatalogRowRejections $rejections
    $rejectedIndex = @{}
    $rejectedRooms = New-Q2DzTargetMap
    foreach ($rejection in $rejections) {
      $rejectedIndex[[int]$rejection.Index] = $true
      if ($null -ne $rejection.Room) { $rejectedRooms[[string]$rejection.Room] = $true }
    }
    $acceptedRows = @()
    $index = -1
    foreach ($row in @($catalog.rooms)) {
      $index++
      if ($rejectedIndex.ContainsKey($index)) { continue }
      if (-not (Test-Q2DzCatalogRowClassifiable $row)) { continue }
      $acceptedRows += $row
    }
    # Strict selector on the filtered set: its row and duplicate checks are
    # unchanged; an accepted row always passes them.
    $readyRows = @(Get-Q2DzReadyRooms ([pscustomobject]@{ rooms = [object[]]$acceptedRows }))
    $readyCount = $readyRows.Count
    $stage = 'lookup'
    $lookup = & $FetchJson $Q2DZ_LOOKUP_ENDPOINT
    $lookup = [pscustomobject]@{ aliases = @(Select-Q2DzSanitizedAliases $lookup) }
    $registeredReady = @(Select-Q2DzRegisteredRooms -Rows $readyRows -LookupPayload $lookup)
    $registeredCount = $registeredReady.Count
    $stage = 'servers'
    $rtcRows = @(& $FetchJson $Q2DZ_RTC_ENDPOINT)
    $iceServers = @(Get-Q2DzDedicatedIceServers $rtcRows)
    $check = if ($RunContentCheck) { $RunContentCheck } else {
      {
        param($Path, $Map, $Root)
        try {
          & $Path --root $Root --game $Q2DZ_GAME --map $Map | Out-Null
        } catch {
          throw 'content checker execution failed'
        }
        $exitCode = [int]$LASTEXITCODE
        if ($exitCode -eq 0) { return $true }
        if ($exitCode -eq 3) { return $false }
        throw 'content checker execution failed'
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
    $stage = 'plan'
    $updated = Get-Q2DzTargetPlan -Targets $Targets -Rows $registeredReady -RejectedRooms $rejectedRooms -MakeConfig $makeConfig -MapAvailable $mapAvailable
    $updated.IceServers = @($iceServers)
    $updated | Add-Member -NotePropertyName Accepted -NotePropertyValue (Sort-Q2DzStrings @($acceptedRows | ForEach-Object { [string]$_.room })) -Force
    $updated | Add-Member -NotePropertyName Registered -NotePropertyValue (Sort-Q2DzStrings @($registeredReady | ForEach-Object { [string]$_.room })) -Force
    $targetCount = $updated.Targets.Count
    $dropCount = $updated.Drops.Count
    $putCount = $updated.Targets.Count
    $result = 'ok'
    return $updated
  }
  catch {
    $failureException = $_.Exception
    if ($null -ne $failureException.Data -and $failureException.Data.Contains('q2dz.http')) {
      $errorKind = 'http'
      $httpStatus = [int]$failureException.Data['q2dz.http']
    } else {
      $errorKind = Get-Q2DzRefreshErrorKind -Exception $failureException
      $httpStatus = 0
    }
    throw
  }
  finally {
    $timer.Stop()
    Write-Q2DzCatalogEvent ('CATALOG_REFRESH result={0} stage={1} error={2} http={3} elapsed_ms={4} previous={5} ready={6} registered={7} targets={8} drops={9} puts={10}' -f `
      $result,$stage,$errorKind,$httpStatus,$timer.ElapsedMilliseconds,$previousCount,$readyCount,$registeredCount,$targetCount,$dropCount,$putCount)
  }
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
  # Property values are fetched in statement position: emitting a
  # ConcurrentQueue from an if-branch would enumerate it into nothing.
  $childQueue = $null
  if ($null -ne $owner.PSObject.Properties['Queue']) { $childQueue = $owner.Queue }
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
        Read-Q2DzChildQueue $childQueue
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
        Read-Q2DzChildQueue $childQueue
        $deadline = (Get-Q2DzMonotonicSeconds) + [Math]::Max($PollIntervalSeconds, 0.001)
        continue
      }
      $targets = Copy-Q2DzTargetMap $updated.Targets
      $generation++
      $code = Write-Q2DzHeartbeat -Process $process -Writer $writer -Plan $updated -Generation $generation
      Read-Q2DzChildQueue $childQueue
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
    # The stderr reader ends when the pipe hits EOF; join it so lines written
    # right before exit are enqueued. A lingering grandchild can hold the
    # inherited handle open, so the wait is bounded and the stream is closed
    # to unblock the reader before the process is disposed.
    if ($null -ne $owner.PSObject.Properties['StderrReader'] -and $null -ne $owner.StderrReader) {
      $readerDone = $false
      try { $readerDone = $owner.StderrHandle.AsyncWaitHandle.WaitOne(3000) } catch {}
      if (-not $readerDone -and $process -is [System.Diagnostics.Process]) {
        try { $process.StandardError.Dispose() } catch {}
        try { $readerDone = $owner.StderrHandle.AsyncWaitHandle.WaitOne(2000) } catch {}
      }
      if ($readerDone) {
        try { $owner.StderrReader.EndInvoke($owner.StderrHandle) }
        catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
        try { $owner.StderrReader.Dispose() } catch {}
      }
    }
    Read-Q2DzChildQueue $childQueue
    if ($process.PSObject.Methods.Name -contains 'Dispose') {
      try { $process.Dispose() }
      catch { if ($null -eq $disposeError) { $disposeError = $_.Exception } }
    }
    if ($null -ne $disposeError) { throw $disposeError }
  }
}

function Start-Q2DzLauncher {
  # Every exit after SESSION start — including an installation-check throw,
  # or an exception from the coordinator call — must emit exactly one
  # SESSION stop. $stopReason/$stopCode hold the outcome for the single
  # emission in this function's finally block.
  # The launcher has no room picker: Q2PRO-X's Custom browser is the only room list.
  $state = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone\state'
  Initialize-Q2DzSessionLog -StateDirectory $state
  $build = 'unknown'
  $versionFile = Join-Path $app 'VERSION'
  if (Test-Path -LiteralPath $versionFile) {
    $build = ((Get-Content -LiteralPath $versionFile -Raw) -replace '[^A-Za-z0-9._-]', '').Trim()
    if (-not $build) { $build = 'unknown' }
  }
  $rc = 'none'
  if ($build -match 'rc(\d+)') { $rc = $Matches[1] }
  Write-Q2DzCatalogEvent ('SESSION start build={0} rc={1} os=windows' -f $build,$rc)
  $stopReason = 'error'
  $stopCode = 1
  try {
    $q2PathFile = Join-Path $state 'q2-path.txt'
    $coordinator = Join-Path $app 'q2dz-session.exe'
    $contentCheck = Join-Path $app 'q2dz-content-check.exe'
    $gateway = Join-Path $app 'gateway\q2-doszone-webview2-gateway.exe'
    foreach ($path in $coordinator,$contentCheck,$gateway) {
      if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        $stopReason = 'installation_incomplete'
        $stopCode = 1
        throw "Installation is incomplete: $path"
      }
    }
    if (Get-Process 'Q2PRO-X' -ErrorAction SilentlyContinue) {
      [Windows.Forms.MessageBox]::Show('Close the running Q2PRO-X before joining a Dos.Zone room.','Q2PRO-X - Dos.Zone') | Out-Null
      $stopReason = 'already_running'
      $stopCode = 4
      return $stopCode
    }
    $q2 = if (Test-Path $q2PathFile) { (Get-Content $q2PathFile -Raw).Trim() } else { '' }
    if (-not $q2 -or -not (Test-Path -LiteralPath $q2 -PathType Leaf)) {
      $picker = New-Object Windows.Forms.OpenFileDialog
      $picker.Title = 'Select your installed Q2PRO-X.exe'
      $picker.Filter = 'Q2PRO-X|Q2PRO-X.exe'
      if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) {
        $stopReason = 'picker_cancelled'
        $stopCode = 1
        return $stopCode
      }
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
    $result = Invoke-Q2DzHub -InitialTargets $initialValue -Q2Path $q2 -CoordinatorPath $coordinator -CoordinatorArguments @(
      '--gateway', $gateway,
      '--q2', $q2,
      '--session-root', (Join-Path $state 'sessions')
    ) -Refresh $refresh
    $stopReason = 'coordinator_exit'
    $stopCode = [int]$result
    return $result
  } finally {
    Write-Q2DzCatalogEvent ('SESSION stop reason={0} rc={1}' -f $stopReason,$stopCode)
  }
}

if ($MyInvocation.InvocationName -ne '.') {
  exit (Start-Q2DzLauncher)
}
