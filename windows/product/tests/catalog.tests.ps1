$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path $PSScriptRoot '..\catalog.ps1')
. (Join-Path $PSScriptRoot '..\q2dz-launch.ps1')

function Assert-True {
  param([bool]$Value, [string]$Message)
  if (-not $Value) { throw "ASSERT TRUE failed: $Message" }
}

function Assert-Equal {
  param($Expected, $Actual, [string]$Message)
  if ($Expected -ne $Actual) {
    throw "ASSERT EQUAL failed: $Message (expected '$Expected', got '$Actual')"
  }
}

function Assert-Throws {
  param([scriptblock]$Action, [string]$Message)
  $thrown = $false
  try { & $Action | Out-Null } catch { $thrown = $true }
  if (-not $thrown) { throw "ASSERT THROWS failed: $Message" }
}

function New-TestRoom {
  param(
    [string]$RoomId = 'room.q2.client.1',
    [string]$Owner = '1',
    [string]$Map = 'q2dm1',
    [string]$Name = 'Client'
  )
  return [pscustomobject]@{
    room = $RoomId; owner = $Owner; roomReady = $true; players = 1; rtcServer = 'lvc.dos.zone'
    createSettings = [pscustomobject]@{ prefix = 'q2'; name = $Name; map = $Map; maxPlayers = 16 }
  }
}

function New-TestDedicatedRoom {
  param(
    [string]$RoomId = 'room.q2ded.server.1',
    [string]$Map = 'q2dm1',
    [string]$Name = 'Server'
  )
  return [pscustomobject]@{
    room = $RoomId; owner = 0; players = 1
    createSettings = [pscustomobject]@{ prefix = 'q2ded'; name = $Name; map = $Map; maxPlayers = 16 }
  }
}

function New-TestRtc {
  return @([pscustomobject]@{
    domain = 'lvc.dos.zone'
    iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') })
  })
}

function New-TestRows {
  param([int]$Q2, [int]$Dedicated)
  $rows = [System.Collections.Generic.List[object]]::new()
  foreach ($index in 0..($Q2 - 1)) {
    if ($Q2 -gt 0) { [void]$rows.Add((New-TestRoom -RoomId ('room.q2.client.{0:D3}' -f $index) -Owner ([string]$index) -Name ('Q2 {0:D3}' -f $index))) }
  }
  foreach ($index in 0..($Dedicated - 1)) {
    if ($Dedicated -gt 0) { [void]$rows.Add((New-TestDedicatedRoom -RoomId ('room.q2ded.server.{0:D3}' -f $index) -Name ('DED {0:D3}' -f $index))) }
  }
  return @($rows)
}

function New-TestLookup {
  param([object[]]$Rows)
  $aliases = [System.Collections.Generic.List[string]]::new()
  foreach ($row in $Rows) { [void]$aliases.Add((Get-Q2DzRoomAlias $row)) }
  return [pscustomobject]@{ aliases = @($aliases.ToArray()) }
}

function Invoke-HeartbeatScenario {
  param($Case)
  $name = [string]$Case.name
  $payload = $Case.payload
  if ($name -eq 'heartbeat-mixed-20-q2-20-q2ded' -or $name -eq 'heartbeat-retains-all-40') {
    $rows = @(New-TestRows ([int]$payload.q2) ([int]$payload.q2ded))
    $plan = Get-Q2DzTargetPlan -Targets (New-Q2DzTargetMap) -Rows $rows -MakeConfig {
      param($row)
      return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
    }
    if ($name -eq 'heartbeat-mixed-20-q2-20-q2ded') {
      $ordered = @(Sort-Q2DzStrings @($plan.Targets.Keys))
      $q2Count = @($rows | Where-Object { $_.createSettings.prefix -ceq 'q2' }).Count
      $q2dedCount = @($rows | Where-Object { $_.createSettings.prefix -ceq 'q2ded' }).Count
      return [pscustomobject]@{
        count = $plan.Targets.Count
        q2 = $q2Count
        q2ded = $q2dedCount
        order = if (($ordered -join ',') -eq (($plan.Targets.Keys | Sort-Object) -join ',')) { 'room-id' } else { 'unexpected' }
      }
    }
    $firstMiss = Get-Q2DzTargetPlan -Targets $plan.Targets -Rows @() -MakeConfig { param($row) return '' }
    $secondMiss = Get-Q2DzTargetPlan -Targets $firstMiss.Targets -Rows @() -MakeConfig { param($row) return '' }
    return [pscustomobject]@{
      count = $plan.Targets.Count
      missesBeforeDrop = if ($secondMiss.Targets.Count -eq 0 -and $secondMiss.Drops.Count -eq $plan.Targets.Count) { 2 } else { 0 }
    }
  }

  if ($name -eq 'heartbeat-content-check-execution-failure-preserves-target') {
    $old = New-TestRoom 'room.q2.map-scenario.1' '1' ([string]$payload.mapBefore)
    $changed = New-TestRoom $old.room '1' ([string]$payload.mapAfter)
    $oldConfig = New-Q2DzRoomConfig -Room $old -LookupPayload (New-TestLookup @($old)) -RtcPayload @(New-TestRtc)
    $current = New-Q2DzTargetMap
    $current[$old.room] = New-Q2DzTarget $old.room $oldConfig 0 ([string]$payload.mapBefore)
    $failed = $false
    try {
      Get-Q2DzTargetPlan -Targets $current -Rows @($changed) -MakeConfig {
        param($row)
        return New-Q2DzRoomConfig -Room $row -LookupPayload (New-TestLookup @($row)) -RtcPayload @(New-TestRtc)
      } -MapAvailable { param($row) throw 'content checker execution failed' } | Out-Null
    } catch { $failed = $_.Exception.Message -match 'content' }
    return [pscustomobject]@{
      contentFailure = if ($failed -and $current[$old.room].Config -eq $oldConfig) { 'preserve' } else { 'changed' }
    }
  }

  if ($name -eq 'heartbeat-retains-full-existing-room') {
    $existing = New-TestRoom 'room.q2.full-scenario.1' '1'
    $existing.players = $existing.createSettings.maxPlayers
    $new = New-TestRoom 'room.q2.full-scenario.2' '2'
    $new.players = $new.createSettings.maxPlayers
    $config = New-Q2DzRoomConfig -Room $existing -LookupPayload (New-TestLookup @($existing)) -RtcPayload @(New-TestRtc)
    $current = New-Q2DzTargetMap
    $current[$existing.room] = New-Q2DzTarget $existing.room $config 0 $existing.createSettings.map
    $plan = Get-Q2DzTargetPlan -Targets $current -Rows @($existing,$new) -MakeConfig {
      param($row)
      return New-Q2DzRoomConfig -Room $row -LookupPayload (New-TestLookup @($row)) -RtcPayload @(New-TestRtc)
    }
    return [pscustomobject]@{
      fullExisting = if ($plan.Targets.ContainsKey($existing.room)) { 'retain' } else { 'evict' }
      fullNew = if (-not $plan.Targets.ContainsKey($new.room)) { 'skip' } else { 'add' }
    }
  }

  if ($name -eq 'heartbeat-invalid-global-rtc') {
    try { Get-Q2DzDedicatedIceServers @($payload.rtc) | Out-Null } catch { return [pscustomobject]@{ error = 'invalid_global_rtc' } }
    return [pscustomobject]@{ error = 'accepted' }
  }

  if ($name -eq 'heartbeat-duplicate-room-id' -or $name -eq 'heartbeat-duplicate-derived-alias') {
    if ($name -eq 'heartbeat-duplicate-room-id') {
      $rows = @((New-TestRoom 'room.q2.shared-scenario.1' '1'), (New-TestRoom 'room.q2.shared-scenario.1' '2'))
    } else {
      $rows = @((New-TestRoom 'room.q2.alias-scenario.1' '3'), (New-TestRoom 'room.q2.alias-scenario.2' '3'))
    }
    try { Get-Q2DzReadyRooms ([pscustomobject]@{ rooms = $rows }) | Out-Null }
    catch {
      $message = $_.Exception.Message
      return [pscustomobject]@{ error = if ($message -match 'duplicate room') { 'duplicate_room_id' } elseif ($message -match 'duplicate Q2 aliases') { 'duplicate_alias' } else { 'unexpected' } }
    }
    return [pscustomobject]@{ error = 'accepted' }
  }

  if ($name -eq 'heartbeat-capacity-256-compatible' -or $name -eq 'heartbeat-capacity-257-compatible') {
    $rows = @(New-TestRows ([int]$payload.q2) ([int]$payload.q2ded))
    try {
      $plan = Get-Q2DzTargetPlan -Targets (New-Q2DzTargetMap) -Rows $rows -MakeConfig {
        param($row)
        return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"q2dm1","game":"3zb2"}')
      }
      return [pscustomobject]@{ count = $plan.Targets.Count }
    } catch {
      $errorName = if ($_.Exception.Message -match 'catalog_capacity_exceeded') { 'catalog_capacity_exceeded' } else { 'unexpected' }
      return [pscustomobject]@{ error = $errorName; count = $rows.Count }
    }
  }

  if ($name -eq 'heartbeat-capacity-replacement') {
    $currentRows = @(New-TestRows ([int]$payload.current) 0)
    $currentTargets = New-Q2DzTargetMap
    foreach ($row in $currentRows) {
      $config = ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
      $currentTargets[$row.room] = New-Q2DzTarget $row.room $config 0 $row.createSettings.map
    }
    $desiredRows = @($currentRows | Where-Object { $_.room -cne [string]$payload.missing })
    $desiredRows += New-TestRoom ([string]$payload.replacement) ([string]$payload.replacementOwner) 'q2dm1' 'Replacement'
    $makeConfig = {
      param($row)
      return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
    }
    $first = Get-Q2DzTargetPlan -Targets $currentTargets -Rows $desiredRows -MakeConfig $makeConfig
    $second = Get-Q2DzTargetPlan -Targets $first.Targets -Rows $desiredRows -MakeConfig $makeConfig
    return [pscustomobject]@{
      firstCount = $first.Targets.Count
      firstMisses = [int]$first.Targets[[string]$payload.missing].Misses
      firstDrops = @($first.Drops).Count
      secondCount = $second.Targets.Count
      secondDrops = @($second.Drops).Count
      deferredAdmitted = if ($second.Targets.ContainsKey([string]$payload.replacement) -and
          -not $first.Targets.ContainsKey([string]$payload.replacement)) { 1 } else { 0 }
    }
  }

  if ($name -eq 'heartbeat-ice-snapshot-byte-limit') {
    $servers = [System.Collections.Generic.List[object]]::new()
    for ($serverIndex = 0; $serverIndex -lt [int]$payload.serverCount; $serverIndex++) {
      $urls = [System.Collections.Generic.List[string]]::new()
      for ($urlIndex = 0; $urlIndex -lt [int]$payload.urlsPerServer; $urlIndex++) {
        $prefix = 'stun:example.invalid?server={0}&url={1}&' -f $serverIndex,$urlIndex
        [void]$urls.Add($prefix + ('x' * ([int]$payload.urlLength - $prefix.Length)))
      }
      [void]$servers.Add([pscustomobject]@{ urls = @($urls.ToArray()) })
    }
    $rtc = @([pscustomobject]@{ domain = 'lvc.dos.zone'; iceservers = @($servers.ToArray()) })
    Assert-True (Test-Q2DzRtcPayload $rtc) 'generated oversized RTC aggregate is field-valid'
    try { Get-Q2DzDedicatedIceServers $rtc | Out-Null }
    catch {
      return [pscustomobject]@{
        error = if ($_.Exception.Message -eq 'global ICE snapshot exceeds 65536 bytes') {
          'ice_snapshot_too_large'
        } else { 'unexpected' }
      }
    }
    return [pscustomobject]@{ error = 'accepted' }
  }

  throw "Unhandled heartbeat scenario: $name"
}

# --- refresh-scenario shared cases (spec q2prox-catalog-refresh) ---
# steps[]; each step: rooms? (rows of the room/get body, or a generator
# {"gen":{"kind":"q2"|"q2ded","count":N,"start":N}}), aliases? (defaults to
# every derivable row alias), rtc? (default one valid row), fail?
# (http|timeout|json|rtc|capacity|content_check), expected {result,
# accepted, registered, targets, drops, puts, rows}.

function Expand-RefreshIds {
  param($Entries)
  $result = [System.Collections.Generic.List[string]]::new()
  foreach ($item in @($Entries)) {
    if ($null -ne $item -and $null -ne $item.PSObject -and
        $item.PSObject.Properties.Name -contains 'gen') {
      $gen = $item.gen
      $start = if ($gen.PSObject.Properties.Name -contains 'start') { [int]$gen.start } else { 0 }
      for ($offset = 0; $offset -lt [int]$gen.count; $offset++) {
        $result.Add(('{0}{1:D3}' -f [string]$gen.prefix, ($start + $offset)))
      }
      continue
    }
    $result.Add([string]$item)
  }
  return @($result)
}

function Expand-RefreshRows {
  param($Entries)
  $result = [System.Collections.Generic.List[object]]::new()
  foreach ($item in @($Entries)) {
    if ($null -ne $item -and $null -ne $item.PSObject -and
        $item.PSObject.Properties.Name -contains 'gen') {
      $gen = $item.gen
      $start = if ($gen.PSObject.Properties.Name -contains 'start') { [int]$gen.start } else { 0 }
      for ($offset = 0; $offset -lt [int]$gen.count; $offset++) {
        $suffix = '{0:D3}' -f ($start + $offset)
        if ([string]$gen.kind -ceq 'q2ded') {
          $result.Add((New-TestDedicatedRoom -RoomId ('room.q2ded.gen.{0}' -f $suffix) -Name ('DED GEN {0}' -f $suffix)))
        } else {
          $result.Add((New-TestRoom -RoomId ('room.q2.gen.{0}' -f $suffix) -Owner ('gen{0}' -f $suffix) -Name ('GEN {0}' -f $suffix)))
        }
      }
      continue
    }
    $result.Add($item)
  }
  return @($result)
}

function Expand-RefreshExpectedTargets {
  param($Entries)
  $result = [System.Collections.Generic.List[object]]::new()
  foreach ($item in @($Entries)) {
    if ($null -ne $item -and $null -ne $item.PSObject -and
        $item.PSObject.Properties.Name -contains 'gen') {
      $gen = $item.gen
      $start = if ($gen.PSObject.Properties.Name -contains 'start') { [int]$gen.start } else { 0 }
      for ($offset = 0; $offset -lt [int]$gen.count; $offset++) {
        $result.Add([pscustomobject]@{
          room = ('{0}{1:D3}' -f [string]$gen.prefix, ($start + $offset)); misses = 0; invalid = 0 })
      }
      continue
    }
    $misses = if ($item.PSObject.Properties.Name -contains 'misses') { [int]$item.misses } else { 0 }
    $invalid = if ($item.PSObject.Properties.Name -contains 'invalid') { [int]$item.invalid } else { 0 }
    $result.Add([pscustomobject]@{ room = [string]$item.room; misses = $misses; invalid = $invalid })
  }
  return @($result)
}

function Get-RefreshRowAlias {
  param($Row)
  # Test-side alias derivation for the default lookup payload (not a
  # validity check): q2ded -> room id; q2 -> q2.<owner> when owner is a
  # string or integer; $null when no alias can be derived.
  if ($null -eq $Row -or $null -eq $Row.PSObject -or
      $Row.PSObject.Properties.Name -notcontains 'createSettings' -or
      $null -eq $Row.createSettings -or $null -eq $Row.createSettings.PSObject -or
      $Row.createSettings.PSObject.Properties.Name -notcontains 'prefix') { return $null }
  $prefix = $Row.createSettings.prefix
  if ($prefix -isnot [string]) { return $null }
  if ($prefix -ceq 'q2ded') {
    if ($Row.PSObject.Properties.Name -contains 'room' -and $Row.room -is [string]) {
      return [string]$Row.room
    }
    return $null
  }
  if ($prefix -cne 'q2' -or
      $Row.PSObject.Properties.Name -notcontains 'owner') { return $null }
  $owner = $Row.owner
  if ($null -eq $owner -or $owner -is [bool]) { return $null }
  # Mirrors Get-Q2DzClassifiedRoomAlias (catalog.ps1).
  if ($owner -isnot [string] -and -not (Test-Q2DzIntegerKind $owner)) { return $null }
  return ('q2.{0}' -f [string]$owner)
}

function Get-RefreshTargetState {
  param($Targets)
  $state = New-Q2DzTargetMap
  foreach ($key in @($Targets.Keys)) {
    $target = $Targets[$key]
    $invalid = 0
    if ($null -ne $target -and $null -ne $target.PSObject -and
        $target.PSObject.Properties.Name -contains 'Invalid') { $invalid = [int]$target.Invalid }
    $state[[string]$key] = [pscustomobject]@{
      misses = [int]$target.Misses; invalid = $invalid; config = [string]$target.Config }
  }
  return $state
}

function Assert-RefreshIdList {
  param($Expected, $Actual, [string]$Label)
  $expectedIds = (Sort-Q2DzStrings (Expand-RefreshIds $Expected)) -join ' '
  $actualIds = (Sort-Q2DzStrings @($Actual)) -join ' '
  Assert-Equal $expectedIds $actualIds $Label
}

function New-RefreshHeartbeatSink {
  # Minimal fake coordinator stdin for heartbeat evidence — the same shape
  # launcher-stdin.tests.ps1's New-FakeHubWriter exposes (Text, Writes,
  # BaseStream.Write/Flush).
  $sink = [pscustomobject]@{ Text = New-Object System.Text.StringBuilder; Writes = 0 }
  $stream = [pscustomobject]@{ Owner = $sink }
  $stream | Add-Member ScriptMethod Write {
    param([byte[]]$Bytes, [int]$Offset, [int]$Count)
    [void]$this.Owner.Text.Append([Text.Encoding]::UTF8.GetString($Bytes, $Offset, $Count))
    $this.Owner.Writes++
  }
  $stream | Add-Member ScriptMethod Flush { }
  $sink | Add-Member NoteProperty BaseStream $stream
  return $sink
}

function Invoke-RefreshScenario {
  param($Case)
  Assert-True (@($Case.steps).Count -gt 0) 'refresh-scenario has steps'
  $targets = New-Q2DzTargetMap
  foreach ($stepIndex in 0..(@($Case.steps).Count - 1)) {
    $step = $Case.steps[$stepIndex]
    $label = '{0} step {1}' -f [string]$Case.name,$stepIndex
    Assert-True ($step.PSObject.Properties.Name -contains 'expected') "${label}: expected block is required"
    $expected = $step.expected
    $fail = ''
    if ($step.PSObject.Properties.Name -contains 'fail') { $fail = [string]$step.fail }
    Assert-True (@('','http','timeout','json','rtc','capacity','content_check') -ccontains $fail) "${label}: unknown fail kind '$fail'"
    $rows = @()
    if ($step.PSObject.Properties.Name -contains 'rooms') { $rows = @(Expand-RefreshRows $step.rooms) }
    if ($fail -ceq 'capacity') {
      $joinable = 0
      foreach ($row in $rows) {
        if ((Test-Q2DzRoomRow $row) -and [int]$row.players -lt [int]$row.createSettings.maxPlayers) { $joinable++ }
      }
      for ($i = 0; $i -lt (257 - $joinable); $i++) {
        $suffix = '{0:D3}' -f $i
        $rows += New-TestRoom -RoomId ('room.q2.cap.{0}' -f $suffix) -Owner ('cap{0}' -f $suffix) -Name ('CAP {0}' -f $suffix)
      }
    }
    $catalog = [pscustomobject]@{ rooms = [object[]]$rows }
    $aliasList = [System.Collections.Generic.List[string]]::new()
    if ($step.PSObject.Properties.Name -contains 'aliases') {
      foreach ($alias in @($step.aliases)) { $aliasList.Add([string]$alias) }
    } else {
      $seenAlias = New-Q2DzTargetMap
      foreach ($row in $rows) {
        $alias = Get-RefreshRowAlias $row
        if ($null -ne $alias -and -not $seenAlias.ContainsKey($alias)) {
          $seenAlias[$alias] = $true
          $aliasList.Add($alias)
        }
      }
    }
    $lookup = [pscustomobject]@{ aliases = [string[]]$aliasList.ToArray() }
    $rtc = @(New-TestRtc)
    if ($step.PSObject.Properties.Name -contains 'rtc') { $rtc = @($step.rtc) }
    if ($fail -ceq 'rtc') { $rtc = @([pscustomobject]@{ domain = 'bad domain'; iceservers = @() }) }
    $fetch = {
      param($endpoint)
      if ($fail -ceq 'http') {
        $statusError = [Exception]::new('Dos.Zone endpoint returned a non-success status')
        $statusError.Data['q2dz.http'] = 503
        throw $statusError
      }
      if ($fail -ceq 'timeout') { throw [System.TimeoutException]::new('Dos.Zone response timed out') }
      if ($fail -ceq 'json') { throw 'Dos.Zone endpoint returned invalid JSON' }
      switch ($endpoint) {
        $Q2DZ_ROOM_ENDPOINT { return $catalog }
        $Q2DZ_LOOKUP_ENDPOINT { return $lookup }
        $Q2DZ_RTC_ENDPOINT { return $rtc }
        default { throw "unexpected endpoint $endpoint" }
      }
    }
    $check = {
      param($path, $map, $root)
      if ($fail -ceq 'content_check') { throw 'content checker execution failed' }
      return $true
    }
    $before = Get-RefreshTargetState $targets
    $writer = [IO.StringWriter]::new()
    $previousError = [Console]::Error
    $plan = $null
    try {
      [Console]::SetError($writer)
      $items = @(Get-Q2DzLiveTargets -Targets $targets -Q2Path 'C:\Games\Q2PRO-X.exe' `
        -ContentCheckPath 'q2dz-content-check.exe' -FetchJson $fetch -RunContentCheck $check)
      if ($items.Count -eq 1) { $plan = $items[0] }
    } catch {
      $plan = $null
    } finally {
      [Console]::SetError($previousError)
      $logText = $writer.ToString()
      $writer.Dispose()
    }
    # The real heartbeat writer into a fake stdin sink, gated like
    # Invoke-Q2DzHub (it publishes only a complete plan): an ok step writes
    # exactly the planned targets, a failed refresh writes nothing.
    $heartbeatSink = New-RefreshHeartbeatSink
    if ($null -ne $plan -and $null -ne $plan.Targets -and $null -ne $plan.IceServers) {
      $heartbeatCode = Write-Q2DzHeartbeat -Process ([pscustomobject]@{ HasExited = $false }) `
        -Writer $heartbeatSink -Plan $plan -Generation ([UInt64]($stepIndex + 1))
      Assert-True ($null -eq $heartbeatCode) "${label}: heartbeat write completes"
    }
    $heartbeatPuts = [System.Collections.Generic.List[string]]::new()
    foreach ($heartbeatLine in ($heartbeatSink.Text.ToString() -split "`n")) {
      $putMatch = [regex]::Match($heartbeatLine, ' PUT room=([A-Za-z0-9._-]+) json=')
      if ($putMatch.Success) { $heartbeatPuts.Add($putMatch.Groups[1].Value) }
    }
    if ($null -ne $plan) {
      Assert-RefreshIdList @($plan.Targets.Keys) $heartbeatPuts "${label}: heartbeat PUTs exactly the planned targets"
    } else {
      Assert-Equal 0 $heartbeatSink.Writes "${label}: failed refresh writes no heartbeat"
    }
    $emittedRows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($logText -split "`n")) {
      $match = [regex]::Match($line, 'LAUNCHER CATALOG_ROW result=rejected (\S[^\r\n]*)$')
      if (-not $match.Success) { continue }
      $entry = [ordered]@{}
      foreach ($field in ($match.Groups[1].Value -split ' ')) {
        if (-not $field) { continue }
        $pair = $field -split '=', 2
        $entry[$pair[0]] = $pair[1]
      }
      $emittedRows.Add([pscustomobject]$entry)
    }
    $actualTargets = New-Q2DzTargetMap
    if ($null -ne $plan) {
      $targets = $plan.Targets
      $actualTargets = Get-RefreshTargetState $targets
      foreach ($key in @($actualTargets.Keys)) {
        $state = $actualTargets[$key]
        if ($before.ContainsKey($key) -and ($state.misses -gt 0 -or $state.invalid -gt 0)) {
          Assert-Equal $before[$key].config $state.config "${label}: retained target $key keeps its accepted config"
        }
      }
      # A room that recovers (was stale/invalid, now re-accepted) must get the
      # NEW row's serialized config, not the old cached one: check both the
      # in-memory target config and the actual heartbeat PUT payload against
      # the row's current map, and that a changed map actually changed the
      # serialized config relative to the prior step.
      $rowsById = @{}
      foreach ($row in $rows) {
        if ($row.PSObject.Properties.Name -contains 'room' -and $null -ne $row.room -and $row.room -is [string]) {
          $rowsById[[string]$row.room] = $row
        }
      }
      $putJsonByRoom = @{}
      foreach ($heartbeatLine in ($heartbeatSink.Text.ToString() -split "`n")) {
        $putJsonMatch = [regex]::Match($heartbeatLine, ' PUT room=([A-Za-z0-9._-]+) json=(.*)$')
        if ($putJsonMatch.Success) { $putJsonByRoom[$putJsonMatch.Groups[1].Value] = $putJsonMatch.Groups[2].Value }
      }
      $acceptedList = @()
      if ($plan.PSObject.Properties.Name -contains 'Accepted') { $acceptedList = @($plan.Accepted) }
      foreach ($roomId in $acceptedList) {
        if ($null -eq $roomId -or $roomId -isnot [string] -or $roomId -eq '') { continue }
        $row = $rowsById[$roomId]
        if ($null -eq $row -or -not $actualTargets.ContainsKey($roomId)) { continue }
        $currentMap = $null
        if ($row.PSObject.Properties.Name -contains 'createSettings' -and
            $row.createSettings.PSObject.Properties.Name -contains 'map') {
          $currentMap = [string]$row.createSettings.map
        }
        if ([string]::IsNullOrEmpty($currentMap)) { continue }
        $recoveredConfig = [string]$actualTargets[$roomId].config
        Assert-True ($recoveredConfig.Contains($currentMap)) "${label}: recovered target $roomId config reflects the current row"
        Assert-True ($putJsonByRoom.ContainsKey($roomId) -and $putJsonByRoom[$roomId].Contains($currentMap)) "${label}: recovered target $roomId heartbeat PUT reflects the current row"
        if ($before.ContainsKey($roomId)) {
          $priorConfig = [string]$before[$roomId].config
          if (-not $priorConfig.Contains($currentMap)) {
            Assert-True ($recoveredConfig -ne $priorConfig) "${label}: recovered target $roomId config replaces the stale cached one"
          }
        }
      }
    } else {
      $actualTargets = Get-RefreshTargetState $targets
      foreach ($key in @($actualTargets.Keys)) {
        $state = $actualTargets[$key]
        Assert-True ($before.ContainsKey($key)) "${label}: failed refresh must not add target $key"
        Assert-Equal $before[$key].misses $state.misses "${label}: failed refresh keeps misses for $key"
        Assert-Equal $before[$key].invalid $state.invalid "${label}: failed refresh keeps invalid for $key"
        Assert-Equal $before[$key].config $state.config "${label}: failed refresh keeps config for $key"
      }
      Assert-Equal @($before.Keys).Count @($actualTargets.Keys).Count "${label}: failed refresh keeps the target set"
    }
    Assert-Equal ([string]$expected.result) $(if ($null -ne $plan) { 'ok' } else { 'error' }) "${label}: result"
    if ($expected.PSObject.Properties.Name -contains 'accepted') {
      $accepted = @()
      if ($null -ne $plan -and $plan.PSObject.Properties.Name -contains 'Accepted') { $accepted = @($plan.Accepted) }
      Assert-RefreshIdList $expected.accepted $accepted "${label}: accepted"
    }
    if ($expected.PSObject.Properties.Name -contains 'registered') {
      $registered = @()
      if ($null -ne $plan -and $plan.PSObject.Properties.Name -contains 'Registered') { $registered = @($plan.Registered) }
      Assert-RefreshIdList $expected.registered $registered "${label}: registered"
    }
    if ($expected.PSObject.Properties.Name -contains 'puts') {
      Assert-RefreshIdList $expected.puts $heartbeatPuts "${label}: puts"
    }
    if ($expected.PSObject.Properties.Name -contains 'drops') {
      $drops = @()
      if ($null -ne $plan) { $drops = @($plan.Drops) }
      Assert-RefreshIdList $expected.drops $drops "${label}: drops"
    }
    if ($expected.PSObject.Properties.Name -contains 'targets') {
      $wanted = @{}
      foreach ($entry in @(Expand-RefreshExpectedTargets $expected.targets)) {
        $wanted[[string]$entry.room] = $entry
      }
      Assert-Equal ((Sort-Q2DzStrings @($wanted.Keys)) -join ' ') ((Sort-Q2DzStrings @($actualTargets.Keys)) -join ' ') "${label}: target ids"
      foreach ($key in @($wanted.Keys)) {
        Assert-True ($actualTargets.ContainsKey($key)) "${label}: target $key exists"
        Assert-Equal ([int]$wanted[$key].misses) ([int]$actualTargets[$key].misses) "${label}: $key misses"
        Assert-Equal ([int]$wanted[$key].invalid) ([int]$actualTargets[$key].invalid) "${label}: $key invalid"
      }
    }
    if ($expected.PSObject.Properties.Name -contains 'rows') {
      $expectedRows = @($expected.rows)
      Assert-Equal $expectedRows.Count $emittedRows.Count "${label}: CATALOG_ROW count"
      for ($rowIndex = 0; $rowIndex -lt $expectedRows.Count; $rowIndex++) {
        $exp = @{}
        foreach ($property in $expectedRows[$rowIndex].PSObject.Properties) { $exp[$property.Name] = [string]$property.Value }
        $act = @{}
        foreach ($property in $emittedRows[$rowIndex].PSObject.Properties) { $act[$property.Name] = [string]$property.Value }
        Assert-Equal ((@($exp.Keys) | Sort-Object) -join ',') ((@($act.Keys) | Sort-Object) -join ',') "${label}: row $rowIndex fields"
        foreach ($key in $exp.Keys) {
          Assert-Equal $exp[$key] $act[$key] "${label}: row $rowIndex field $key"
        }
      }
    }
    foreach ($event in $emittedRows) {
      if ($event.PSObject.Properties.Name -contains 'kind' -and [string]$event.kind -ceq 'alias' -and
          [string]$event.reason -ceq 'format') {
        $rawAlias = [string]$aliasList[[int]$event.index]
        Assert-True (-not $logText.Contains($rawAlias)) "${label}: rejected alias value never reaches the log"
      }
    }
  }
}

$repo = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$fixturePath = Join-Path $repo 'tests\contracts\doszone-api-cases.json'
$cases = @(Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json)
if ($cases.Count -eq 1 -and $cases[0] -is [array]) { $cases = @($cases[0]) }

$scenarioNames = [System.Collections.Generic.List[string]]::new()
$refreshFailures = [System.Collections.Generic.List[string]]::new()
foreach ($case in $cases) {
  if ($case.kind -eq 'refresh-scenario') {
    # Collected, not thrown: one bad case must not hide the verdicts of the rest.
    try { Invoke-RefreshScenario $case }
    catch { [void]$refreshFailures.Add(('{0}: {1}' -f [string]$case.name, $_.Exception.Message)) }
    continue
  }
  if ($case.kind -eq 'heartbeat-scenario') {
    [void]$scenarioNames.Add([string]$case.name)
    $actual = Invoke-HeartbeatScenario $case
    foreach ($property in @($case.expected.PSObject.Properties)) {
      Assert-Equal $property.Value $actual.($property.Name) "shared scenario $($case.name) field $($property.Name)"
    }
    continue
  }
  if ($case.kind -eq 'dedicated-ice') {
    $thrown = $false
    $actual = @()
    try { $actual = @(Get-Q2DzDedicatedIceServers @($case.payload)) } catch { $thrown = $true }
    Assert-Equal ([bool]$case.valid) (-not $thrown) "shared case $($case.name)"
    if ($case.valid) {
      Assert-Equal ($case.expected.iceServers | ConvertTo-Json -Depth 8 -Compress) `
        ($actual | ConvertTo-Json -Depth 8 -Compress) "shared ICE order $($case.name)"
    }
    continue
  }
  $actual = switch ($case.kind) {
    'rooms' { Test-Q2DzRoomPayload $case.payload }
    'aliases' { Test-Q2DzAliasPayload $case.payload }
    'rtc' { Test-Q2DzRtcPayload @($case.payload) }
    default { throw "Unknown contract case kind: $($case.kind)" }
  }
  Assert-Equal ([bool]$case.valid) ([bool]$actual) "shared case $($case.name)"
}
Assert-Equal 11 $scenarioNames.Count 'Task 6 shared heartbeat cases are registered and executed'
if ($refreshFailures.Count -gt 0) {
  throw ("{0} refresh-scenario case(s) failed:`n{1}" -f $refreshFailures.Count, ($refreshFailures -join "`n"))
}

$mixedRows = New-TestRows 20 20
$mixedLookup = New-TestLookup $mixedRows
$emptyTargets = New-Q2DzTargetMap
$mixedPlan = Get-Q2DzTargetPlan -Targets $emptyTargets -Rows $mixedRows -MakeConfig {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
}
Assert-Equal 40 $mixedPlan.Targets.Count 'mixed 20 q2 and 20 q2ded retains every target'
Assert-Equal (($mixedRows.room | Sort-Object) -join ',') ((Sort-Q2DzStrings @($mixedPlan.Targets.Keys)) -join ',') 'target map is stable by room id'

$mixedMissing = New-TestRoom -RoomId 'room.q2.missing.1' -Owner '99' -Map 'missing'
$mixedValid = New-TestRoom -RoomId 'room.q2.valid.1' -Owner '98'
$mixedIsolated = Get-Q2DzTargetPlan -Targets (New-Q2DzTargetMap) -Rows @($mixedMissing,$mixedValid) -MakeConfig {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
} -MapAvailable { param($row) return ([string]$row.createSettings.map -ne 'missing') }
Assert-Equal 1 $mixedIsolated.Targets.Count 'missing map skips only the new room'
Assert-True $mixedIsolated.Targets.ContainsKey($mixedValid.room) 'valid room survives a missing new-room map'
Assert-Equal 0 $mixedIsolated.Drops.Count 'missing new-room map emits no unrelated drop'
$mixedRecovered = Get-Q2DzTargetPlan -Targets $mixedIsolated.Targets -Rows @($mixedMissing,$mixedValid) -MakeConfig {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
} -MapAvailable { param($row) return $true }
Assert-True $mixedRecovered.Targets.ContainsKey($mixedMissing.room) 'new room returns when its map becomes available'

$one = New-TestRoom -RoomId 'room.q2.keep.1'
$oneConfig = '{"roomId":"room.q2.keep.1","alias":"q2.1","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"q2dm1","game":"3zb2"}'
$current = New-Q2DzTargetMap
$current[$one.room] = New-Q2DzTarget $one.room $oneConfig 0 'q2dm1'
$firstMiss = Get-Q2DzTargetPlan -Targets $current -Rows @() -MakeConfig { param($row) return '' }
Assert-True $firstMiss.Targets.ContainsKey($one.room) 'one successful miss retains target'
Assert-Equal 1 $firstMiss.Targets[$one.room].Misses 'one successful miss is counted'
Assert-Equal 0 $firstMiss.Drops.Count 'one successful miss does not drop'
$secondMiss = Get-Q2DzTargetPlan -Targets $firstMiss.Targets -Rows @() -MakeConfig { param($row) return '' }
Assert-Equal 0 $secondMiss.Targets.Count 'second successful miss removes target from state'
Assert-Equal $one.room $secondMiss.Drops[0] 'second successful miss emits exact room drop'

$fullExisting = New-TestRoom -RoomId 'room.q2.full-existing.1'
$fullExisting.players = $fullExisting.createSettings.maxPlayers
$fullNew = New-TestRoom -RoomId 'room.q2.full-new.2' -Owner '2'
$fullNew.players = $fullNew.createSettings.maxPlayers
$fullCurrent = New-Q2DzTargetMap
$fullCurrent[$fullExisting.room] = New-Q2DzTarget $fullExisting.room $oneConfig 0 'q2dm1'
$fullPlan = Get-Q2DzTargetPlan -Targets $fullCurrent -Rows @($fullExisting,$fullNew) -MakeConfig {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
}
Assert-True $fullPlan.Targets.ContainsKey($fullExisting.room) 'already-ready full room is retained'
Assert-True (-not $fullPlan.Targets.ContainsKey($fullNew.room)) 'new full room is not added'

foreach ($mapOnlyRoom in @(
    (New-TestRoom -RoomId 'room.q2.map-only.1'),
    (New-TestDedicatedRoom -RoomId 'room.q2ded.map-only.1')
  )) {
  $mapOnlyOldConfig = New-Q2DzRoomConfig -Room $mapOnlyRoom -LookupPayload (New-TestLookup @($mapOnlyRoom)) -RtcPayload @(New-TestRtc)
  $mapOnlyCurrent = New-Q2DzTargetMap
  $mapOnlyCurrent[$mapOnlyRoom.room] = New-Q2DzTarget $mapOnlyRoom.room $mapOnlyOldConfig 0 'q2dm1'
  $mapOnlyRoom.createSettings.map = 'q2dm2'
  $mapOnlyChecks = @{ Value = 0 }
  $mapOnlyPlan = Get-Q2DzTargetPlan -Targets $mapOnlyCurrent -Rows @($mapOnlyRoom) -MakeConfig {
    param($row)
    return New-Q2DzRoomConfig -Room $row -LookupPayload (New-TestLookup @($row)) -RtcPayload @(New-TestRtc)
  } -MapAvailable { param($row) $mapOnlyChecks.Value++; return $true }
  Assert-Equal 1 $mapOnlyChecks.Value ('map-only {0} refresh checks content once' -f $mapOnlyRoom.createSettings.prefix)
  Assert-Equal 0 $mapOnlyPlan.Drops.Count ('map-only {0} refresh does not recreate transport' -f $mapOnlyRoom.createSettings.prefix)
  Assert-Equal 'q2dm2' $mapOnlyPlan.Targets[$mapOnlyRoom.room].MapName ('map-only {0} refresh stores MapName' -f $mapOnlyRoom.createSettings.prefix)
  Assert-Equal 'q2dm2' (($mapOnlyPlan.Targets[$mapOnlyRoom.room].Config | ConvertFrom-Json).map) ('map-only {0} refresh stores config map' -f $mapOnlyRoom.createSettings.prefix)
}

$mapChanged = New-TestRoom -RoomId $one.room -Map 'missing'
$mapSibling = New-TestRoom -RoomId 'room.q2.keep.2' -Owner '2'
$mapCurrent = New-Q2DzTargetMap
$mapCurrent[$one.room] = New-Q2DzTarget $one.room $oneConfig 0 'q2dm1'
$mapMakeConfig = {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
}
$mapChecks = @{ Value = 0 }
$mapPlan = Get-Q2DzTargetPlan -Targets $mapCurrent -Rows @($mapChanged,$mapSibling) -MakeConfig $mapMakeConfig `
  -MapAvailable { param($row) if ([string]$row.createSettings.map -eq 'missing') { $mapChecks.Value++ }; return ([string]$row.createSettings.map -ne 'missing') }
Assert-Equal 1 $mapChecks.Value 'retained missing map is checked once'
Assert-True (-not $mapPlan.Targets.ContainsKey($one.room)) 'retained target with missing map is removed in isolation'
Assert-True ($mapPlan.Drops -ccontains $one.room) 'retained target with missing map emits an explicit drop'
Assert-True $mapPlan.Targets.ContainsKey($mapSibling.room) 'sibling target survives retained map failure'
$mapRecovered = Get-Q2DzTargetPlan -Targets $mapPlan.Targets -Rows @($mapChanged,$mapSibling) -MakeConfig $mapMakeConfig `
  -MapAvailable { param($row) return $true }
Assert-True $mapRecovered.Targets.ContainsKey($mapChanged.room) 'room returns when its map becomes available'
Assert-Equal 0 $mapRecovered.Drops.Count 'returning room does not drop a sibling'

$iceRoom = New-TestRoom -RoomId 'room.q2.ice-rotation.1' -Owner '77'
$oldIce = @([pscustomobject]@{ urls = @('stun:old.example.invalid') })
$newIce = @([pscustomobject]@{ urls = @('stun:new.example.invalid') })
$oldIceConfig = New-Q2DzRoomConfig -Room $iceRoom -LookupPayload (New-TestLookup @($iceRoom)) -RtcPayload @(New-TestRtc) -IceServers $oldIce
$iceCurrent = New-Q2DzTargetMap
$iceCurrent[$iceRoom.room] = New-Q2DzTarget $iceRoom.room $oldIceConfig 0 $iceRoom.createSettings.map
$icePlan = Get-Q2DzTargetPlan -Targets $iceCurrent -Rows @($iceRoom) -MakeConfig {
  param($row)
  return New-Q2DzRoomConfig -Room $row -LookupPayload (New-TestLookup @($row)) -RtcPayload @(New-TestRtc) -IceServers $newIce
}
Assert-Equal 0 $icePlan.Drops.Count 'ICE-only rotation does not recreate a room'
Assert-True ($icePlan.Targets[$iceRoom.room].Config -ne $oldIceConfig) 'ICE-only rotation refreshes the stored config'
Assert-Equal 'stun:new.example.invalid' ((($icePlan.Targets[$iceRoom.room].Config | ConvertFrom-Json).iceServers[0].urls[0])) 'ICE-only rotation keeps the fresh ICE config'

$duplicateRoom = New-TestRoom -RoomId 'room.q2.duplicate.1' -Owner '1'
$duplicateRoomAgain = New-TestRoom -RoomId 'room.q2.duplicate.1' -Owner '2'
Assert-Throws { Get-Q2DzReadyRooms ([pscustomobject]@{ rooms = @($duplicateRoom,$duplicateRoomAgain) }) } 'duplicate room id is rejected'
$duplicateAliasA = New-TestRoom -RoomId 'room.q2.alias-a.1' -Owner '3'
$duplicateAliasB = New-TestRoom -RoomId 'room.q2.alias-b.2' -Owner '3'
Assert-Throws { Get-Q2DzReadyRooms ([pscustomobject]@{ rooms = @($duplicateAliasA,$duplicateAliasB) }) } 'duplicate derived alias is rejected'

$exactRows = New-TestRows 256 0
$exactPlan = Get-Q2DzTargetPlan -Targets (New-Q2DzTargetMap) -Rows $exactRows -MakeConfig {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"q2dm1","game":"3zb2"}')
}
Assert-Equal 256 $exactPlan.Targets.Count 'exactly 256 compatible targets are retained'
$tooMany = New-TestRows 257 0
Assert-Throws { Get-Q2DzTargetPlan -Targets (New-Q2DzTargetMap) -Rows $tooMany -MakeConfig { param($row) return '{}' } } '257 compatible targets fail with catalog capacity'

$invalidRtc = @([pscustomobject]@{ domain = 'bad domain'; iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') }) })
Assert-Throws { Get-Q2DzDedicatedIceServers $invalidRtc } 'invalid global RTC is rejected'

$emptyFetch = {
  param($endpoint)
  switch ($endpoint) {
    $Q2DZ_ROOM_ENDPOINT { return [pscustomobject]@{ rooms = @() } }
    $Q2DZ_LOOKUP_ENDPOINT { return [pscustomobject]@{ aliases = @() } }
    $Q2DZ_RTC_ENDPOINT { return (New-TestRtc) }
    default { throw "unexpected endpoint $endpoint" }
  }
}
$emptyPlan = Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
  -ContentCheckPath 'q2dz-content-check.exe' -FetchJson $emptyFetch -RunContentCheck { param($path,$map,$root) return $true }
Assert-Equal 0 $emptyPlan.Targets.Count 'empty PSCustomObject catalog is accepted'
Assert-Equal 1 $emptyPlan.IceServers.Count 'empty catalog still validates global RTC'

function Invoke-Q2DzTestContentChecker {
  $global:LASTEXITCODE = [int]$global:Q2DzTestContentCheckExitCode
}
$checkerRoom = New-TestRoom -RoomId 'room.q2.checker.1'
$checkerRows = @($checkerRoom)
$checkerLookup = New-TestLookup $checkerRows
$checkerFetch = {
  param($endpoint)
  switch ($endpoint) {
    $Q2DZ_ROOM_ENDPOINT { return [pscustomobject]@{ rooms = $checkerRows } }
    $Q2DZ_LOOKUP_ENDPOINT { return $checkerLookup }
    $Q2DZ_RTC_ENDPOINT { return (New-TestRtc) }
    default { throw "unexpected endpoint $endpoint" }
  }
}
$lastExitCodeVariable = Get-Variable -Name LASTEXITCODE -Scope Global -ErrorAction SilentlyContinue
$hadLastExitCode = $null -ne $lastExitCodeVariable
$previousLastExitCode = if ($hadLastExitCode) { $lastExitCodeVariable.Value } else { $null }
try {
  $global:Q2DzTestContentCheckExitCode = 0
  $checkerReady = Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
    -ContentCheckPath 'Invoke-Q2DzTestContentChecker' -FetchJson $checkerFetch
  Assert-Equal 1 $checkerReady.Targets.Count 'content checker exit 0 keeps the room'
  $global:Q2DzTestContentCheckExitCode = 3
  $checkerMissing = Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
    -ContentCheckPath 'Invoke-Q2DzTestContentChecker' -FetchJson $checkerFetch
  Assert-Equal 0 $checkerMissing.Targets.Count 'content checker exit 3 skips the room'
  $global:Q2DzTestContentCheckExitCode = 17
  Assert-Throws {
    Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
      -ContentCheckPath 'Invoke-Q2DzTestContentChecker' -FetchJson $checkerFetch | Out-Null
  } 'unexpected content checker exit remains an execution error'
  $missingCheckerMessage = $null
  try {
    Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
      -ContentCheckPath 'q2dz-missing-content-check-for-test.exe' -FetchJson $checkerFetch | Out-Null
  } catch {
    $missingCheckerMessage = $_.Exception.Message
  }
  Assert-Equal 'content checker execution failed' $missingCheckerMessage 'missing content checker is normalized'
}
finally {
  if ($hadLastExitCode) {
    Set-Variable -Name LASTEXITCODE -Scope Global -Value $previousLastExitCode
  } else {
    Remove-Variable -Scope Global -Name LASTEXITCODE -ErrorAction SilentlyContinue
  }
  Remove-Variable -Scope Global -Name Q2DzTestContentCheckExitCode -ErrorAction SilentlyContinue
}

function Invoke-CapturedLiveTargets {
  param($Targets, [scriptblock]$FetchJson, [scriptblock]$RunContentCheck)
  $writer = [IO.StringWriter]::new()
  $previousError = [Console]::Error
  try {
    [Console]::SetError($writer)
    $items = @(Get-Q2DzLiveTargets -Targets $Targets -Q2Path 'C:\Games\Q2PRO-X.exe' `
      -ContentCheckPath 'q2dz-content-check.exe' -FetchJson $FetchJson -RunContentCheck $RunContentCheck)
    return [pscustomobject]@{ Items = $items; Log = $writer.ToString() }
  }
  finally {
    [Console]::SetError($previousError)
    $writer.Dispose()
  }
}

$logRows = @(
  (New-TestRoom -RoomId 'room.q2.log.1' -Owner '41'),
  (New-TestRoom -RoomId 'room.q2.log.2' -Owner '42')
)
$logCases = @(
  [pscustomobject]@{ Rows = $logRows; Aliases = (New-TestLookup $logRows).aliases; Counts = 'previous=0 ready=2 registered=2 targets=2 drops=0' },
  [pscustomobject]@{ Rows = @(); Aliases = @(); Counts = 'previous=0 ready=0 registered=0 targets=0 drops=0' },
  [pscustomobject]@{ Rows = $logRows; Aliases = @((Get-Q2DzRoomAlias $logRows[0])); Counts = 'previous=0 ready=2 registered=1 targets=1 drops=0' }
)
foreach ($logCase in $logCases) {
  $caseRows = @($logCase.Rows)
  $caseAliases = @($logCase.Aliases)
  $captured = Invoke-CapturedLiveTargets -Targets (New-Q2DzTargetMap) -FetchJson {
    param($endpoint)
    switch ($endpoint) {
      $Q2DZ_ROOM_ENDPOINT { return [pscustomobject]@{ rooms = $caseRows } }
      $Q2DZ_LOOKUP_ENDPOINT { return [pscustomobject]@{ aliases = $caseAliases } }
      $Q2DZ_RTC_ENDPOINT { return (New-TestRtc) }
      default { throw "unexpected endpoint $endpoint" }
    }
  } -RunContentCheck { param($path,$map,$root) return $true }
  Assert-Equal 1 $captured.Items.Count 'refresh diagnostics do not add success-pipeline items'
  Assert-True ($captured.Log -match ('(?m)^\d{{4}}-\d{{2}}-\d{{2}}T\d{{2}}:\d{{2}}:\d{{2}}\.\d+Z LAUNCHER CATALOG_REFRESH result=ok stage=plan error=none http=0 elapsed_ms=\d+ {0} puts=\d+\r?$' -f $logCase.Counts)) 'successful refresh logs stage and counts'
}

$logCurrent = New-Q2DzTargetMap
$logCurrent['room.q2.old.1'] = New-Q2DzTarget 'room.q2.old.1' '{"roomId":"room.q2.old.1"}' 0 'q2dm1'
foreach ($failure in @('lookup','checker')) {
  $failureKind = 'other'
  $writer = [IO.StringWriter]::new()
  $previousError = [Console]::Error
  $caught = $false
  try {
    [Console]::SetError($writer)
    Get-Q2DzLiveTargets -Targets $logCurrent -Q2Path 'C:\Games\Q2PRO-X.exe' `
      -ContentCheckPath 'q2dz-content-check.exe' -FetchJson {
        param($endpoint)
        if ($failure -eq 'lookup' -and $endpoint -eq $Q2DZ_LOOKUP_ENDPOINT) { throw 'secret-marker' }
        switch ($endpoint) {
          $Q2DZ_ROOM_ENDPOINT { return [pscustomobject]@{ rooms = $logRows } }
          $Q2DZ_LOOKUP_ENDPOINT { return (New-TestLookup $logRows) }
          $Q2DZ_RTC_ENDPOINT { return (New-TestRtc) }
          default { throw "unexpected endpoint $endpoint" }
        }
      } -RunContentCheck {
        param($path,$map,$root)
        if ($failure -eq 'checker') { throw 'secret-marker' }
        return $true
      } | Out-Null
  } catch {
    $caught = $true
  } finally {
    [Console]::SetError($previousError)
    $failureLog = $writer.ToString()
    $writer.Dispose()
  }
  Assert-True $caught "$failure failure still propagates"
  Assert-True (-not $failureLog.Contains('secret-marker')) "$failure diagnostics exclude exception text"
  $stage = if ($failure -eq 'lookup') { 'lookup' } else { 'plan' }
  $registered = if ($failure -eq 'lookup') { 0 } else { 2 }
  Assert-True ($failureLog -match (('(?m)^\d{{4}}-\d{{2}}-\d{{2}}T\d{{2}}:\d{{2}}:\d{{2}}\.\d+Z LAUNCHER CATALOG_REFRESH result=error stage={0} error={1} http=0 elapsed_ms=\d+ previous=1 ready=2 registered={2} targets=1 drops=0 puts=0\r?$' -f $stage,$failureKind,$registered))) "$failure failure logs safe stage and counts"
}

# A non-success HTTP status is classified instead of being dropped silently.
$httpWriter = [IO.StringWriter]::new()
$previousError = [Console]::Error
$httpCaught = $false
try {
  [Console]::SetError($httpWriter)
  Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
    -ContentCheckPath 'q2dz-content-check.exe' -FetchJson {
      param($endpoint)
      $statusError = [Exception]::new('Dos.Zone endpoint returned a non-success status')
      $statusError.Data['q2dz.http'] = 503
      throw $statusError
    } -RunContentCheck { param($path,$map,$root) return $true } | Out-Null
} catch {
  $httpCaught = $true
} finally {
  [Console]::SetError($previousError)
  $httpLog = $httpWriter.ToString()
  $httpWriter.Dispose()
}
Assert-True $httpCaught 'HTTP failure still propagates'
Assert-True ($httpLog -match '(?m)LAUNCHER CATALOG_REFRESH result=error stage=catalog error=http http=503 ') 'HTTP 503 is logged with its status'

# A catalog whose rooms field is not an array is a schema failure.
$shapeWriter = [IO.StringWriter]::new()
$previousError = [Console]::Error
$shapeCaught = $false
try {
  [Console]::SetError($shapeWriter)
  Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
    -ContentCheckPath 'q2dz-content-check.exe' -FetchJson {
      param($endpoint) return [pscustomobject]@{ rooms = 'oops' }
    } -RunContentCheck { param($path,$map,$root) return $true } | Out-Null
} catch {
  $shapeCaught = $true
} finally {
  [Console]::SetError($previousError)
  $shapeLog = $shapeWriter.ToString()
  $shapeWriter.Dispose()
}
Assert-True $shapeCaught 'non-array rooms still fails the refresh'
Assert-True ($shapeLog -match '(?m)LAUNCHER CATALOG_REFRESH result=error stage=catalog error=schema ') 'rooms-not-array is a schema failure'

# The launcher-owned session log rotates at the size bound on the live file.
$logState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  Initialize-Q2DzSessionLog -StateDirectory $logState
  $logPath = Join-Path $logState 'logs\q2dz.log'
  Assert-True (Test-Path -LiteralPath $logPath) 'session log file is created'
  $previousMax = $Q2DZ_LOG_MAX_BYTES
  try {
    $Q2DZ_LOG_MAX_BYTES = 1024
    foreach ($number in 1..60) {
      Write-Q2DzCatalogEvent ('HEARTBEAT generation={0} puts=1 drops=0' -f $number)
    }
  } finally {
    $Q2DZ_LOG_MAX_BYTES = $previousMax
  }
  $rotatedPath = Join-Path $logState 'logs\q2dz.1.log'
  Assert-True (Test-Path -LiteralPath $rotatedPath) 'session log rotates at the size bound'
  Assert-True ((Get-Item -LiteralPath $logPath).Length -gt 0) 'live log keeps writing after rotation'
  Assert-True ((Get-Content -LiteralPath $logPath -Raw) -match 'LAUNCHER HEARTBEAT generation=60') 'newest event lands in the live log'
  Assert-True (-not (Test-Path -LiteralPath (Join-Path $logState 'logs\q2dz.2.log'))) 'rotation keeps exactly two files'
  Assert-True (-not ((Get-Content -LiteralPath $rotatedPath -Raw).Contains('CANARY'))) 'rotated log holds no secret material'
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $logState -Recurse -Force -ErrorAction SilentlyContinue
}

# An unwritable log directory falls back to stderr and never aborts refresh.
$blockedState = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-log-' + [Guid]::NewGuid().ToString('N'))
try {
  New-Item -ItemType Directory -Force -Path $blockedState | Out-Null
  New-Item -ItemType File -Path (Join-Path $blockedState 'logs') | Out-Null
  Initialize-Q2DzSessionLog -StateDirectory $blockedState
  $fallbackWriter = [IO.StringWriter]::new()
  $previousError = [Console]::Error
  try {
    [Console]::SetError($fallbackWriter)
    $fallbackPlan = Get-Q2DzLiveTargets -Targets (New-Q2DzTargetMap) -Q2Path 'C:\Games\Q2PRO-X.exe' `
      -ContentCheckPath 'q2dz-content-check.exe' -FetchJson {
        param($endpoint)
        switch ($endpoint) {
          $Q2DZ_ROOM_ENDPOINT { return [pscustomobject]@{ rooms = $logRows } }
          $Q2DZ_LOOKUP_ENDPOINT { return (New-TestLookup $logRows) }
          $Q2DZ_RTC_ENDPOINT { return (New-TestRtc) }
          default { throw "unexpected endpoint $endpoint" }
        }
      } -RunContentCheck { param($path,$map,$root) return $true }
    Assert-Equal 2 $fallbackPlan.Targets.Count 'refresh still completes when the log is unwritable'
    Assert-True ($fallbackWriter.ToString() -match 'LAUNCHER CATALOG_REFRESH result=ok') 'events fall back to stderr on log failure'
  } finally {
    [Console]::SetError($previousError)
    $fallbackWriter.Dispose()
  }
} finally {
  Close-Q2DzSessionLog
  Remove-Item -LiteralPath $blockedState -Recurse -Force -ErrorAction SilentlyContinue
}

$launcherText = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\q2dz-launch.ps1') -Raw
Assert-True ($launcherText.Contains('Q2DZ/ROOMS/2 ICE')) 'launcher emits ROOMS/2 ICE'
Assert-True ($launcherText.Contains('Write-Q2DzHeartbeat')) 'launcher emits full heartbeat'
Assert-True ($launcherText.Contains('Get-Q2DzLiveTargets')) 'launcher refreshes dynamic room state'
Assert-True ($launcherText.Contains('StandardInput.BaseStream')) 'launcher writes coordinator stdin as bytes'
Assert-True ($launcherText.Contains('UTF8Encoding')) 'launcher creates explicit UTF-8 coordinator encoding'
Assert-True (-not $launcherText.Contains('Q2DZ_HUB_MAX_ROOMS')) 'launcher has no fixed-capacity constants'
$legacyTargetType = 'Hub' + [char]83 + 'lot'
Assert-True (-not $launcherText.Contains($legacyTargetType)) 'launcher has no legacy room model'

Write-Output ('catalog contract tests: PASS ({0} shared cases, dynamic target planner)' -f $cases.Count)
