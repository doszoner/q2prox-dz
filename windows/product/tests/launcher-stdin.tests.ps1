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

$launcherText = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\q2dz-launch.ps1') -Raw
Assert ($launcherText.Contains('RedirectStandardInput')) 'launcher redirects coordinator stdin'
Assert ($launcherText.Contains('StandardInput.BaseStream')) 'launcher writes coordinator stdin bytes'
Assert ($launcherText.Contains('UTF8Encoding')) 'launcher uses explicit UTF-8 without BOM'
Assert ($launcherText.Contains('HUB_POLL_SECONDS')) 'launcher has a bounded poll interval'
Assert ($launcherText.Contains('$lines.Clear()')) 'launcher clears heartbeat line buffers'
Assert ($launcherText.Contains('[Array]::Clear($bytes')) 'launcher clears encoded byte buffers'
Assert (-not $launcherText.Contains('Q2DZ_HUB_MAX_ROOMS')) 'launcher has no fixed-capacity constants'
$legacyTargetType = 'Hub' + [char]83 + 'lot'
Assert (-not $launcherText.Contains($legacyTargetType)) 'launcher has no legacy room model'

Write-Host 'launcher stdin contract: PASS (dynamic heartbeat, UTF-8, EOF and exit codes)'
