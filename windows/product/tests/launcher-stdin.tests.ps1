[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Assert([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function New-FakeHubWriter {
  param(
    $Process,
    [bool]$FailAfterFirstWrite = $false,
    [Exception]$DisposeError = $null,
    [Exception]$WriteError = $null
  )
  $writer = [pscustomobject]@{
    Process = $Process
    Text = New-Object System.Text.StringBuilder
    Writes = 0
    Flushes = 0
    Disposed = $false
    FailAfterFirstWrite = $FailAfterFirstWrite
    DisposeError = $DisposeError
    WriteError = $WriteError
  }
  $writer | Add-Member ScriptMethod Write {
    param([string]$Value)
    if ($this.Process.HasExited) { throw [IO.IOException]::new('pipe closed') }
    if ($null -ne $this.WriteError) { throw $this.WriteError }
    [void]$this.Text.Append($Value)
    $this.Writes++
    if ($this.FailAfterFirstWrite -and $this.Writes -eq 1) {
      $this.Process.HasExited = $true
      $this.Process.ExitCode = 74
    }
  }
  $writer | Add-Member ScriptMethod Flush {
    if ($this.Process.HasExited) { throw [IO.IOException]::new('pipe closed') }
    $this.Flushes++
  }
  $writer | Add-Member ScriptMethod Dispose {
    $this.Disposed = $true
    if ($null -ne $this.DisposeError) { throw $this.DisposeError }
  }
  return $writer
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
    if (-not $this.HasExited -and $this.Waits -ge $this.ExitAfterWaits) {
      $this.HasExited = $true
    }
    return [bool]$this.HasExited
  }
  $process | Add-Member ScriptMethod Kill {
    $this.Kills++
    $this.HasExited = $true
  }
  $process | Add-Member ScriptMethod CloseMainWindow {
    $this.ClosedWindows++
    return $false
  }
  $process | Add-Member ScriptMethod Dispose { $this.Disposed++ }
  $process | Add-Member ScriptMethod Terminate {
    $this.Terminates++
    $this.HasExited = $true
  }
  return $process
}

. (Join-Path $PSScriptRoot '..\q2dz-launch.ps1')

$dedicatedRoom = [pscustomobject]@{
  room = 'room.q2ded.server.1'; owner = 0; players = 1
  createSettings = [pscustomobject]@{ prefix = 'q2ded'; name = 'Server'; map = 'q2dm1'; maxPlayers = 16 }
}
$dedicatedRtc = @(
  [pscustomobject]@{ domain = 'a.example.invalid'; iceservers = @([pscustomobject]@{ urls = @('stun:shared.example.invalid') }) },
  [pscustomobject]@{ domain = 'b.example.invalid'; iceservers = @(
    [pscustomobject]@{ urls = @('stun:shared.example.invalid') },
    [pscustomobject]@{ urls = @('stun:b.example.invalid') }
  ) }
)
$dedicatedJson = New-Q2DzRoomConfig -Room $dedicatedRoom `
  -LookupPayload ([pscustomobject]@{ aliases = @($dedicatedRoom.room) }) -RtcPayload $dedicatedRtc
$dedicatedConfig = $dedicatedJson | ConvertFrom-Json
Assert ($dedicatedConfig.alias -ceq $dedicatedRoom.room) 'dedicated config uses the exact room alias'
Assert (@($dedicatedConfig.iceServers).Count -eq 2) 'dedicated config contains stable deduplicated ICE entries'
Assert ($dedicatedConfig.game -ceq '3zb2' -and -not $dedicatedConfig.ordered) 'dedicated config keeps local game invariants'

$oversizedRtc = @([pscustomobject]@{
  domain = 'bulk.example.invalid'
  iceservers = @(0..2 | ForEach-Object {
    $server = $_
    [pscustomobject]@{ urls = @(0..15 | ForEach-Object {
      "turn:s$server.example.invalid?x=$(('x' * 1600))$_"
    }) }
  })
})
$oversizedIce = @(Get-Q2DzDedicatedIceServers $oversizedRtc)
Assert ($oversizedIce.Count -eq 3) 'oversized fixture passes RTC and aggregate limits'
$oversizedThrown = $false
try {
  New-Q2DzRoomConfig -Room $dedicatedRoom -LookupPayload $dedicatedLookup -RtcPayload $oversizedRtc | Out-Null
} catch { $oversizedThrown = $true }
Assert $oversizedThrown 'dedicated compact config over 65535 bytes is rejected'

$legacyRoom = [pscustomobject]@{
  room = 'room.q2.client.1'; owner = '1'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Client'; map = 'q2dm1'; maxPlayers = 16 }
}
$excessRtc = @(
  [pscustomobject]@{ domain = 'lvc.example.invalid'; iceservers = @(
    [pscustomobject]@{ urls = @('stun:legacy.example.invalid') }
  ) },
  [pscustomobject]@{ domain = 'bulk.example.invalid'; iceservers = @(0..15 | ForEach-Object {
    [pscustomobject]@{ urls = @("stun:s$_.example.invalid") }
  }) }
)
$emptySlots = @(0..4 | ForEach-Object { New-Q2DzHubSlot })
$degraded = @(Get-Q2DzLiveHubSlots -Slots $emptySlots -Q2Path 'C:\Games\Q2PRO-X.exe' `
  -ContentCheckPath 'q2dz-content-check.exe' -FetchJson {
    param($Endpoint)
    if ($Endpoint -ceq $Q2DZ_ROOM_ENDPOINT) { return [pscustomobject]@{ rooms = @($dedicatedRoom,$legacyRoom) } }
    if ($Endpoint -ceq $Q2DZ_LOOKUP_ENDPOINT) { return [pscustomobject]@{ aliases = @($dedicatedRoom.room,'q2.1') } }
    if ($Endpoint -ceq $Q2DZ_RTC_ENDPOINT) { return $excessRtc }
    throw "unexpected endpoint: $Endpoint"
  } -RunContentCheck { return $true })
Assert (@($degraded[0..2] | Where-Object { $_.RoomId }).Count -eq 0) 'excess aggregate ICE hides dedicated rooms'
Assert ($degraded[3].RoomId -ceq $legacyRoom.room) 'excess aggregate ICE does not block legacy q2 reconcile'

$initial = @(
  (New-Q2DzHubSlot), (New-Q2DzHubSlot), (New-Q2DzHubSlot),
  (New-Q2DzHubSlot 'room.q2.a' '{"roomId":"room.q2.a"}'),
  (New-Q2DzHubSlot)
)

# A successful refresh writes both independent slot commands again.  The
# process exits after the next wait; no timer thread is involved.
$process = New-FakeHubProcess -ExitAfterWaits 2
$writer = New-FakeHubWriter $process
$owner = [pscustomobject]@{ Process = $process; Writer = $writer }
$refreshes = 0
$refresh = {
  param($slots)
  $script:refreshes++
  return @(
    (New-Q2DzHubSlot), (New-Q2DzHubSlot), (New-Q2DzHubSlot),
    (New-Q2DzHubSlot 'room.q2.a' $slots[3].Config),
    (New-Q2DzHubSlot 'room.q2.b' '{"roomId":"room.q2.b"}')
  )
}
$result = Invoke-Q2DzHub -InitialSlots $initial -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { $owner } -Refresh $refresh -PollIntervalSeconds 0
Assert ($result -eq 0) 'normal coordinator exit must be returned'
Assert ($refreshes -eq 1) 'one successful poll must be performed'
Assert ($writer.Disposed) 'launcher cleanup must close coordinator stdin'
$lines = $writer.Text.ToString().Split("`n", [StringSplitOptions]::RemoveEmptyEntries)
Assert ($lines.Count -eq 10) "expected five initial and five heartbeat commands, got $($lines.Count)"
Assert ($lines[3].StartsWith('3 room.q2.a ')) 'legacy room uses slot three'
Assert ($lines[4] -eq "4 -") 'fifth slot command is independent'
Assert ($lines[8].StartsWith('3 room.q2.a ')) 'heartbeat keeps legacy slot three'
Assert ($lines[9].StartsWith('4 room.q2.b ')) 'heartbeat fills legacy slot four'

# A catalog failure retains state and sends no heartbeat commands.
$process = New-FakeHubProcess -ExitAfterWaits 1
$writer = New-FakeHubWriter $process
$owner = [pscustomobject]@{ Process = $process; Writer = $writer }
$result = Invoke-Q2DzHub -InitialSlots $initial -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { $owner } -Refresh { throw 'catalog unavailable' } -PollIntervalSeconds 0
Assert ($result -eq 0) 'catalog failure must not change coordinator exit'
$lines = $writer.Text.ToString().Split("`n", [StringSplitOptions]::RemoveEmptyEntries)
Assert ($lines.Count -eq 5) 'catalog failure must not send destructive heartbeat'

# Exit 74 before reading stdin and immediately after the first command must
# survive the launcher, including the broken-pipe path.
$process = New-FakeHubProcess -ExitAfterWaits 1 -ExitCode 74 -AlreadyExited $true
$writer = New-FakeHubWriter $process
$owner = [pscustomobject]@{ Process = $process; Writer = $writer }
$result = Invoke-Q2DzHub -InitialSlots $initial -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { $owner } -Refresh { return $initial } -PollIntervalSeconds 0
Assert ($result -eq 74) 'coordinator exit before stdin read must be preserved'

$process = New-FakeHubProcess -ExitAfterWaits 1
$writer = New-FakeHubWriter $process -FailAfterFirstWrite $true
$owner = [pscustomobject]@{ Process = $process; Writer = $writer }
$result = Invoke-Q2DzHub -InitialSlots $initial -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
  -CoordinatorArguments @() -StartCoordinator { $owner } -Refresh { return $initial } -PollIntervalSeconds 0
Assert ($result -eq 74) 'coordinator exit after first command must be preserved'

# A writer-disposal failure is reported only after all bounded process cleanup
# steps have run; cleanup must not be skipped or mask the original exception.
$process = New-FakeHubProcess -ExitAfterWaits 1000
$writeError = [InvalidOperationException]::new('initial write failed')
$disposeError = [InvalidOperationException]::new('writer dispose failed')
$writer = New-FakeHubWriter $process -DisposeError $disposeError -WriteError $writeError
$owner = [pscustomobject]@{ Process = $process; Writer = $writer }
$thrown = $null
try {
  Invoke-Q2DzHub -InitialSlots $initial -Q2Path 'Q2PRO-X.exe' -CoordinatorPath 'q2dz-session.exe' `
    -CoordinatorArguments @() -StartCoordinator { $owner } -Refresh { return $initial } -PollIntervalSeconds 0 | Out-Null
} catch { $thrown = $_.Exception }
Assert ($null -ne $thrown -and $thrown.Message -eq 'writer dispose failed') 'writer disposal error is preserved'
Assert ($process.Waits -ge 2) 'cleanup performs bounded waits after writer disposal failure'
Assert ($process.ClosedWindows -eq 1) 'cleanup requests graceful coordinator termination'
Assert ($process.Kills -eq 1) 'cleanup force-kills only after graceful termination timeout'
Assert ($process.Disposed -eq 1) 'cleanup always disposes the coordinator process'

# Keep a real child-process check next to the unit fakes.  This exercises the
# actual OS pipe, StreamWriter encoding, independent slot lines, and EOF.
$shellPath = $null
foreach ($candidate in @('pwsh', 'pwsh.exe', 'powershell.exe', 'powershell')) {
  $command = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($command) {
    $shellPath = if ($command.Source) { [string]$command.Source } else { [string]$command.Path }
    break
  }
}
Assert ($null -ne $shellPath -and $shellPath.Length -gt 0) 'PowerShell executable is available for the OS-pipe integration test'
$pipeRoot = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-launcher-pipe-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $pipeRoot -Force | Out-Null
$childScript = Join-Path $pipeRoot 'capture.ps1'
$capturePath = Join-Path $pipeRoot 'stdin.bin'
$eofPath = Join-Path $pipeRoot 'eof.txt'
$captureScript = @'
param(
  [Parameter(Mandatory = $true)][string]$Capture,
  [Parameter(Mandatory = $true)][string]$Eof
)
$inputStream = [Console]::OpenStandardInput()
$memory = New-Object System.IO.MemoryStream
$inputStream.CopyTo($memory)
[IO.File]::WriteAllBytes($Capture, $memory.ToArray())
[IO.File]::WriteAllText($Eof, 'eof', [Text.Encoding]::ASCII)
'@
[IO.File]::WriteAllText($childScript, $captureScript, (New-Object System.Text.UTF8Encoding($false)))
$pipeOwner = $null
try {
  $pipeOwner = Start-Q2DzCoordinator -Path $shellPath -Arguments @(
    '-NoProfile', '-NonInteractive', '-File', $childScript,
    '-Capture', $capturePath, '-Eof', $eofPath
  )
  $utf8Canary = -join [char[]](0x0442, 0x0435, 0x0441, 0x0442)
  $pipeConfig = '{"name":"' + $utf8Canary + '","map":"q2dm1"}'
  $pipeSlots = @(
    (New-Q2DzHubSlot), (New-Q2DzHubSlot), (New-Q2DzHubSlot),
    (New-Q2DzHubSlot 'room.q2.pipe' $pipeConfig), (New-Q2DzHubSlot)
  )
  $pipeCode = Write-Q2DzHubSlots -Process $pipeOwner.Process -Writer $pipeOwner.Writer -Slots $pipeSlots
  Assert ($null -eq $pipeCode) 'live child remains running until stdin EOF'
  $pipeOwner.Writer.Dispose()
  Assert ($pipeOwner.Process.WaitForExit(10000)) 'live child observes stdin EOF and exits'
  Assert ([int]$pipeOwner.Process.ExitCode -eq 0) 'live child exits successfully after EOF'
  Assert (Test-Path -LiteralPath $capturePath -PathType Leaf) 'live child captured coordinator stdin'
  Assert (Test-Path -LiteralPath $eofPath -PathType Leaf) 'live child recorded EOF'
  $capturedBytes = [IO.File]::ReadAllBytes($capturePath)
  $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
  $capturedText = $strictUtf8.GetString($capturedBytes)
  $expectedText = -join @(0..4 | ForEach-Object { ConvertTo-Q2DzHubCommand $_ $pipeSlots[$_] })
  Assert ($capturedText -ceq $expectedText) 'live child received exact independent UTF-8 slot commands'
  Assert ($capturedBytes.Length -lt 3 -or
    $capturedBytes[0] -ne 0xEF -or $capturedBytes[1] -ne 0xBB -or $capturedBytes[2] -ne 0xBF) 'live child input has no UTF-8 BOM'
  Assert ($capturedText.Contains($utf8Canary)) 'live child received UTF-8 room configuration'
  Assert (-not $capturedText.Contains("`r")) 'live child input uses LF line endings'
  Assert ([IO.File]::ReadAllText($eofPath) -ceq 'eof') 'live child completed its EOF path'
}
finally {
  if ($pipeOwner) {
    if (-not $pipeOwner.Process.HasExited) {
      try { $pipeOwner.Process.Kill() } catch {}
      try { $pipeOwner.Process.WaitForExit(5000) } catch {}
    }
    try { $pipeOwner.Writer.Dispose() } catch {}
    try { $pipeOwner.Process.Dispose() } catch {}
  }
  if (Test-Path -LiteralPath $pipeRoot) { Remove-Item -LiteralPath $pipeRoot -Recurse -Force }
}

$launcherText = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\q2dz-launch.ps1') -Raw
Assert ($launcherText.Contains('RedirectStandardInput')) 'launcher redirects coordinator stdin'
Assert ($launcherText.Contains('StandardInput.BaseStream')) 'launcher uses coordinator stdin'
Assert ($launcherText.Contains('UTF8Encoding')) 'launcher uses explicit UTF-8 encoding'
Assert ($launcherText.Contains('WaitForExit(1000)')) 'launcher uses bounded exit polling'
Assert ($launcherText.Contains('HUB_POLL_SECONDS')) 'launcher has a fifteen-second poll interval'
Assert (-not $launcherText.Contains('There are no joinable Quake II rooms right now.')) 'launcher has no zero-room early exit'

Write-Host 'launcher stdin contract: PASS (slots, heartbeats, UTF-8, EOF and exit codes)'
