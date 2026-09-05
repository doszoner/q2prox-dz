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
function Gateway-PidPath([string]$dir) { Join-Path $dir 'gateway.pid' }

function Wait-Path([string]$path, [bool]$present = $true, [int]$timeoutMs = 10000) {
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if ((Test-Path -LiteralPath $path) -eq $present) { return $true }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}

function Wait-MarkerValue([string]$dir, [string]$name, [string]$expected, [int]$timeoutMs = 10000) {
  $path = Marker $dir $name
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if (Test-Path -LiteralPath $path) {
      try {
        if ((Get-Content -LiteralPath $path -Raw).Trim() -eq $expected) { return $true }
      } catch { }
    }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}

function Wait-GatewayPid([string]$dir, [int]$timeoutMs = 10000) {
  $path = Gateway-PidPath $dir
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if (Test-Path -LiteralPath $path) {
      $text = (Get-Content -LiteralPath $path -Raw).Trim()
      $value = 0
      $ok = [int]::TryParse($text, [ref]$value)
      if ($ok -and $value -gt 0) { return $value }
    }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  throw "shared gateway pid did not appear in $dir"
}

function Wait-SourceUrl([string]$dir, [int]$timeoutMs = 10000) {
  $path = Marker $dir 'q2-source-url'
  if (-not (Wait-Path $path $true $timeoutMs)) {
    throw "Q2 source URL marker did not appear in $dir"
  }
  return Get-Content -LiteralPath $path
}

function Wait-GatewayPidChange([string]$dir, [int]$oldPid, [int]$timeoutMs = 10000) {
  $path = Gateway-PidPath $dir
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if (Test-Path -LiteralPath $path) {
      $text = (Get-Content -LiteralPath $path -Raw).Trim()
      $value = 0
      $ok = [int]::TryParse($text, [ref]$value)
      if ($ok -and $value -gt 0 -and $value -ne $oldPid) { return $value }
    }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  throw "shared gateway did not restart after pid $oldPid"
}

function Wait-ProcessGone([int]$processId, [int]$timeoutMs = 10000) {
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) { return $true }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}

function Assert-ProcessAlive([int]$processId, [string]$label) {
  if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) {
    throw "$label process $processId is not alive"
  }
}

function Room-Json([string]$id) {
  '{"version":1,"roomId":"' + $id + '","alias":"q2.' + $id +
    '","ordered":false,"iceServers":[],"map":"q2dm1","game":"3zb2"}'
}

function Case-Diagnostics([object]$case) {
  $markers = @()
  if (Test-Path -LiteralPath $case.Dir) {
    $markers = Get-ChildItem -LiteralPath $case.Dir -Filter '*.marker' -ErrorAction SilentlyContinue |
      Sort-Object Name |
      ForEach-Object {
        $value = ''
        try { $value = (Get-Content -LiteralPath $_.FullName -Raw).Trim() } catch { }
        "$($_.Name)=$value"
      }
  }
  $exit = if ($case.Process.HasExited) { [string]$case.Process.ExitCode } else { 'running' }
  if (-not $case.Process.HasExited) {
    try { $case.Process.StandardInput.Close() } catch { }
    $case.Process.WaitForExit(3000) | Out-Null
  }
  $stderr = ''
  $stdout = ''
  if ($case.Process.HasExited) {
    try { $stderr = $case.Process.StandardError.ReadToEnd().Trim() } catch { }
    try { $stdout = $case.Process.StandardOutput.ReadToEnd().Trim() } catch { }
  }
  return "case=$($case.Name) coordinator=$exit markers=[$($markers -join '; ')] stderr=[$stderr] stdout=[$stdout]"
}

function Send-Command([Diagnostics.Process]$process, [string]$line) {
  $process.StandardInput.WriteLine($line)
  $process.StandardInput.Flush()
}

function Send-Bytes([Diagnostics.Process]$process, [byte[]]$bytes) {
  $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
  $process.StandardInput.Flush()
}

function Send-Ice([Diagnostics.Process]$process) {
  Send-Command $process 'Q2DZ/ROOMS/2 ICE generation=1 json=[]'
}

function Send-Put([Diagnostics.Process]$process, [string]$roomId) {
  Send-Command $process ('Q2DZ/ROOMS/2 PUT room=' + $roomId + ' json=' + (Room-Json $roomId))
}

function Retry-PutUntilGatewayCount([Diagnostics.Process]$process, [string]$dir, [string]$roomId,
  [string]$count, [int]$timeoutMs = 10000) {
  $deadline = [Environment]::TickCount + $timeoutMs
  do {
    Send-Put $process $roomId
    if (Wait-MarkerValue $dir 'gateway-put-count' $count 100) { return $true }
    Start-Sleep -Milliseconds 50
  } while ([Environment]::TickCount -lt $deadline)
  return $false
}

function Send-Drop([Diagnostics.Process]$process, [string]$roomId) {
  Send-Command $process ('Q2DZ/ROOMS/2 DROP room=' + $roomId)
}

function Read-Source([string]$url) {
  $client = New-Object System.Net.WebClient
  try { return $client.DownloadString($url) } finally { $client.Dispose() }
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

function Start-Case([string]$name, [string]$gatewayA = 'normal', [string]$gatewayB = 'normal',
  [string]$q2Mode = 'long-running', [string]$gatewayDefault = 'normal', [int]$readyMs = 1000) {
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
    '--ready-timeout-ms', [string]$readyMs, '--graceful-timeout-ms', '500')
  $psi.Arguments = (($arguments | ForEach-Object { Quote-Argument $_ }) -join ' ')
  $process = New-Object System.Diagnostics.Process
  $process.StartInfo = $psi
  if (-not $process.Start()) { throw "could not start coordinator for $name" }
  $case = [pscustomobject]@{ Name = $name; Dir = $dir; Process = $process }
  $script:activeCase = $case
  return $case
}

function Assert-NoOrphans([string]$dir) {
  Start-Sleep -Milliseconds 500
  foreach ($pidFile in @(Get-ChildItem -LiteralPath $dir -Filter '*.pid' -File -ErrorAction SilentlyContinue)) {
    $text = (Get-Content -LiteralPath $pidFile.FullName -Raw -ErrorAction SilentlyContinue).Trim()
    $value = 0
    $ok = [int]::TryParse($text, [ref]$value)
    if ($ok -and (Get-Process -Id $value -ErrorAction SilentlyContinue)) {
      throw "orphan process $value from $($pidFile.Name)"
    }
  }
  $sessions = Join-Path $dir 'sessions'
  if (Test-Path -LiteralPath $sessions) {
    $left = @(Get-ChildItem -LiteralPath $sessions -Force -ErrorAction SilentlyContinue)
    if ($left.Count -gt 0) { throw "orphan session data: $($left.FullName -join ', ')" }
  }
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
  if ($credentialFiles.Count -gt 0) { throw "$($case.Name) persisted a credential canary" }
  Write-Host "PASS $($case.Name) (exit=$expected)"
  if ($script:activeCase -and $script:activeCase.Process.Id -eq $case.Process.Id) { $script:activeCase = $null }
}

function Stop-MarkedProcesses([string]$dir) {
  foreach ($pidFile in @(Get-ChildItem -LiteralPath $dir -Filter '*.pid' -File -ErrorAction SilentlyContinue)) {
    $text = (Get-Content -LiteralPath $pidFile.FullName -Raw -ErrorAction SilentlyContinue).Trim()
    $value = 0
    $ok = [int]::TryParse($text, [ref]$value)
    if ($ok -and $value -gt 0) { Stop-Process -Id $value -Force -ErrorAction SilentlyContinue }
  }
}

function Clear-Environment {
  Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_GATEWAY_MODE_A,
    Env:Q2DZ_FAKE_GATEWAY_MODE_B,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue
}

function Assert-CoordinatorInvariants {
  $sourcePath = Join-Path $PSScriptRoot '../src/windows/main.cpp'
  $sourceText = Get-Content -LiteralPath $sourcePath -Raw
  if ($sourceText -match 'kHubMaxRooms|LiveSlot|SlotCommand|room-[0-9]') {
    throw 'coordinator still contains fixed room slots or room-indexed directories'
  }
  foreach ($required in @('struct LiveTarget', 'std::unordered_map<std::string, LiveTarget>',
      'TargetRoutes', 'restart_count', 'start_shared_gateway', 'source.publish({})',
      'control.drain(kHubCommandsPerCycle', 'recovery_count', 'target_recovery_limit',
      'command_bytes_', 'HubCommandFramer::max_queued_bytes', 'queued_count')) {
    if ($sourceText -notmatch [regex]::Escape($required)) { throw "missing coordinator invariant: $required" }
  }
  if ($sourceText -match 'events\.finished') { throw 'Queue.finished must not be read without its mutex' }
  Write-Host 'PASS coordinator-source-invariants'
}

try {
  Assert-CoordinatorInvariants

  # One process serves a dynamic 0 -> 1 -> 2 -> 1 -> 0 target set.
  $case = Start-Case 'dynamic'
  if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw 'Q2 did not read the empty source' }
  $source = Wait-SourceUrl $case.Dir
  $q2Pid = [int](Get-Content (Marker $case.Dir 'q2.pid'))
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  $gatewayPid = Wait-GatewayPid $case.Dir
  if (-not (Wait-SourceCount $source 1)) { throw ('source did not publish room A; ' + (Case-Diagnostics $case)) }
  Send-Put $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 2)) { throw ('source did not publish room B; ' + (Case-Diagnostics $case)) }
  Assert-ProcessAlive $gatewayPid 'shared gateway'
  Assert-ProcessAlive $q2Pid 'Q2'
  Send-Put $case.Process 'room-a'
  Send-Put $case.Process 'room-b'
  Start-Sleep -Milliseconds 250
  if ((Wait-GatewayPid $case.Dir) -ne $gatewayPid) { throw 'heartbeat restarted shared gateway' }
  Send-Drop $case.Process 'room-a'
  if (-not (Wait-SourceCount $source 1)) { throw 'DROP room A removed room B' }
  Assert-ProcessAlive $q2Pid 'Q2 after room A drop'
  Send-Drop $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 0)) { throw 'DROP room B did not empty source' }
  Stop-Case $case

  # An oversized fragment poisons the queue; later valid commands are ignored.
  $case = Start-Case 'protocol-fail-closed'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $oversized = [string]::new('x', 256KB + 1) + "`n"
  $tail = "Q2DZ/ROOMS/2 ICE generation=1 json=[]`n" +
    "Q2DZ/ROOMS/2 PUT room=room-after-error json=" + (Room-Json 'room-after-error') + "`n"
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes($oversized + $tail))
  if (-not $case.Process.WaitForExit(10000)) { throw 'protocol-fail-closed coordinator did not exit' }
  if ((Test-Path -LiteralPath (Marker $case.Dir 'gateway-ice-count')) -or
      (Test-Path -LiteralPath (Marker $case.Dir 'gateway-put-count'))) {
    throw 'protocol-fail-closed executed a command after the oversized fragment'
  }
  Stop-Case $case 13

  # A malformed command after more than one drain-sized batch invalidates the whole batch.
  $case = Start-Case 'protocol-malformed-batch'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $batch = New-Object System.Text.StringBuilder
  [void]$batch.AppendLine('Q2DZ/ROOMS/2 ICE generation=1 json=[]')
  for ($index = 1; $index -le 33; $index++) {
    [void]$batch.AppendLine(('Q2DZ/ROOMS/2 PUT room=room-batch-' + $index + ' json={}'))
  }
  [void]$batch.AppendLine('Q2DZ/ROOMS/2 PUT room=room-malformed json={')
  $batchText = $batch.ToString()
  if ([Text.Encoding]::UTF8.GetByteCount($batchText) -gt 8192) {
    throw 'protocol-malformed-batch must fit one control-reader batch'
  }
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes($batchText))
  if (-not $case.Process.WaitForExit(10000)) { throw 'protocol-malformed-batch coordinator did not exit' }
  if ((Test-Path -LiteralPath (Marker $case.Dir 'gateway-ice-count')) -or
      (Test-Path -LiteralPath (Marker $case.Dir 'gateway-put-count'))) {
    throw 'protocol-malformed-batch executed commands before rejecting the malformed tail'
  }
  Stop-Case $case 13

  # READY may arrive in a different order from PUT; both routes remain isolated.
  $case = Start-Case 'shuffled-ready' 'shuffled-ready' 'shuffled-ready' 'long-running' 'shuffled-ready'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  Send-Put $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 2)) { throw 'shuffled READY did not retain both targets' }
  if (-not (Wait-MarkerValue $case.Dir 'gateway-last-ready-id' '1')) {
    throw 'fake gateway did not deliver shuffled READY (B before A)'
  }
  Stop-Case $case

  # A target FAILED/CLOSED event is isolated from a healthy sibling.
  foreach ($failureMode in @('fatal-immediate', 'room-close-immediate')) {
    $case = Start-Case ("isolated-" + $failureMode) $failureMode 'normal'
    if (-not (Wait-Path (Marker $case.Dir 'q2-source-empty'))) { throw "$failureMode case did not start Q2" }
    $source = Wait-SourceUrl $case.Dir
    Send-Ice $case.Process
    Send-Put $case.Process 'room-a'
    Send-Put $case.Process 'room-b'
    if (-not (Wait-SourceCount $source 1)) { throw "healthy room B was not retained after $failureMode" }
    $gatewayPid = Wait-GatewayPid $case.Dir
    Assert-ProcessAlive $gatewayPid "shared gateway after $failureMode"
    Assert-ProcessAlive ([int](Get-Content (Marker $case.Dir 'q2.pid'))) "Q2 after $failureMode"
    Stop-Case $case
  }

  # Old events after DROP+PUT cannot affect the replacement target.
  $case = Start-Case 'stale-target' 'stale-after-drop' 'normal'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  if (-not (Wait-SourceCount $source 1)) { throw 'stale case initial target was not ready' }
  $gatewayPid = Wait-GatewayPid $case.Dir
  Send-Drop $case.Process 'room-a'
  if (-not (Wait-SourceCount $source 0)) { throw 'stale case DROP did not clear source' }
  Send-Put $case.Process 'room-a'
  if (-not (Wait-SourceCount $source 1)) { throw 'replacement target was not ready' }
  Start-Sleep -Milliseconds 250
  if (-not (Wait-SourceCount $source 1)) { throw 'stale event changed replacement source count' }
  $replacementSource = Read-Source $source
  if ($replacementSource -notmatch '(?m)^127\.0\.0\.1:27912$') {
    throw 'replacement endpoint was not retained after stale events'
  }
  if ($replacementSource -match '(?m)^127\.0\.0\.1:27911$') {
    throw 'old endpoint was retained after stale events'
  }
  if ((Wait-GatewayPid $case.Dir) -ne $gatewayPid) { throw 'stale target caused global restart' }
  Stop-Case $case

  # A pending target timeout removes only that target.
  $case = Start-Case 'target-timeout' 'never-ready' 'normal' 'long-running' 'normal' 300
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  Send-Put $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 1)) { throw 'target timeout did not leave room B' }
  Start-Sleep -Milliseconds 500
  if (-not (Wait-MarkerValue $case.Dir 'gateway-drop-count' '1')) {
    throw 'target timeout did not drop room A after its deadline'
  }
  if (-not (Wait-SourceCount $source 1)) { throw 'timed-out room A was retained with room B' }
  Assert-ProcessAlive ([int](Get-Content (Marker $case.Dir 'q2.pid'))) 'Q2 after target timeout'
  Stop-Case $case

  # Preserve the former bounded q2/q2ded recovery policy across new target ids.
  $case = Start-Case 'bounded-recovery' 'failed-after-ready' 'normal' 'long-running' 'fatal-immediate'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '1')) { throw 'q2 recovery initial PUT missing' }
  if (-not (Retry-PutUntilGatewayCount $case.Process $case.Dir 'room-a' '2')) { throw 'q2 recovery retry missing' }
  Send-Put $case.Process 'room-a'
  Start-Sleep -Milliseconds 250
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '2')) { throw 'q2 recovery was unbounded' }
  if (-not (Wait-MarkerValue $case.Dir 'gateway-last-put-id' '2')) { throw 'q2 recovery did not replace target id' }
  Send-Put $case.Process 'room.q2ded.server'
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '3')) { throw 'q2ded initial PUT missing' }
  if (-not (Retry-PutUntilGatewayCount $case.Process $case.Dir 'room.q2ded.server' '4')) { throw 'q2ded first retry missing' }
  if (-not (Retry-PutUntilGatewayCount $case.Process $case.Dir 'room.q2ded.server' '5')) { throw 'q2ded second retry missing' }
  Send-Put $case.Process 'room.q2ded.server'
  Start-Sleep -Milliseconds 250
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '5')) { throw 'q2ded recovery was unbounded' }
  Stop-Case $case

  # q2 CLOSED is terminal; q2ded CLOSED gets the same two bounded retries as FAILED.
  $case = Start-Case 'bounded-recovery-closed' 'room-close-immediate' 'normal' 'long-running' 'room-close-immediate'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '1')) { throw 'q2 CLOSED initial PUT missing' }
  Send-Put $case.Process 'room-a'
  Start-Sleep -Milliseconds 250
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '1')) { throw 'q2 CLOSED was retried' }
  Send-Put $case.Process 'room.q2ded.server'
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '2')) { throw 'q2ded CLOSED initial PUT missing' }
  if (-not (Retry-PutUntilGatewayCount $case.Process $case.Dir 'room.q2ded.server' '3')) { throw 'q2ded CLOSED first retry missing' }
  if (-not (Retry-PutUntilGatewayCount $case.Process $case.Dir 'room.q2ded.server' '4')) { throw 'q2ded CLOSED second retry missing' }
  Send-Put $case.Process 'room.q2ded.server'
  Start-Sleep -Milliseconds 250
  if (-not (Wait-MarkerValue $case.Dir 'gateway-put-count' '4')) { throw 'q2ded CLOSED recovery was unbounded' }
  Stop-Case $case

  # A partial command from before global recovery is discarded through its newline.
  $case = Start-Case 'partial-recovery'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  $firstGateway = Wait-GatewayPid $case.Dir
  if (-not (Wait-SourceCount $source 1)) { throw 'partial recovery setup was not ready' }
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes('Q2DZ/ROOMS/2 PUT room=room-split json={'))
  Start-Sleep -Milliseconds 250
  Stop-Process -Id $firstGateway -Force -ErrorAction SilentlyContinue
  if (-not (Wait-ProcessGone $firstGateway)) { throw 'partial recovery gateway did not exit' }
  $secondGateway = Wait-GatewayPidChange $case.Dir $firstGateway
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes("}`n"))
  Send-Ice $case.Process
  Send-Put $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 1)) { throw 'suffix after recovery killed subsequent commands' }
  Assert-ProcessAlive $secondGateway 'shared gateway after partial recovery'
  Assert-ProcessAlive ([int](Get-Content (Marker $case.Dir 'q2.pid'))) 'Q2 after partial recovery'
  Stop-Case $case

  # The discarded remainder remains bounded even when it never supplies a newline.
  $case = Start-Case 'partial-recovery-oversized'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  $firstGateway = Wait-GatewayPid $case.Dir
  if (-not (Wait-SourceCount $source 1)) { throw 'oversized recovery setup was not ready' }
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes('Q2DZ/ROOMS/2 PUT room=room-split json={'))
  Start-Sleep -Milliseconds 250
  Stop-Process -Id $firstGateway -Force -ErrorAction SilentlyContinue
  if (-not (Wait-ProcessGone $firstGateway)) { throw 'oversized recovery gateway did not exit' }
  [void](Wait-GatewayPidChange $case.Dir $firstGateway)
  $oversizedRemainder = [string]::new('x', 256KB)
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes($oversizedRemainder))
  if (-not $case.Process.WaitForExit(10000)) { throw 'oversized recovery coordinator did not reject the remainder' }
  Stop-Case $case 13

  # EOF while discarding a recovered partial command is a protocol failure.
  $case = Start-Case 'partial-recovery-eof'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  $firstGateway = Wait-GatewayPid $case.Dir
  if (-not (Wait-SourceCount $source 1)) { throw 'EOF recovery setup was not ready' }
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes('Q2DZ/ROOMS/2 PUT room=room-split json={'))
  Start-Sleep -Milliseconds 250
  Stop-Process -Id $firstGateway -Force -ErrorAction SilentlyContinue
  if (-not (Wait-ProcessGone $firstGateway)) { throw 'EOF recovery gateway did not exit' }
  [void](Wait-GatewayPidChange $case.Dir $firstGateway)
  Send-Bytes $case.Process ([Text.Encoding]::UTF8.GetBytes('}'))
  $case.Process.StandardInput.Close()
  if (-not $case.Process.WaitForExit(10000)) { throw 'EOF recovery coordinator did not reject the discarded partial line' }
  Stop-Case $case 13

  # Exactly one global recovery; no payload replay; second crash disables input.
  $case = Start-Case 'global-restart' 'exit-after-ready' 'normal' 'long-running' 'exit-after-ready'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  $firstGateway = Wait-GatewayPid $case.Dir
  if (-not (Wait-SourceCount $source 1)) { throw 'first gateway never populated source before crash' }
  if (-not (Wait-ProcessGone $firstGateway)) { throw 'first shared gateway did not crash' }
  if (-not (Wait-SourceCount $source 0)) { throw 'global recovery replayed room A' }
  $secondGateway = Wait-GatewayPidChange $case.Dir $firstGateway
  Assert-ProcessAlive $secondGateway 'replacement shared gateway'
  Send-Ice $case.Process
  Send-Put $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 1)) { throw 'normal heartbeat did not restore room B after restart' }
  Assert-ProcessAlive $secondGateway 'shared gateway before second crash'
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  if (-not (Wait-ProcessGone $secondGateway)) { throw 'second shared gateway did not crash' }
  if (-not (Wait-SourceCount $source 0)) { throw 'second crash repopulated source' }
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  Start-Sleep -Milliseconds 250
  if ((Wait-GatewayPid $case.Dir) -ne $secondGateway) { throw 'second crash started a third gateway' }
  Assert-ProcessAlive ([int](Get-Content (Marker $case.Dir 'q2.pid'))) 'Q2 after global crashes'
  Stop-Case $case

  # Q2 exit and owner hard-kill both clean the single gateway tree.
  $case = Start-Case 'q2-exit' 'normal' 'normal' 'exit-normal'
  if (-not ($case.Process.WaitForExit(10000))) { throw 'Q2 exit was not propagated' }
  Stop-Case $case

  $case = Start-Case 'owner-hard-kill'
  Wait-Path (Marker $case.Dir 'q2-source-empty') | Out-Null
  $source = Wait-SourceUrl $case.Dir
  Send-Ice $case.Process
  Send-Put $case.Process 'room-a'
  Send-Put $case.Process 'room-b'
  if (-not (Wait-SourceCount $source 2)) { throw 'hard-kill setup did not publish two targets' }
  $sessionRoot = Join-Path $case.Dir 'sessions'
  $session = @(Get-ChildItem -LiteralPath $sessionRoot -Directory -ErrorAction Stop)
  if ($session.Count -ne 1) { throw 'hard-kill expected one session directory' }
  Stop-Process -Id $case.Process.Id -Force -ErrorAction SilentlyContinue
  if (-not $case.Process.WaitForExit(5000)) { throw 'hard-kill coordinator did not exit' }
  if (-not (Wait-Path $session[0].FullName $false 10000)) { throw 'hard-kill left session data' }
  Assert-NoOrphans $case.Dir
  $script:activeCase = $null
  Write-Host 'PASS owner-hard-kill'

  $case = Start-Case 'q2-crash' 'normal' 'normal' 'crash'
  if (-not $case.Process.WaitForExit(10000)) { throw 'Q2 crash did not exit' }
  Stop-Case $case 74

  # The rooms parser still rejects malformed and incomplete commands.
  $case = Start-Case 'malformed'
  Send-Command $case.Process '5 -'
  $case.Process.StandardInput.Close()
  Stop-Case $case 13

  $case = Start-Case 'partial'
  $bytes = [Text.Encoding]::UTF8.GetBytes('Q2DZ/ROOMS/2 PUT room=room-partial json={')
  $case.Process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
  $case.Process.StandardInput.Flush()
  $case.Process.StandardInput.Close()
  Stop-Case $case 13

  Write-Host 'WINDOWS DYNAMIC COORDINATOR MATRIX PASS'
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
  if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
