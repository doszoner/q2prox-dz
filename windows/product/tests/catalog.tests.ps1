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

  if ($name -eq 'heartbeat-map-content-failure-preserves-target') {
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
      } -MapAvailable { param($row) return $false } | Out-Null
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

$repo = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$fixturePath = Join-Path $repo 'tests\contracts\doszone-api-cases.json'
$cases = @(Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json)
if ($cases.Count -eq 1 -and $cases[0] -is [array]) { $cases = @($cases[0]) }

$scenarioNames = [System.Collections.Generic.List[string]]::new()
foreach ($case in $cases) {
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

$mixedRows = New-TestRows 20 20
$mixedLookup = New-TestLookup $mixedRows
$emptyTargets = New-Q2DzTargetMap
$mixedPlan = Get-Q2DzTargetPlan -Targets $emptyTargets -Rows $mixedRows -MakeConfig {
  param($row)
  return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
}
Assert-Equal 40 $mixedPlan.Targets.Count 'mixed 20 q2 and 20 q2ded retains every target'
Assert-Equal (($mixedRows.room | Sort-Object) -join ',') ((Sort-Q2DzStrings @($mixedPlan.Targets.Keys)) -join ',') 'target map is stable by room id'

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

$mapChanged = New-TestRoom -RoomId $one.room -Map 'missing'
$mapCurrent = New-Q2DzTargetMap
$mapCurrent[$one.room] = New-Q2DzTarget $one.room $oneConfig 0 'q2dm1'
$mapFailed = $false
try {
  Get-Q2DzTargetPlan -Targets $mapCurrent -Rows @($mapChanged) -MakeConfig {
    param($row)
    return ('{"roomId":"' + $row.room + '","alias":"' + (Get-Q2DzRoomAlias $row) + '","ordered":false,"iceServers":[{"urls":["stun:example.invalid"]}],"map":"' + $row.createSettings.map + '","game":"3zb2"}')
  } -MapAvailable { param($row) return $false } | Out-Null
} catch { $mapFailed = $_.Exception.Message -match 'content' }
Assert-True $mapFailed 'map content failure preserves prior state'
Assert-Equal $oneConfig $mapCurrent[$one.room].Config 'map content failure does not mutate old target'

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
