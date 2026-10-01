[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\q2dz-launch.ps1')

function Assert([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function New-FakeHubProcess {
  param([int]$ExitAfterWaits = 2, [int]$ExitCode = 0, [bool]$AlreadyExited = $false)
  $process = [pscustomobject]@{
    HasExited = $AlreadyExited
    ExitCode = $ExitCode
    ExitAfterWaits = $ExitAfterWaits
    Waits = 0
    Kills = 0
    Terminates = 0
    ClosedWindows = 0
    Disposed = 0
  }
  $process | Add-Member ScriptMethod WaitForExit {
    param([int]$Milliseconds)
    $this.Waits++
    if (-not $this.HasExited -and $this.Waits -ge $this.ExitAfterWaits) { $this.HasExited = $true }
    return [bool]$this.HasExited
  }
  $process | Add-Member ScriptMethod Kill { $this.Kills++; $this.HasExited = $true }
  $process | Add-Member ScriptMethod CloseMainWindow { $this.ClosedWindows++; return $false }
  $process | Add-Member ScriptMethod Dispose { $this.Disposed++ }
  return $process
}

function New-FakeHubWriter {
  param($Process, [bool]$FailAfterFirstWrite = $false, [Exception]$DisposeError = $null)
  $writer = [pscustomobject]@{
    Process = $Process
    Text = New-Object System.Text.StringBuilder
    Writes = 0
    Flushes = 0
    Disposed = $false
    FailAfterFirstWrite = $FailAfterFirstWrite
    DisposeError = $DisposeError
  }
  $stream = [pscustomobject]@{ Owner = $writer }
  $stream | Add-Member ScriptMethod Write {
    param([byte[]]$Bytes, [int]$Offset, [int]$Count)
    if ($this.Owner.Process.HasExited) { throw [IO.IOException]::new('pipe closed') }
    $text = [Text.Encoding]::UTF8.GetString($Bytes, $Offset, $Count)
    [void]$this.Owner.Text.Append($text)
    $this.Owner.Writes++
    if ($this.Owner.FailAfterFirstWrite -and $this.Owner.Writes -eq 1) {
      $this.Owner.Process.HasExited = $true
      $this.Owner.Process.ExitCode = 74
    }
  }
  $stream | Add-Member ScriptMethod Flush { $this.Owner.Flushes++ }
  $writer | Add-Member NoteProperty BaseStream $stream
  $writer | Add-Member ScriptMethod Dispose {
    $this.Disposed = $true
    if ($null -ne $this.DisposeError) { throw $this.DisposeError }
  }
  return $writer
}

function New-TestRoom {
  param([string]$RoomId = 'room.q2.client.1', [string]$Owner = '1', [string]$Map = 'q2dm1')
  return [pscustomobject]@{
    room = $RoomId; owner = $Owner; roomReady = $true; players = 1; rtcServer = 'lvc.dos.zone'
    createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Client'; map = $Map; maxPlayers = 16 }
  }
}

function New-TestDedicatedRoom {
  param([string]$RoomId = 'room.q2ded.server.1', [string]$Map = 'q2dm1')
  return [pscustomobject]@{
    room = $RoomId; owner = 0; players = 1
    createSettings = [pscustomobject]@{ prefix = 'q2ded'; name = 'Server'; map = $Map; maxPlayers = 16 }
  }
}

function New-TestRtc {
  $rows = @([pscustomobject]@{
    domain = 'lvc.dos.zone'
    iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') })
  })
  Write-Output -NoEnumerate $rows
}

function New-TestTargetPlan {
  param([object[]]$Rows)
  $targets = New-Q2DzTargetMap
  foreach ($row in $Rows) {
    $json = New-Q2DzRoomConfig -Room $row -LookupPayload ([pscustomobject]@{ aliases = @((Get-Q2DzRoomAlias $row)) }) -RtcPayload (New-TestRtc)
    $targets[$row.room] = New-Q2DzTarget $row.room $json 0 $row.createSettings.map
  }
  return [pscustomobject]@{
    Targets = $targets
    IceServers = @((New-TestRtc)[0].iceservers)
    Drops = @()
  }
}

$rows = @((New-TestRoom 'room.q2.client.1' '1'), (New-TestDedicatedRoom 'room.q2ded.server.1'))
$transportConfig = New-Q2DzRoomConfig -Room $rows[0] `
  -LookupPayload ([pscustomobject]@{ aliases = @((Get-Q2DzRoomAlias $rows[0])) }) -RtcPayload (New-TestRtc)
$transportValue = $transportConfig | ConvertFrom-Json
Assert ($transportValue.signalingUrl -eq 'wss://net.js-dos.com:444') 'PowerShell uses the shared signaling transport contract'
Assert ($transportValue.iceServers -is [array]) 'PowerShell room config keeps ICE as an array'
$plan = New-TestTargetPlan $rows
$process = New-FakeHubProcess -ExitAfterWaits 99
$writer = New-FakeHubWriter $process
Write-Q2DzHeartbeat -Process $process -Writer $writer -Plan $plan -Generation 1 | Out-Null
$lines = @($writer.Text.ToString().Split("`n", [StringSplitOptions]::RemoveEmptyEntries))
Assert ($lines.Count -eq 3) 'one ICE and two room PUTs form a complete heartbeat'
Assert ($lines[0].StartsWith('Q2DZ/ROOMS/2 ICE generation=1 json=')) 'ICE precedes every PUT'
Assert ($lines[0] -match 'json=\[') 'ICE is encoded as a JSON array on Windows'
Assert ($lines[1].Contains('PUT room=room.q2.client.1')) 'room ids are ordered ordinally'
Assert ($lines[2].Contains('PUT room=room.q2ded.server.1')) 'mixed q2 and q2ded targets are represented'
Assert ($writer.Flushes -eq 1) 'heartbeat flushes once'

$dropPlan = [pscustomobject]@{ Targets = $plan.Targets; IceServers = $plan.IceServers; Drops = @('room.q2.client.1') }
$writer = New-FakeHubWriter $process
Write-Q2DzHeartbeat -Process $process -Writer $writer -Plan $dropPlan -Generation 2 | Out-Null
$dropLines = @($writer.Text.ToString().Split("`n", [StringSplitOptions]::RemoveEmptyEntries))
Assert ($dropLines[0].Contains('generation=2')) 'each successful poll has a fresh generation'
Assert ($dropLines[1] -eq 'Q2DZ/ROOMS/2 DROP room=room.q2.client.1') 'drop uses room id, not an array index'
Assert ($dropLines[2].Contains('PUT room=room.q2.client.1')) 'changed target is re-added after its drop'

$process = New-FakeHubProcess -ExitAfterWaits 2
$writer = New-FakeHubWriter $process
$refreshes = 0
$refresh = {
  param($currentTargets)
  $script:refreshes++
  return $plan
}
$result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { param($path,$args) [pscustomobject]@{ Process = $process; Writer = $writer } } `
  -Refresh $refresh -PollIntervalSeconds 0
Assert ($result -eq 0) 'normal coordinator exit is returned'
Assert ($refreshes -eq 1) 'one successful poll is performed'
$lines = @($writer.Text.ToString().Split("`n", [StringSplitOptions]::RemoveEmptyEntries))
Assert ($lines.Count -eq 6) 'initial heartbeat and one full heartbeat are both sent'
Assert (($lines | Where-Object { $_ -match ' ICE ' }).Count -eq 2) 'each heartbeat has exactly one ICE'

$process = New-FakeHubProcess -ExitAfterWaits 1
$writer = New-FakeHubWriter $process
$result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { param($path,$args) [pscustomobject]@{ Process = $process; Writer = $writer } } `
  -Refresh { throw 'catalog unavailable' } -PollIntervalSeconds 0
Assert ($result -eq 0) 'failed poll does not change coordinator result'
$lines = @($writer.Text.ToString().Split("`n", [StringSplitOptions]::RemoveEmptyEntries))
Assert ($lines.Count -eq 3) 'failed poll sends no partial heartbeat'

$process = New-FakeHubProcess -AlreadyExited $true -ExitCode 74
$writer = New-FakeHubWriter $process
$result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { param($path,$args) [pscustomobject]@{ Process = $process; Writer = $writer } } `
  -Refresh { return $plan } -PollIntervalSeconds 0
Assert ($result -eq 74) 'coordinator exit before reading is preserved'

$process = New-FakeHubProcess -ExitAfterWaits 1000
$writer = New-FakeHubWriter $process -FailAfterFirstWrite $true
$result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { param($path,$args) [pscustomobject]@{ Process = $process; Writer = $writer } } `
  -Refresh { return $plan } -PollIntervalSeconds 0
Assert ($result -eq 74) 'broken stdin returns coordinator exit code'

# A real child whose stderr exceeds the pipe buffer must not stall the hub,
# and lines written right before exit must still reach the session log.
$pwshExe = (Get-Process -Id $PID).Path
$stderrFlood = @'
$line = 'x' * 96
foreach ($n in 1..4000) { [Console]::Error.WriteLine('flood-' + $n.ToString('D4') + '-' + $line) }
[Console]::Error.WriteLine('TARGET room=room.q2ded.1 id=7 state=terminal reason=transport_failed recovery=3 retry=none published_ready=2')
[Console]::Error.WriteLine('credential=CANARY1')
[Console]::Error.WriteLine('a=candidate:CANARY2')
[Console]::Error.WriteLine('turn:CANARY3@host')
[Console]::Error.WriteLine('GATEWAY stats timerTicks=1 udpFromQ2Datagrams=0')
[Console]::Error.WriteLine('final-marker before exit')
exit 0
'@
$floodState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
$stderrCapture = [IO.StringWriter]::new()
$previousError = [Console]::Error
try {
  Initialize-Q2DzSessionLog -StateDirectory $floodState
  [Console]::SetError($stderrCapture)
  $result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
    -CoordinatorArguments @() -StartCoordinator {
      param($path,$args)
      Start-Q2DzCoordinator -Path $pwshExe -Arguments @('-NoProfile','-Command',$stderrFlood)
    } -Refresh { return $plan } -PollIntervalSeconds 0
} finally {
  [Console]::SetError($previousError)
}
Assert ($result -eq 0) 'child exit code survives the stderr drain'
$floodLogPath = Join-Path $floodState 'logs\q2dz.log'
Assert (Test-Path -LiteralPath $floodLogPath) 'session log file is written'
$floodLog = Get-Content -LiteralPath $floodLogPath -Raw
Assert ($floodLog -match '(?m)COORDINATOR TARGET room=room\.q2ded\.1 id=7 state=terminal reason=transport_failed recovery=3 retry=none published_ready=2\r?$') 'legitimate TARGET event passes the drain unchanged'
Assert ($floodLog -match '(?m)GATEWAY stats timerTicks=1 udpFromQ2Datagrams=0\r?$') 'gateway self-report event passes verbatim'
Assert (($floodLog -split "`n" | Where-Object { $_ -match 'CHILD suppressed=1' }).Count -eq 3) 'all three canary lines are suppressed'
Assert ($floodLog -match '(?m)CHILD line=flood-0001-') 'first flooded line reached the file'
Assert ($floodLog -match '(?m)CHILD line=flood-4000-') 'last flooded line reached the file'
Assert ($floodLog -match '(?m)CHILD line=final-marker before exit\r?$') 'last lines before child exit are drained'
Assert ($floodLog.IndexOf('flood-0001') -lt $floodLog.IndexOf('flood-4000')) 'stderr lines keep their order'
Assert (-not $floodLog.Contains('CANARY')) 'no canary reaches the session log'
$launcherStderr = $stderrCapture.ToString()
Assert (-not $launcherStderr.Contains('CANARY')) 'no canary reaches launcher stderr'
$stderrCapture.Dispose()
Remove-Item -LiteralPath $floodState -Recurse -Force -ErrorAction SilentlyContinue

# Rotation while a child is logging keeps the canary out of both files.
$rotateScript = @'
foreach ($n in 1..80) { [Console]::Error.WriteLine('credential=CANARY1 line=' + $n.ToString('D4') + '-' + ('x' * 96)) }
[Console]::Error.WriteLine('rotation-marker-end')
exit 0
'@
$rotateState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  Initialize-Q2DzSessionLog -StateDirectory $rotateState
  $previousMax = $Q2DZ_LOG_MAX_BYTES
  try {
    $Q2DZ_LOG_MAX_BYTES = 2048
    $result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
      -CoordinatorArguments @() -StartCoordinator {
        param($path,$args)
        Start-Q2DzCoordinator -Path $pwshExe -Arguments @('-NoProfile','-Command',$rotateScript)
      } -Refresh { return $plan } -PollIntervalSeconds 0
    Assert ($result -eq 0) 'rotation run keeps the child exit code'
  } finally {
    $Q2DZ_LOG_MAX_BYTES = $previousMax
  }
  $rotatedLogPath = Join-Path $rotateState 'logs\q2dz.1.log'
  Assert (Test-Path -LiteralPath $rotatedLogPath) 'rotated log file exists after the bound'
  Assert (-not (Get-Content -LiteralPath $rotatedLogPath -Raw).Contains('CANARY')) 'rotated log has no canary'
  Assert (-not (Get-Content -LiteralPath (Join-Path $rotateState 'logs\q2dz.log') -Raw).Contains('CANARY')) 'live log has no canary'
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $rotateState -Recurse -Force -ErrorAction SilentlyContinue
}

# A room id containing a filter substring ("nice" contains "ice") must still
# pass the drain unchanged when the whole line matches the event grammar;
# the same substring outside the grammar is still suppressed (canary).
$niceScript = @'
[Console]::Error.WriteLine('TARGET room=room.q2ded.nice id=1 state=ready reason=none recovery=0 retry=none published_ready=1')
[Console]::Error.WriteLine('a=candidate:deadbeef nice')
exit 0
'@
$niceState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  Initialize-Q2DzSessionLog -StateDirectory $niceState
  $result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
    -CoordinatorArguments @() -StartCoordinator {
      param($path,$args)
      Start-Q2DzCoordinator -Path $pwshExe -Arguments @('-NoProfile','-Command',$niceScript)
    } -Refresh { return $plan } -PollIntervalSeconds 0
  Assert ($result -eq 0) 'nice-room run keeps the child exit code'
  $niceLog = Get-Content -LiteralPath (Join-Path $niceState 'logs\q2dz.log') -Raw
  Assert ($niceLog -match '(?m)COORDINATOR TARGET room=room\.q2ded\.nice id=1 state=ready reason=none recovery=0 retry=none published_ready=1\r?$') 'room id with a filter substring passes the grammar unchanged'
  Assert ($niceLog -match '(?m)CHILD suppressed=1') 'the canary outside the grammar is still suppressed'
  Assert (-not $niceLog.Contains('a=candidate:deadbeef nice')) 'the canary text never reaches the log'
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $niceState -Recurse -Force -ErrorAction SilentlyContinue
}

# A child-stderr line that merely looks like "COMPONENT EVENT key=value" with
# a disallowed/sensitive field must not pass through unfiltered (pass-through
# leak canaries).
$leakScript = @'
[Console]::Error.WriteLine('COORDINATOR TARGET credential=CANARY')
[Console]::Error.WriteLine('GATEWAY error password=CANARY')
[Console]::Error.WriteLine('TARGET room=room.q2ded.1 id=7 state=terminal reason=credentialCANARY recovery=3 retry=none published_ready=2')
exit 0
'@
$leakState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  Initialize-Q2DzSessionLog -StateDirectory $leakState
  $result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
    -CoordinatorArguments @() -StartCoordinator {
      param($path,$args)
      Start-Q2DzCoordinator -Path $pwshExe -Arguments @('-NoProfile','-Command',$leakScript)
    } -Refresh { return $plan } -PollIntervalSeconds 0
  Assert ($result -eq 0) 'leak-probe run keeps the child exit code'
  $leakLog = Get-Content -LiteralPath (Join-Path $leakState 'logs\q2dz.log') -Raw
  Assert (-not $leakLog.Contains('CANARY')) 'event-shaped lines with a secret field never reach the log'
  Assert (([regex]::Matches($leakLog, 'CHILD suppressed=1')).Count -eq 3) 'all three leak probes are suppressed'
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $leakState -Recurse -Force -ErrorAction SilentlyContinue
}

# A sensitive marker that only survives past the 2048-char truncation point
# must still be suppressed: the sensitivity check must run on the full line,
# not the truncated-for-display prefix.
$truncScript = @'
[Console]::Error.WriteLine('CANARY' + ('x' * 2048) + ' credential=hidden')
exit 0
'@
$truncState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  Initialize-Q2DzSessionLog -StateDirectory $truncState
  $result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
    -CoordinatorArguments @() -StartCoordinator {
      param($path,$args)
      Start-Q2DzCoordinator -Path $pwshExe -Arguments @('-NoProfile','-Command',$truncScript)
    } -Refresh { return $plan } -PollIntervalSeconds 0
  Assert ($result -eq 0) 'truncation-boundary run keeps the child exit code'
  $truncLog = Get-Content -LiteralPath (Join-Path $truncState 'logs\q2dz.log') -Raw
  Assert ($truncLog -match '(?m)CHILD suppressed=1') 'sensitive marker past the truncation point is still suppressed'
  Assert (-not $truncLog.Contains('credential=hidden')) 'the sensitive suffix never reaches the log'
  Assert (-not $truncLog.Contains('CHILD line=CANARY')) 'the line is not logged unsuppressed as a CHILD line'
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $truncState -Recurse -Force -ErrorAction SilentlyContinue
}

# Truncating-before-recognition must not let a long line dodge the event
# grammar check: if the sensitive field only appears past the 2048-char
# cutoff, the line must still be recognised against its full text and
# suppressed, not accepted as a well-formed TARGET event with a canary room=.
$gateScript = @'
[Console]::Error.WriteLine('TARGET room=CANARY ' + (('id=1 ' * 406).Trim()) + ' credential=hidden')
exit 0
'@
$gateState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  Initialize-Q2DzSessionLog -StateDirectory $gateState
  $result = Invoke-Q2DzHub -InitialTargets $plan -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
    -CoordinatorArguments @() -StartCoordinator {
      param($path,$args)
      Start-Q2DzCoordinator -Path $pwshExe -Arguments @('-NoProfile','-Command',$gateScript)
    } -Refresh { return $plan } -PollIntervalSeconds 0
  Assert ($result -eq 0) 'gate-truncation run keeps the child exit code'
  $gateLog = Get-Content -LiteralPath (Join-Path $gateState 'logs\q2dz.log') -Raw
  Assert ($gateLog -match '(?m)CHILD suppressed=1') 'gate truncation bypass line is suppressed'
  Assert (-not $gateLog.Contains('CANARY')) 'the canary room id never reaches the log'
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $gateState -Recurse -Force -ErrorAction SilentlyContinue
}

# A real GATEWAY stop record (all actual fields the Windows gateway emits,
# windows/gateway/src/main.cpp) must be parsed as a structured record and
# must not degrade to the generic CHILD line= fallback, including the
# invalid_cadence_drops field.
$realGatewayStop = 'GATEWAY stop udp_to_web=1 web_to_udp=2 targets=3 max_udp_to_transport_datagram=4 ' +
  'max_transport_to_udp_datagram=5 oversized_udp_drops=6 oversized_transport_drops=7 ' +
  'unknown_peer_drops=8 second_source_drops=9 transport_not_ready_drops=10 invalid_cadence_drops=11'
$gatewayStopResult = ConvertTo-Q2DzChildEvent $realGatewayStop
Assert ($gatewayStopResult -eq $realGatewayStop) 'a real GATEWAY stop record (including invalid_cadence_drops) passes through unchanged, not degraded to CHILD line='
Assert (-not $gatewayStopResult.StartsWith('LAUNCHER CHILD line=')) 'GATEWAY stop record is not degraded to CHILD line='

$launcherText = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\q2dz-launch.ps1') -Raw
Assert ($launcherText.Contains('RedirectStandardInput')) 'launcher redirects coordinator stdin'
Assert ($launcherText.Contains('RedirectStandardError')) 'launcher redirects coordinator stderr'
Assert ($launcherText.Contains('StandardError.ReadLine')) 'launcher reads coordinator stderr asynchronously'
Assert ($launcherText.Contains('ConcurrentQueue')) 'stderr readers only enqueue'
Assert ($launcherText.Contains('StandardInput.BaseStream')) 'launcher writes coordinator stdin bytes'
Assert ($launcherText.Contains('UTF8Encoding')) 'launcher uses explicit UTF-8 without BOM'
Assert ($launcherText.Contains('HUB_POLL_SECONDS')) 'launcher has a bounded poll interval'
Assert ($launcherText.Contains('$lines.Clear()')) 'launcher clears heartbeat line buffers'
Assert ($launcherText.Contains('[Array]::Clear($bytes')) 'launcher clears encoded byte buffers'
Assert (-not $launcherText.Contains('Q2DZ_HUB_MAX_ROOMS')) 'launcher has no fixed-capacity constants'
$legacyTargetType = 'Hub' + [char]83 + 'lot'
Assert (-not $launcherText.Contains($legacyTargetType)) 'launcher has no legacy room model'

# Start-Q2DzLauncher must emit exactly one SESSION stop when Invoke-Q2DzHub
# throws after SESSION start (no enclosing finally previously existed).
$excRoot = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-exc-' + [Guid]::NewGuid().ToString('N'))
$excAppDir = Join-Path $excRoot 'app'
$excGatewayDir = Join-Path $excAppDir 'gateway'
New-Item -ItemType Directory -Force $excGatewayDir | Out-Null
New-Item -ItemType File -Path (Join-Path $excAppDir 'q2dz-session.exe') | Out-Null
New-Item -ItemType File -Path (Join-Path $excAppDir 'q2dz-content-check.exe') | Out-Null
New-Item -ItemType File -Path (Join-Path $excGatewayDir 'q2-doszone-webview2-gateway.exe') | Out-Null
$excLocalAppData = Join-Path $excRoot 'local'
$excStateDir = Join-Path $excLocalAppData 'Q2PRO-X Dos.Zone\state'
New-Item -ItemType Directory -Force $excStateDir | Out-Null
$excFakeQ2 = Join-Path $excRoot 'Q2PRO-X.exe'
New-Item -ItemType File -Path $excFakeQ2 | Out-Null
Set-Content -LiteralPath (Join-Path $excStateDir 'q2-path.txt') -Value $excFakeQ2 -Encoding UTF8

$previousApp = $app
$previousLocalAppData = $env:LOCALAPPDATA
function Get-Q2DzLiveTargets { throw 'test: skip live targets' }
function Invoke-Q2DzHub { throw 'test: boom after SESSION start' }
try {
  $app = $excAppDir
  $env:LOCALAPPDATA = $excLocalAppData
  $threw = $false
  try { Start-Q2DzLauncher | Out-Null } catch { $threw = $true }
  Assert $threw 'Start-Q2DzLauncher propagates the post-start exception'
  $excLog = Get-Content -LiteralPath (Join-Path $excStateDir 'logs\q2dz.log') -Raw
  Assert (([regex]::Matches($excLog, 'SESSION stop ')).Count -eq 1) 'exactly one SESSION stop is emitted after a post-start exception'
  Assert ($excLog -match 'LAUNCHER SESSION stop reason=error rc=1') 'the guarded stop uses a safe reason code and rc'
} finally {
  $app = $previousApp
  $env:LOCALAPPDATA = $previousLocalAppData
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $excRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host 'launcher stdin contract: PASS (dynamic heartbeat, UTF-8, EOF and exit codes)'
