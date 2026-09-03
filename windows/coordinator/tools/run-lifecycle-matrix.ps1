[CmdletBinding()]
param([string]$BuildDir = "$PSScriptRoot/../out/release")

$ErrorActionPreference = 'Stop'
$coord = (Resolve-Path "$BuildDir/q2dz-session.exe").Path
$gateway = (Resolve-Path "$BuildDir/q2dz-fake-gateway.exe").Path
$q2 = (Resolve-Path "$BuildDir/q2dz-fake-q2.exe").Path
$root = Join-Path $env:TEMP ("q2dz-matrix-" + [guid]::NewGuid())
$activeCase = $null
New-Item -ItemType Directory -Force $root | Out-Null

function Marker([string]$dir, [string]$name) { Join-Path $dir "$name.marker" }
function Wait-Path([string]$path, [bool]$present = $true, [int]$timeoutMs = 10000) {
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if ((Test-Path -LiteralPath $path) -eq $present) { return $true }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}
function Room-Json([string]$id) {
  '{"version":1,"roomId":"' + $id + '","alias":"q2.' + $id + '","ordered":false,"iceServers":[],"credential":"q2dz-test-canary-' + $id + '"}'
}
function Send-Command([Diagnostics.Process]$process, [string]$line) {
  $process.StandardInput.WriteLine($line)
  $process.StandardInput.Flush()
}
function Read-Source([string]$url) {
  $client = New-Object System.Net.WebClient
  try { return $client.DownloadString($url) } finally { $client.Dispose() }
}
function Quote-Argument([string]$value) {
  if ($value.Length -gt 0 -and $value -notmatch '[\s"]') { return $value }
  $builder = New-Object System.Text.StringBuilder
  [void]$builder.Append('"')
  $slashes = 0
  foreach ($character in $value.ToCharArray()) {
    if ($character -eq '\') { $slashes++; continue }
    if ($character -eq '"') {
      if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2)) -join '') }
      [void]$builder.Append('\')
      [void]$builder.Append('"')
      $slashes = 0
      continue
    }
    if ($slashes -gt 0) { [void]$builder.Append(('\' * $slashes) -join ''); $slashes = 0 }
    [void]$builder.Append($character)
  }
  if ($slashes -gt 0) { [void]$builder.Append(('\' * ($slashes * 2)) -join '') }
  [void]$builder.Append('"')
  return $builder.ToString()
}
function Wait-SourceCount([string]$url, [int]$expected, [int]$timeoutMs = 10000) {
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    try {
      $body = Read-Source $url
      $count = @($body -split "`r?`n" | Where-Object { $_ -match '^127\.0\.0\.1:[0-9]+$' }).Count
      if ($count -eq $expected) { return $true }
    } catch { }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}
function Get-GenerationDirectory([string]$caseDir, [int]$slot) {
  $sessions = @(Get-ChildItem -LiteralPath (Join-Path $caseDir 'sessions') -Directory -ErrorAction SilentlyContinue)
  if ($sessions.Count -ne 1) { throw "expected one active session directory, got $($sessions.Count)" }
  $generationMatches = @(Get-ChildItem -LiteralPath $sessions[0].FullName -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like "room-$slot-*" })
  if ($generationMatches.Count -ne 1) { throw "expected one active generation for slot $slot, got $($generationMatches.Count)" }
  return $generationMatches[0].FullName
}
function Start-Case([string]$name, [string]$gatewayA = 'normal', [string]$gatewayB = 'normal',
  [string]$q2Mode = 'long-running', [string]$gatewayDefault = 'normal') {
  $dir = Join-Path $root $name
  New-Item -ItemType Directory -Force $dir | Out-Null
  $env:Q2DZ_PID_DIR = $dir
  $env:Q2DZ_FAKE_GATEWAY_MODE = $gatewayDefault
  $env:Q2DZ_FAKE_GATEWAY_MODE_A = $gatewayA
  $env:Q2DZ_FAKE_GATEWAY_MODE_B = $gatewayB
  $env:Q2DZ_FAKE_Q2_MODE = $q2Mode
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $coord
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $arguments = @('--gateway', $gateway, '--q2', $q2, '--session-root', (Join-Path $dir 'sessions'),
    '--ready-timeout-ms', '5000', '--graceful-timeout-ms', '500')
  $psi.Arguments = (($arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $psi
  if (-not $process.Start()) { throw "could not start coordinator for $name" }
  $case = [pscustomobject]@{ Name = $name; Dir = $dir; Process = $process }
  $script:activeCase = $case
  return $case
}
function Stop-Case($case, [int]$expected = 0) {
  try { $case.Process.StandardInput.Close() } catch { }
  if (-not $case.Process.WaitForExit(10000)) {
    Stop-Process -Id $case.Process.Id -Force -ErrorAction SilentlyContinue
    $case.Process.WaitForExit(5000)
  }
  $stdout = $case.Process.StandardOutput.ReadToEnd()
  $stderr = $case.Process.StandardError.ReadToEnd()
  if ($case.Process.ExitCode -ne $expected) {
    throw "$($case.Name) returned $($case.Process.ExitCode), expected $expected`n$stdout`n$stderr"
  }
  Assert-NoOrphans $case.Dir
  $credentialFiles = @(Get-ChildItem -LiteralPath $case.Dir -Recurse -File -ErrorAction SilentlyContinue |
    Select-String -SimpleMatch 'q2dz-test-canary-' | Select-Object -ExpandProperty Path -Unique)
  if ($credentialFiles.Count -gt 0) {
    throw "$($case.Name) persisted a credential canary in $((@($credentialFiles | ForEach-Object { Split-Path -Leaf $_ })) -join ', ')"
  }
  Write-Host "PASS $($case.Name) (exit=$expected)"
  if ($script:activeCase -and $script:activeCase.Process.Id -eq $case.Process.Id) { $script:activeCase = $null }
}
function Stop-MarkedProcesses([string]$dir) {
  foreach ($pidFile in @(Get-ChildItem -LiteralPath $dir -Filter '*.pid' -File -ErrorAction SilentlyContinue)) {
    $text = (Get-Content -LiteralPath $pidFile.FullName -Raw -ErrorAction SilentlyContinue).Trim()
    $processId = 0
    if ([int]::TryParse($text, [ref]$processId) -and $processId -gt 0) {
      Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue
    }
  }
}
function Assert-ProcessAlive([int]$processId, [string]$label) {
  if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) {
    throw "$label process $processId is not alive"
  }
}
function Wait-ProcessGone([int]$processId, [int]$timeoutMs = 10000) {
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) { return $true }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}
function Assert-NoOrphans([string]$dir) {
  Start-Sleep -Milliseconds 700
  foreach ($pidFile in @(Get-ChildItem $dir -Filter '*.pid' -File -ErrorAction SilentlyContinue)) {
    $processId = [int](Get-Content $pidFile.FullName)
    if (Get-Process -Id $processId -ErrorAction SilentlyContinue) { throw "orphan process $processId from $($pidFile.Name)" }
  }
  $sessions = Join-Path $dir 'sessions'
  if (Test-Path $sessions) {
    $left = @(Get-ChildItem $sessions -Force -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) { throw "orphan session data: $($left.FullName -join ', ')" }
  }
}
function Clear-Environment {
  Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_GATEWAY_MODE_A,Env:Q2DZ_FAKE_GATEWAY_MODE_B,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue
}
function Assert-CoordinatorInvariants {
  $sourcePath = Join-Path $PSScriptRoot '../src/windows/main.cpp'
  $sourceText = Get-Content -LiteralPath $sourcePath -Raw
  if ($sourceText -notmatch 'buffer_ = remainder') { throw 'control remainder must be copied before wiping' }
  if ($sourceText -notmatch 'secure_clear_string\(remainder\)') { throw 'control remainder wipe is missing' }
  if ($sourceText -notmatch 'struct BufferWipe') { throw 'raw control buffer wipe is missing' }
  if ($sourceText -notmatch 'ioctlsocket\(client, FIONBIO, &nonblocking\) == SOCKET_ERROR') {
    throw 'accepted HTTP clients must remain nonblocking'
  }
  if ($sourceText -notmatch 'control\.drain\(kHubCommandsPerCycle') {
    throw 'control command drain is not bounded per cycle'
  }
  if ($sourceText -notmatch 'kHubMaxRooms = 5') { throw 'coordinator must accept five room slots' }
  if ($sourceText -notmatch 'dedicated_restarts >= 2') { throw 'dedicated recovery budget is missing' }
  if ($sourceText -match 'events\.finished') { throw 'Queue.finished must not be read without its mutex' }
  Write-Host 'PASS coordinator-source-invariants'
}

try {
  Assert-CoordinatorInvariants

  # Q2 starts once with an empty source; changing slots only changes the HTTP body.
  $case = Start-Case 'live-slots'
  if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw 'Q2 did not read the empty custom source' }
  $q2Pid = [int](Get-Content (Marker $case.Dir 'q2.pid'))
  $source = Get-Content (Marker $case.Dir 'q2-source-url')
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-a.pid'))) { throw 'room A did not start' }
  if (-not (Wait-SourceCount $source 1)) { throw 'source did not publish room A' }
  $roomAPid = [int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid'))
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  Start-Sleep -Milliseconds 100
  if ([int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid')) -ne $roomAPid) { throw 'heartbeat restarted room A' }
  Send-Command $case.Process ("1 room-b " + (Room-Json 'room-b'))
  if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-b.pid'))) { throw 'room B did not start' }
  foreach ($index in 2..4) {
    $id = "room-$index"
    Send-Command $case.Process ("$index $id " + (Room-Json $id))
    if (-not (Wait-Path (Join-Path $case.Dir "gateway-$id.pid"))) { throw "room $index did not start" }
  }
  if (-not (Wait-SourceCount $source 5)) { throw 'source did not publish five rooms' }
  Send-Command $case.Process '0 -'
  if (-not (Wait-SourceCount $source 4)) { throw 'source did not remove room A' }
  if ([int](Get-Content (Marker $case.Dir 'q2.pid')) -ne $q2Pid) { throw 'slot update restarted Q2' }
  Stop-Case $case

  # A dedicated binding gets exactly two recovery starts for every terminal
  # stage: pre-ready protocol failure, post-ready exit and ROOM_CLOSED.
  foreach ($mode in @('malformed-ready','exit-after-ready','room-close-after-ready')) {
    $case = Start-Case "dedicated-$mode" 'normal' 'normal' 'long-running' $mode
    if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw "$mode did not start Q2" }
    $dedicatedId = 'room.q2ded.server'
    $pidPath = Join-Path $case.Dir "gateway-$dedicatedId.pid"
    Send-Command $case.Process ("0 $dedicatedId " + (Room-Json $dedicatedId))
    if (-not (Wait-Path $pidPath)) { throw "$mode initial gateway did not start" }
    $previousPid = [int](Get-Content $pidPath)
    foreach ($recovery in 1..2) {
      if (-not (Wait-ProcessGone $previousPid)) { throw "$mode attempt $recovery did not stop" }
      Send-Command $case.Process ("0 $dedicatedId " + (Room-Json $dedicatedId))
      $deadline = [Environment]::TickCount + 10000
      do {
        $currentPid = [int](Get-Content $pidPath)
        if ($currentPid -ne $previousPid) { break }
        Start-Sleep -Milliseconds 50
      } while ([Environment]::TickCount -lt $deadline)
      if ($currentPid -eq $previousPid) { throw "$mode recovery $recovery did not start" }
      $previousPid = $currentPid
    }
    if (-not (Wait-ProcessGone $previousPid)) { throw "$mode final attempt did not stop" }
    Send-Command $case.Process ("0 $dedicatedId " + (Room-Json $dedicatedId))
    Start-Sleep -Milliseconds 500
    if ([int](Get-Content $pidPath) -ne $previousPid) { throw "$mode tombstoned dedicated restarted" }
    Assert-ProcessAlive ([int](Get-Content (Marker $case.Dir 'q2.pid'))) "$mode Q2"
    Stop-Case $case
  }

  # A burst of slot replacements must not starve a concurrently exiting Q2.
  # Each replacement of the never-ready fake gateway has a bounded 500 ms
  # cleanup, so draining the whole queue would delay the terminal Q2 event.
  $case = Start-Case 'control-burst-terminal' 'normal' 'normal' 'exit-during-burst' 'never-ready'
  if (-not (Wait-Path (Marker $case.Dir 'q2-burst-ready'))) { throw 'burst case did not start Q2' }
  for ($index = 0; $index -lt 12; $index++) {
    $slot = $index % 2
    Send-Command $case.Process ("$slot burst-$index " + (Room-Json "burst-$index"))
  }
  if (-not $case.Process.WaitForExit(2000)) { throw 'control burst starved Q2 termination' }
  Stop-Case $case 74

  # A post-ready crash may be retried once; the next heartbeat is tombstoned.
  $case = Start-Case 'retry' 'exit-after-ready'
  $source = $null
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Get-Content (Marker $case.Dir 'q2-source-url')
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-a.pid'))) { throw 'retry first gateway did not start' }
  $firstPid = [int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid'))
  Wait-Path (Join-Path $case.Dir 'gateway-room-a.pid') | Out-Null
  Start-Sleep -Milliseconds 500
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  Start-Sleep -Milliseconds 500
  $secondPid = [int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid'))
  if ($firstPid -eq $secondPid) { throw 'post-ready failure was not retried' }
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  Start-Sleep -Milliseconds 300
  if ([int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid')) -ne $secondPid) { throw 'tombstoned room was restarted' }
  Stop-Case $case

  # ROOM_CLOSED tombstones a room; clearing the slot permits the same ID again.
  $case = Start-Case 'room-close' 'room-close-after-ready'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Get-Content (Marker $case.Dir 'q2-source-url')
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-a.pid'))) { throw 'room-close gateway did not start' }
  Start-Sleep -Milliseconds 1000
  $closedPid = [int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid'))
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  Start-Sleep -Milliseconds 300
  if ([int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid')) -ne $closedPid) { throw 'ROOM_CLOSED room was restarted' }
  Send-Command $case.Process '0 -'
  Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
  Start-Sleep -Milliseconds 500
  if ([int](Get-Content (Join-Path $case.Dir 'gateway-room-a.pid')) -eq $closedPid) { throw 'clear did not reset tombstone' }
  Stop-Case $case

  # A failed slot is isolated: a healthy second room and Q2 remain alive.
  foreach ($failure in @(
    @{ Name = 'gateway-malformed-ready'; Mode = 'malformed-ready' },
    @{ Name = 'gateway-no-newline-flood'; Mode = 'no-newline-flood' },
    @{ Name = 'gateway-line-flood'; Mode = 'line-flood' },
    @{ Name = 'gateway-fatal'; Mode = 'fatal-immediate' },
    @{ Name = 'gateway-room-closed-immediate'; Mode = 'room-close-immediate' },
    @{ Name = 'gateway-never-ready'; Mode = 'never-ready' }
  )) {
    $case = Start-Case $failure.Name $failure.Mode 'normal'
    if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw "$($failure.Name) did not start Q2" }
    Send-Command $case.Process ("0 room-a " + (Room-Json 'room-a'))
    if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-a.pid'))) { throw "$($failure.Name) did not start room A" }
    Send-Command $case.Process ("1 room-b " + (Room-Json 'room-b'))
    if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-b.pid'))) { throw "$($failure.Name) did not start room B" }
    if (-not (Wait-SourceCount (Get-Content (Marker $case.Dir 'q2-source-url')) 1 10000)) {
      throw "$($failure.Name) did not isolate failed room A"
    }
    $bPid = [int](Get-Content (Join-Path $case.Dir 'gateway-room-b.pid'))
    Assert-ProcessAlive $bPid "$($failure.Name) room B"
    Stop-Case $case
  }

  # Q2 and the other room survive repeated replacement of slot 0.
  $case = Start-Case 'replacements'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  Send-Command $case.Process ("1 room-b " + (Room-Json 'room-b'))
  if (-not (Wait-Path (Join-Path $case.Dir 'gateway-room-b.pid'))) { throw 'room B did not start for replacement test' }
  $bPid = [int](Get-Content (Join-Path $case.Dir 'gateway-room-b.pid'))
  $q2Pid = [int](Get-Content (Marker $case.Dir 'q2.pid'))
  $generationPaths = @{}
  for ($index = 0; $index -lt 20; $index++) {
    Send-Command $case.Process '0 -'
    Send-Command $case.Process ("0 replacement-$index " + (Room-Json "replacement-$index"))
    if (-not (Wait-Path (Join-Path $case.Dir "gateway-replacement-$index.pid"))) {
      throw "replacement $index did not start its gateway"
    }
    $generationPath = $null
    $deadline = [Environment]::TickCount + 10000
    do {
      try { $generationPath = Get-GenerationDirectory $case.Dir 0 } catch { $generationPath = $null }
      if ($generationPath) { break }
      Start-Sleep -Milliseconds 50
    } while ([Environment]::TickCount -lt $deadline)
    if (-not $generationPath) { throw "replacement $index did not create an active generation" }
    if ($generationPaths.ContainsKey($generationPath)) { throw "generation directory reused: $generationPath" }
    $generationPaths[$generationPath] = $true
  }
  if ([int](Get-Content (Marker $case.Dir 'q2.pid')) -ne $q2Pid) { throw 'replacement restarted Q2' }
  if ([int](Get-Content (Join-Path $case.Dir 'gateway-room-b.pid')) -ne $bPid) { throw 'replacement killed room B' }
  Stop-Case $case

  # Source shutdown must remain bounded even when Q2 never opens the URL.
  $case = Start-Case 'no-http-request' 'normal' 'normal' 'no-source-request'
  if (-not (Wait-Path (Marker $case.Dir 'q2-started'))) { throw 'no-http-request did not start Q2' }
  if (Wait-Path (Marker $case.Dir 'q2-source-url') $true 700) { throw 'no-http-request unexpectedly queried source' }
  Stop-Case $case

  # Empty input is a valid live hub; Q2 still starts with NUL stdin.
  $case = Start-Case 'empty-input'
  if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw 'empty hub did not start Q2' }
  Stop-Case $case

  # Hard termination of the coordinator is covered by the inherited cleanup guardian.
  $case = Start-Case 'coordinator-hard-kill'
  if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw 'hard-kill did not start Q2' }
  foreach ($index in 0..4) {
    $id = "hard-$index"
    Send-Command $case.Process ("$index $id " + (Room-Json $id))
    if (-not (Wait-Path (Join-Path $case.Dir "gateway-$id.pid"))) { throw "hard-kill did not start room $index" }
  }
  if (-not (Wait-SourceCount (Get-Content (Marker $case.Dir 'q2-source-url')) 5)) {
    throw 'hard-kill did not publish five rooms'
  }
  $hardSessions = Join-Path $case.Dir 'sessions'
  if (-not (Wait-Path $hardSessions)) { throw 'hard-kill did not create session root' }
  $hardSession = @(Get-ChildItem -LiteralPath $hardSessions -Directory -ErrorAction Stop)
  if ($hardSession.Count -ne 1) { throw "hard-kill expected one session, got $($hardSession.Count)" }
  Stop-Process -Id $case.Process.Id -Force -ErrorAction SilentlyContinue
  if (-not $case.Process.WaitForExit(5000)) { throw 'hard-kill coordinator did not exit' }
  if (-not (Wait-Path $hardSession[0].FullName $false 10000)) { throw 'hard-kill left session data' }
  Assert-NoOrphans $case.Dir
  $script:activeCase = $null
  Write-Host 'PASS coordinator-hard-kill'

  # Q2 failure is propagated and owned processes are cleaned.
  $case = Start-Case 'q2-crash' 'normal' 'normal' 'crash'
  if (-not $case.Process.WaitForExit(10000)) { throw 'q2 crash case did not exit' }
  Stop-Case $case 74

  # Malformed command and partial EOF are protocol failures.
  foreach ($item in @(
    @{ Name = 'malformed'; Payload = '5 -';
      Expected = 13 },
    @{ Name = 'partial'; Payload = '0 room-partial {';
      Expected = 13 },
    @{ Name = 'oversized'; Payload = ('0 room-big ' + ('{' + ('x' * 263000) + '}'));
      Expected = 13 }
  )) {
    $case = Start-Case $item.Name
    if ($item.Name -eq 'partial') {
      $bytes = [Text.Encoding]::UTF8.GetBytes($item.Payload)
      $case.Process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
      $case.Process.StandardInput.Flush()
    } else {
      Send-Command $case.Process $item.Payload
    }
    $case.Process.StandardInput.Close()
    Stop-Case $case ([int]$item.Expected)
  }

  # An incomplete HTTP header cannot hold shutdown.
  $case = Start-Case 'half-header'
  Wait-Path (Marker $case.Dir 'q2-source-url') | Out-Null
  $source = Get-Content (Marker $case.Dir 'q2-source-url')
  $sourceUri = [Uri]$source
  $client = New-Object System.Net.Sockets.TcpClient
  $client.Connect($sourceUri.Host, $sourceUri.Port)
  $stream = $client.GetStream()
  $bytes = [Text.Encoding]::ASCII.GetBytes("GET /servers HTTP/1.1`r`n")
  $stream.Write($bytes, 0, $bytes.Length)
  Stop-Case $case
  $stream.Dispose(); $client.Dispose()

  Write-Host 'WINDOWS LIVE COORDINATOR MATRIX PASS'
} finally {
  if ($activeCase -and -not $activeCase.Process.HasExited) {
    try { $activeCase.Process.StandardInput.Close() } catch { }
    if (-not $activeCase.Process.WaitForExit(3000)) {
      Stop-Process -Id $activeCase.Process.Id -Force -ErrorAction SilentlyContinue
      $activeCase.Process.WaitForExit(3000)
    }
  }
  if ($activeCase) { Stop-MarkedProcesses $activeCase.Dir }
  Clear-Environment
  if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
