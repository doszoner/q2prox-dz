$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$catalogPath = Join-Path $PSScriptRoot '..\catalog.ps1'
. $catalogPath

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
  try { & $Action } catch { $thrown = $true }
  if (-not $thrown) { throw "ASSERT THROWS failed: $Message" }
}

function Compress-TestBytes {
  param([byte[]]$Bytes)
  $output = New-Object System.IO.MemoryStream
  $gzip = New-Object -TypeName System.IO.Compression.GZipStream -ArgumentList @(
    $output,
    [System.IO.Compression.CompressionMode]::Compress,
    $true
  )
  try { $gzip.Write($Bytes, 0, $Bytes.Length) } finally { $gzip.Dispose() }
  $result = $output.ToArray()
  $output.Dispose()
  return $result
}

function Start-TestHttpListener {
  param(
    [byte[]]$Body,
    [int]$StatusCode = 200,
    [string]$Location = '',
    [bool]$Gzip = $false,
    [int]$DelayMilliseconds = 0
  )
  $port = Get-Random -Minimum 30000 -Maximum 50000
  $prefix = "http://127.0.0.1:$port/"
  $ready = Join-Path ([System.IO.Path]::GetTempPath()) ("q2dz-catalog-ready-{0}.txt" -f ([guid]::NewGuid()))
  $bodyBase64 = [Convert]::ToBase64String($Body)
  $job = Start-Job -ArgumentList $prefix,$ready,$bodyBase64,$StatusCode,$Location,$Gzip,$DelayMilliseconds -ScriptBlock {
    param($Prefix, $ReadyPath, $BodyBase64, $Status, $Redirect, $UseGzip, $Delay)
    $listener = New-Object System.Net.HttpListener
    $listener.Prefixes.Add($Prefix)
    $listener.Start()
    Set-Content -LiteralPath $ReadyPath -Value 'ready' -Encoding ASCII
    try {
      $context = $listener.GetContext()
      $bytes = [Convert]::FromBase64String($BodyBase64)
      $context.Response.StatusCode = $Status
      if ($Redirect) { $context.Response.RedirectLocation = $Redirect }
      if ($UseGzip) { $context.Response.Headers.Add('Content-Encoding', 'gzip') }
      $context.Response.ContentLength64 = $bytes.Length
      $context.Response.OutputStream.Flush()
      if ($Delay -gt 0) { Start-Sleep -Milliseconds $Delay }
      $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
      $context.Response.OutputStream.Close()
      $context.Response.Close()
    } finally {
      $listener.Stop()
      $listener.Close()
      Remove-Item -LiteralPath $ReadyPath -Force -ErrorAction SilentlyContinue
    }
  }
  for ($i = 0; $i -lt 100 -and -not (Test-Path -LiteralPath $ready); $i++) {
    Start-Sleep -Milliseconds 50
  }
  if (-not (Test-Path -LiteralPath $ready)) {
    Stop-Job $job -ErrorAction SilentlyContinue
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    throw 'localhost HttpListener did not start'
  }
  return @{ Job = $job; Uri = $prefix; Ready = $ready }
}

function Stop-TestHttpListener {
  param($Server)
  Stop-Job $Server.Job -ErrorAction SilentlyContinue
  Remove-Job $Server.Job -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $Server.Ready -Force -ErrorAction SilentlyContinue
}

$repo = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$fixturePath = Join-Path $repo 'tests\contracts\doszone-api-cases.json'
$cases = @(Get-Content -LiteralPath $fixturePath -Raw | ConvertFrom-Json)
if ($cases.Count -eq 1 -and $cases[0] -is [array]) { $cases = @($cases[0]) }

foreach ($case in $cases) {
  if ($case.kind -eq 'dedicated-ice') {
    $thrown = $false
    $actualIce = @()
    try { $actualIce = @(Get-Q2DzDedicatedIceServers @($case.payload)) } catch { $thrown = $true }
    Assert-Equal ([bool]$case.valid) (-not $thrown) "shared case $($case.name)"
    if ($case.valid) {
      Assert-Equal ($case.expected.iceServers | ConvertTo-Json -Depth 8 -Compress) `
        ($actualIce | ConvertTo-Json -Depth 8 -Compress) "shared ICE order $($case.name)"
    }
    continue
  }
  switch ($case.kind) {
    'rooms' { $actual = Test-Q2DzRoomPayload $case.payload }
    'aliases' { $actual = Test-Q2DzAliasPayload $case.payload }
    'rtc' { $actual = Test-Q2DzRtcPayload @($case.payload) }
    default { throw "Unknown contract case kind: $($case.kind)" }
  }
  Assert-Equal ([bool]$case.valid) ([bool]$actual) "shared case $($case.name)"
  if ($case.kind -eq 'rooms') {
    $selectionCount = 0
    $selectedFound = $false
    $selectionThrew = $false
    $expectedRoomId = if ($case.valid) { $case.payload.rooms[0].room } else { '' }
    try {
      foreach ($row in (Get-Q2DzJoinableRooms $case.payload)) {
        $selectionCount++
        if ($row.room -eq $expectedRoomId) { $selectedFound = $true }
      }
    } catch { $selectionThrew = $true }
    if ($case.valid) {
      Assert-True $selectedFound "valid room case $($case.name) reaches selection"
      if ($case.PSObject.Properties.Name -contains 'expected') {
        $row = $case.payload.rooms[0]
        Assert-Equal $case.expected.alias (Get-Q2DzRoomAlias $row) "shared alias $($case.name)"
        $actualRange = @(0..4 | Where-Object { (Get-Q2DzSlotKind $_) -ceq (Get-Q2DzRoomKind $row) })
        Assert-Equal ($case.expected.slotRange -join ',') (($actualRange[0],$actualRange[-1]) -join ',') "shared slots $($case.name)"
      }
    } elseif (-not $selectionThrew) {
      Assert-Equal 0 $selectionCount "invalid room case $($case.name) does not reach selection"
    }
  }
}

Assert-True (Test-Q2DzEndpoint $Q2DZ_ROOM_ENDPOINT) 'room endpoint must be allow-listed'
Assert-True (Test-Q2DzEndpoint $Q2DZ_LOOKUP_ENDPOINT) 'lookup endpoint must be allow-listed'
Assert-True (Test-Q2DzEndpoint $Q2DZ_RTC_ENDPOINT) 'RTC endpoint must be allow-listed'
Assert-Throws { Test-Q2DzEndpoint 'http://127.0.0.1:1/' } 'HTTP endpoint must not be allow-listed'
Assert-Throws { Test-Q2DzEndpoint 'https://cloud.dos.zone/room/get?extra=1' } 'endpoint query must not be allow-listed'
Assert-Throws { Test-Q2DzEndpoint 'HTTPS://CLOUD.DOS.ZONE/ROOM/GET' } 'uppercase endpoint must not be allow-listed'

$client = New-Q2DzHttpClient
$server = $null
try {
  $validBody = [Text.Encoding]::UTF8.GetBytes('{"ok":true}')
  $server = Start-TestHttpListener -Body $validBody
  $parsed = Read-Q2DzJson -Client $client -Uri $server.Uri
  Assert-True ($parsed.ok -eq $true) 'localhost JSON should parse through bounded reader'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$server = $null
$originalRoomEndpoint = $Q2DZ_ROOM_ENDPOINT
try {
  $server = Start-TestHttpListener -Body ([Text.Encoding]::UTF8.GetBytes('{"rooms":[]}'))
  $Q2DZ_ROOM_ENDPOINT = $server.Uri
  $catalogObject = Get-Q2DzJson $server.Uri
  Assert-True ($catalogObject.PSObject.Properties.Name -contains 'rooms') 'Get-Q2DzJson must not wrap a top-level object'
  Assert-True ($catalogObject.rooms -is [array]) 'Get-Q2DzJson must preserve nested arrays'
} finally {
  $Q2DZ_ROOM_ENDPOINT = $originalRoomEndpoint
  if ($server) { Stop-TestHttpListener $server }
}

$server = $null
$client = New-Q2DzHttpClient
try {
  $arrayBody = [Text.Encoding]::UTF8.GetBytes('[{"domain":"lvc.example.invalid","iceservers":[{"urls":["stun:example.invalid"]}]}]')
  $server = Start-TestHttpListener -Body $arrayBody
  $arrayParsed = Read-Q2DzJson -Client $client -Uri $server.Uri
  Assert-True ($arrayParsed -is [array]) 'top-level JSON array must remain an array'
  Assert-True (Test-Q2DzRtcPayload $arrayParsed) 'preserved top-level RTC array must validate'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$server = $null
$originalRtcEndpoint = $Q2DZ_RTC_ENDPOINT
try {
  $arrayBody = [Text.Encoding]::UTF8.GetBytes('[{"domain":"lvc.example.invalid","iceservers":[{"urls":["stun:example.invalid"]}]}]')
  $server = Start-TestHttpListener -Body $arrayBody
  $Q2DZ_RTC_ENDPOINT = $server.Uri
  $arrayThroughFetch = @(& { param($Endpoint) Get-Q2DzJson $Endpoint } $server.Uri)
  Assert-Equal 1 $arrayThroughFetch.Count 'one-row RTC response must remain one flat row through FetchJson'
  Assert-True ($arrayThroughFetch[0] -isnot [array]) 'FetchJson must not nest a top-level array'
  Assert-True (Test-Q2DzRtcPayload $arrayThroughFetch) 'flat one-row RTC response must validate'
} finally {
  $Q2DZ_RTC_ENDPOINT = $originalRtcEndpoint
  if ($server) { Stop-TestHttpListener $server }
}

$server = $null
$client = New-Q2DzHttpClient
try {
  $server = Start-TestHttpListener -Body ([Text.Encoding]::UTF8.GetBytes('{}')) -StatusCode 302 -Location 'http://127.0.0.1:9/'
  Assert-Throws { Read-Q2DzJson -Client $client -Uri $server.Uri } 'redirect must be rejected'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$server = $null
$client = New-Q2DzHttpClient
try {
  $server = Start-TestHttpListener -Body ([Text.Encoding]::UTF8.GetBytes('{"ok":true}')) -DelayMilliseconds 1500
  Assert-Throws { Read-Q2DzJson -Client $client -Uri $server.Uri -TimeoutSeconds 1 } 'body deadline must cover delayed body reads'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$server = $null
$client = New-Q2DzHttpClient
try {
  $server = Start-TestHttpListener -Body ([Text.Encoding]::UTF8.GetBytes('{not-json'))
  Assert-Throws { Read-Q2DzJson -Client $client -Uri $server.Uri } 'malformed JSON must be rejected'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$server = $null
$client = New-Q2DzHttpClient
try {
  $large = [Text.Encoding]::UTF8.GetBytes(('x' * (2MB + 1)))
  $server = Start-TestHttpListener -Body (Compress-TestBytes $large) -Gzip $true
  Assert-Throws { Read-Q2DzJson -Client $client -Uri $server.Uri } 'decompressed body over 2 MiB must be rejected'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$server = $null
$client = New-Q2DzHttpClient
try {
  $rawLarge = New-Object byte[] $Q2DZ_MAX_BODY
  $random = [Security.Cryptography.RandomNumberGenerator]::Create()
  try { $random.GetBytes($rawLarge) } finally { $random.Dispose() }
  $rawCompressed = Compress-TestBytes $rawLarge
  Assert-True ($rawCompressed.Length -gt $Q2DZ_MAX_BODY) 'incompressible gzip body must exceed raw limit'
  $server = Start-TestHttpListener -Body $rawCompressed -Gzip $true
  Assert-Throws { Read-Q2DzJson -Client $client -Uri $server.Uri } 'compressed raw body over 2 MiB must be rejected'
} finally {
  if ($server) { Stop-TestHttpListener $server }
  $client.Dispose()
}

$room = [pscustomobject]@{
  room = 'room.q2.test.1'; owner = '1'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Test'; map = 'q2dm1'; maxPlayers = 16 }
}
$rtc = [pscustomobject]@{
  domain = 'lvc.example.invalid'
  iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') })
}
$lookupPayload = [pscustomobject]@{ aliases = @('q2.1') }

# Live hub slot planner tests are pure: no network, timer, process or content
# checker is involved.  They describe the bounded state transitions before
# the launcher starts using them.
$slot = {
  param([string]$RoomId = '', [string]$Config = '', [int]$Misses = 0, [string]$MapName = '')
  New-Q2DzHubSlot $RoomId $Config $Misses $MapName
}
$q2Slots = {
  param($First = $null, $Second = $null, $Dedicated = $null)
  @(
    $(if ($Dedicated) { $Dedicated } else { & $slot }), (& $slot), (& $slot),
    $(if ($First) { $First } else { & $slot }),
    $(if ($Second) { $Second } else { & $slot })
  )
}
$roomB = [pscustomobject]@{
  room = 'room.q2.test.2'; owner = '2'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Other'; map = 'q2dm2'; maxPlayers = 16 }
}
$readyRows = @(Get-Q2DzReadyRooms ([pscustomobject]@{ rooms = @($roomB, $room) }))
Assert-Equal $roomB.room $readyRows[0].room 'ready rooms are sorted by name'
Assert-Equal 2 $readyRows.Count 'ready room selection retains all ready rooms, including full rooms'
Assert-Equal 2 (@(Get-Q2DzJoinableRooms ([pscustomobject]@{ rooms = @($roomB, $room) }))).Count 'joinable rooms only apply the capacity filter'
Assert-Equal 1 (@(Select-Q2DzRegisteredRooms -Rows $readyRows -LookupPayload ([pscustomobject]@{ aliases = @('q2.1') }))).Count 'registered selection filters ready rows by alias'

$makeConfig = {
  param($row)
  if ($row.room -eq $room.room) { return 'A' }
  return 'B'
}
$planned = Update-Q2DzHubSlots -Slots @(& $q2Slots (& $slot $room.room 'A' 0)) -Rows $readyRows -MakeConfig $makeConfig
Assert-Equal $room.room $planned[3].RoomId 'existing room is retained'
Assert-Equal 'A' $planned[3].Config 'existing room keeps its original config'
Assert-Equal $roomB.room $planned[4].RoomId 'free slot is filled from ready rows'
Assert-Equal 'B' $planned[4].Config 'new room receives its compact config'

$oneMiss = Update-Q2DzHubSlots -Slots @(& $q2Slots (& $slot $room.room 'A' 0)) -Rows @() -MakeConfig $makeConfig
Assert-Equal $room.room $oneMiss[3].RoomId 'one successful missing poll retains a room'
Assert-Equal 1 $oneMiss[3].Misses 'one successful missing poll increments misses'
$twoMisses = Update-Q2DzHubSlots -Slots @(& $q2Slots (& $slot $room.room 'A' 1)) -Rows @() -MakeConfig $makeConfig
Assert-Equal '' $twoMisses[3].RoomId 'two successful missing polls free a slot'

$fullRoom = $room | Select-Object *
$fullRoom.players = $fullRoom.createSettings.maxPlayers
$retainedFull = Update-Q2DzHubSlots -Slots @(& $q2Slots (& $slot $room.room 'A' 0)) -Rows @($fullRoom) -MakeConfig $makeConfig
Assert-Equal $room.room $retainedFull[3].RoomId 'retained room is not removed when full'
$newFull = Update-Q2DzHubSlots -Slots @(& $q2Slots) -Rows @($fullRoom) -MakeConfig $makeConfig
Assert-Equal '' $newFull[3].RoomId 'new full room does not fill a free slot'

$oversizedConfig = { param($row) if ($row.room -eq $roomB.room) { return ('x' * (256KB)) }; return 'A' }
$oversized = Update-Q2DzHubSlots -Slots @(& $q2Slots (& $slot $room.room 'A' 0)) -Rows @($room, $roomB) -MakeConfig $oversizedConfig
Assert-Equal $room.room $oversized[3].RoomId 'oversized candidate does not disturb retained room'
Assert-Equal '' $oversized[4].RoomId 'oversized candidate is skipped locally'
$threeRows = @($room, $roomB, ($roomB | Select-Object *))
$threeRows[2].room = 'room.q2.test.3'; $threeRows[2].owner = '3'
$threeRows[2].createSettings.name = 'Third'
$threeSlots = Update-Q2DzHubSlots -Slots @(& $q2Slots) -Rows $threeRows -MakeConfig { param($row) return $row.room }
Assert-Equal 2 (@($threeSlots | Where-Object { $_.RoomId })).Count 'hub never admits more than two rooms'

Assert-Equal "0 -`n" (ConvertTo-Q2DzHubCommand 0 (& $slot)) 'empty slot command is independent and newline terminated'
Assert-Equal "1 -`n" (ConvertTo-Q2DzHubCommand 1 (& $slot)) 'second empty slot command is independent and newline terminated'
Assert-Equal "4 -`n" (ConvertTo-Q2DzHubCommand 4 (& $slot)) 'fifth empty slot is accepted'
Assert-Throws { ConvertTo-Q2DzHubCommand 5 (& $slot) } 'sixth hub slot is rejected'
Assert-Throws { ConvertTo-Q2DzHubCommand 0 (& $slot 'room.q2.large' ('x' * (256KB))) } 'oversized hub command is rejected'

$dedicated = [pscustomobject]@{
  room = 'room.q2ded.server.1'; owner = 0; players = 1
  createSettings = [pscustomobject]@{ prefix = 'q2ded'; name = 'Server'; map = 'q2dm1'; maxPlayers = 16 }
}
$dedicatedLookup = [pscustomobject]@{ aliases = @($dedicated.room) }
$dedicatedRtc = @(
  [pscustomobject]@{ domain = 'a.example.invalid'; iceservers = @(
    [pscustomobject]@{ urls = @('stun:shared.example.invalid') },
    [pscustomobject]@{ urls = @('turn:a.example.invalid'); username = 'u'; credential = 'c' }
  ) },
  [pscustomobject]@{ domain = 'b.example.invalid'; iceservers = @(
    [pscustomobject]@{ urls = @('stun:shared.example.invalid') },
    [pscustomobject]@{ urls = @('stun:b.example.invalid') }
  ) }
)
$dedicatedIce = @(Get-Q2DzDedicatedIceServers $dedicatedRtc)
Assert-Equal $dedicated.room (Get-Q2DzRoomAlias $dedicated) 'dedicated room uses its exact room alias'
Assert-Equal 3 $dedicatedIce.Count 'dedicated room flattens and stably deduplicates all RTC rows'
Assert-Equal 'stun:shared.example.invalid' $dedicatedIce[0].urls[0] 'dedicated ICE order keeps the first occurrence'

$dedicatedRows = @($dedicated)
foreach ($number in 2..4) {
  $copy = $dedicated | Select-Object *
  $copy.room = "room.q2ded.server.$number"
  $copy.createSettings = $dedicated.createSettings | Select-Object *
  $copy.createSettings.name = "Server $number"
  $dedicatedRows += $copy
}
$mixedPlan = Update-Q2DzHubSlots -Slots @(& $q2Slots) -Rows @($dedicatedRows + @($room,$roomB)) `
  -MakeConfig { param($row) return ($row.room | ConvertTo-Json -Compress) }
Assert-Equal 3 @($mixedPlan[0..2] | Where-Object { $_.RoomId }).Count 'dedicated pool is capped at three slots'
Assert-Equal 2 @($mixedPlan[3..4] | Where-Object { $_.RoomId }).Count 'legacy q2 pool keeps two slots'

$oldDedicated = & $slot $dedicated.room '{"map":"q2dm0"}' 0 'q2dm0'
$mapChecks = 0
$changedMap = Update-Q2DzHubSlots -Slots @(& $q2Slots $null $null $oldDedicated) -Rows @($dedicated) `
  -MakeConfig { param($row) return ('{"map":"' + $row.createSettings.map + '"}') } `
  -MapAvailable { param($row) $script:mapChecks++; return $true }
Assert-Equal 1 $mapChecks 'dedicated map change is checked once'
Assert-Equal 'q2dm1' $changedMap[0].MapName 'dedicated map state is refreshed'
Assert-True $changedMap[0].Config.Contains('q2dm1') 'dedicated config is refreshed without replacing the slot'

$legacyChangedMap = $room | Select-Object *
$legacyChangedMap.createSettings = $room.createSettings | Select-Object *
$legacyChangedMap.createSettings.map = 'q2dm2'
$legacyMapChecks = 0
$legacyMap = Update-Q2DzHubSlots -Slots @(& $q2Slots (& $slot $room.room '{"map":"q2dm1"}' 0 'q2dm1')) `
  -Rows @($legacyChangedMap) -MakeConfig { param($row) return ('{"map":"' + $row.createSettings.map + '"}') } `
  -MapAvailable { param($row) $script:legacyMapChecks++; return $true }
Assert-Equal 1 $legacyMapChecks 'legacy q2 map change is checked once'
Assert-Equal 'q2dm2' $legacyMap[3].MapName 'legacy q2 map state is refreshed'
Assert-True $legacyMap[3].Config.Contains('q2dm2') 'legacy q2 config follows the changed map'

$unsupportedProfileRoom = [pscustomobject]@{
  room = 'room.q2.unsupported-profile.1'; owner = '1'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; profile = 'baseq2'; name = 'Unsupported'; map = 'q2dm1'; maxPlayers = 16 }
}
$nonQ2Room = [pscustomobject]@{
  room = 'room.q3.unsupported.1'; owner = '1'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q3'; name = 'Unsupported'; map = 'q3dm1'; maxPlayers = 16 }
}
foreach ($unsupportedRoom in @($unsupportedProfileRoom,$nonQ2Room)) {
  $transportFetches = @()
  Assert-Throws {
    Get-Q2DzSelectedTransport -Room $unsupportedRoom -FetchJson {
      param($Endpoint)
      $script:transportFetches += $Endpoint
      throw 'credential endpoint must not be called'
    }
  } 'unsupported room must be rejected before credential fetch'
  Assert-Equal 0 $transportFetches.Count 'unsupported room must fetch zero credential endpoints'
}
$transportFetches = @()
$transport = Get-Q2DzSelectedTransport -Room $room -FetchJson {
  param($Endpoint)
  $script:transportFetches += $Endpoint
  if ($Endpoint -ceq $Q2DZ_LOOKUP_ENDPOINT) { return $lookupPayload }
  if ($Endpoint -ceq $Q2DZ_RTC_ENDPOINT) { return @($rtc) }
  throw "unexpected endpoint: $Endpoint"
}
Assert-Equal 2 $transportFetches.Count 'supported room fetches lookup then RTC exactly once'
Assert-Equal $Q2DZ_LOOKUP_ENDPOINT $transportFetches[0] 'lookup precedes RTC credential fetch'
Assert-Equal $Q2DZ_RTC_ENDPOINT $transportFetches[1] 'RTC credential fetch follows validation and lookup'
Assert-Equal 'q2.1' $transport.Alias 'supported transport returns confirmed catalog alias'

$secondRtc = [pscustomobject]@{
  domain = 'lvc2.example.invalid'
  iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') })
}
$noEnumerateRtcRows = @($rtc, $secondRtc)
$transport = Get-Q2DzSelectedTransport -Room $room -FetchJson {
  param($Endpoint)
  if ($Endpoint -ceq $Q2DZ_LOOKUP_ENDPOINT) { return $lookupPayload }
  if ($Endpoint -ceq $Q2DZ_RTC_ENDPOINT) {
    Write-Output -NoEnumerate $noEnumerateRtcRows
    return
  }
  throw "unexpected endpoint: $Endpoint"
}
Assert-Equal 'lvc.example.invalid' $transport.Rtc.domain 'no-enumerated multi-row RTC payload remains flat'

$mixedRows = @(
  $room,
  [pscustomobject]@{
    room = 'room.q2.bad.1'; owner = '2'; roomReady = $true; players = $true; rtcServer = 'lvc.example.invalid'
    createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Bad'; map = 'q2dm1'; maxPlayers = 16 }
  },
  [pscustomobject]@{
    room = 'room.q3.ignore.1'; owner = '3'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
    createSettings = [pscustomobject]@{ prefix = 'q3'; name = 'Q3'; map = 'q3dm1'; maxPlayers = 16 }
  }
)
$mixedSelectionCount = 0
$mixedSelected = $false
foreach ($row in (Get-Q2DzJoinableRooms ([pscustomobject]@{ rooms = $mixedRows }))) {
  $mixedSelectionCount++
  if ($row.room -eq $room.room) { $mixedSelected = $true }
}
Assert-Equal 1 $mixedSelectionCount 'mixed catalog exposes only valid Q2 rows'
Assert-True $mixedSelected 'mixed catalog keeps valid Q2 row selectable'

$staleRoom = [pscustomobject]@{
  room = 'room.q2.stale.1'; owner = '2'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Stale'; map = 'q2dm1'; maxPlayers = 16 }
}
$registeredCount = 0
$registeredSelected = $false
foreach ($row in (Get-Q2DzRegisteredRooms -Catalog ([pscustomobject]@{ rooms = @($room,$staleRoom) }) -LookupPayload $lookupPayload)) {
  $registeredCount++
  if ($row.room -eq $staleRoom.room) { $registeredSelected = $true }
}
Assert-Equal 1 $registeredCount 'initial list is filtered by registered aliases'
Assert-True (-not $registeredSelected) 'stale alias is hidden before display'

$caseAliasLookup = [pscustomobject]@{ aliases = @('Q2.1') }
$caseRegistered = @(Get-Q2DzRegisteredRooms -Catalog ([pscustomobject]@{ rooms = @($room) }) -LookupPayload $caseAliasLookup)
Assert-Equal 0 $caseRegistered.Count 'aliases differing only by case must not register a room'
Assert-Throws { Test-Q2DzSelectedRoom -Room $room -LookupPayload $caseAliasLookup -RtcPayload @($rtc) } 'case-mismatched alias must be rejected'

Assert-True (Test-Q2DzSelectedRoom -Room $room -LookupPayload $lookupPayload -RtcPayload @($rtc)) 'fresh selected room must validate'
Assert-Throws { Test-Q2DzSelectedRoom -Room $room -LookupPayload ([pscustomobject]@{ aliases = @('q2.other') }) -RtcPayload @($rtc) } 'alias mismatch must be rejected'
Assert-Throws { Test-Q2DzSelectedRoom -Room $room -LookupPayload $lookupPayload -RtcPayload @([pscustomobject]@{ domain = 'https://lvc.example.invalid'; iceservers = @($rtc.iceservers) }) } 'URL-shaped RTC domain must be rejected'
Assert-Throws { Test-Q2DzSelectedRoom -Room $room -LookupPayload $lookupPayload -RtcPayload @([pscustomobject]@{ domain = 'lvc.example.invalid'; ordered = $true; iceservers = @($rtc.iceservers) }) } 'ordered RTC must be rejected'
Assert-Equal $false (Test-Q2DzRoomPayload ([pscustomobject]@{ rooms = $room })) 'scalar rooms must be rejected'
Assert-Equal $false (Test-Q2DzAliasPayload ([pscustomobject]@{ aliases = 'q2.1' })) 'scalar aliases must be rejected'
Assert-Equal $false (Test-Q2DzRtcPayload $rtc) 'scalar RTC top-level must be rejected'
Assert-Equal $false (Test-Q2DzRtcPayload @([pscustomobject]@{ domain = 'lvc.example.invalid'; iceservers = $rtc.iceservers[0] })) 'scalar iceservers must be rejected'
Assert-Throws { Get-Q2DzJoinableRooms ([pscustomobject]@{ rooms = $room }) } 'scalar rooms must not reach selection'
$duplicateRoom = [pscustomobject]@{
  room = $room.room; owner = '2'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Duplicate'; map = 'q2dm1'; maxPlayers = 16 }
}
Assert-Throws { Get-Q2DzJoinableRooms ([pscustomobject]@{ rooms = @($room, $duplicateRoom) }) } 'duplicate room ids must not reach selection'
$caseRoom = [pscustomobject]@{
  room = 'ROOM.Q2.TEST.1'; owner = '1'; roomReady = $true; players = 1; rtcServer = 'lvc.example.invalid'
  createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'Case variant'; map = 'q2dm1'; maxPlayers = 16 }
}
Assert-Equal 2 (@(Get-Q2DzJoinableRooms ([pscustomobject]@{ rooms = @($room, $caseRoom) }))).Count 'room ids differing only by case are distinct'

$launcherText = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\q2dz-launch.ps1') -Raw
Assert-True ($launcherText.Contains('The launcher has no room picker')) 'launcher keeps room selection inside Q2PRO-X'
Assert-True (-not $launcherText.Contains('Open in Q2PRO-X')) 'launcher has no duplicate room action'
Assert-True (-not $launcherText.Contains('ListBox')) 'launcher has no preliminary room chooser'
Assert-True (-not $launcherText.Contains('--hub')) 'launcher uses the coordinator''s only supported mode'
Assert-True ($launcherText.Contains("'--gateway', " + '$gateway')) 'launcher invokes the single hub coordinator mode'
Assert-True ($launcherText.Contains('Get-Q2DzLiveHubSlots')) 'launcher refreshes the live hub state'
Assert-True ($launcherText.Contains('Update-Q2DzHubSlots')) 'launcher uses the bounded slot planner'
Assert-True ($launcherText.Contains('ConvertTo-Q2DzHubCommand')) 'launcher sends independent bounded slot commands'
Assert-True ($launcherText.Contains('$rtcState')) 'launcher fetches one RTC snapshot while filling slots'
Assert-True (-not $launcherText.Contains('Get-Q2DzSelectedTransport -Room $candidate')) 'launcher does not refetch transport data per room'
Assert-True ($launcherText.Contains('RedirectStandardInput')) 'launcher uses a real redirected coordinator stdin'
Assert-True ($launcherText.Contains('StandardInput.BaseStream')) 'launcher writes coordinator stdin as explicit bytes'
Assert-True ($launcherText.Contains('UTF8Encoding')) 'launcher encodes coordinator stdin as UTF-8 without BOM'

Write-Output ('catalog contract tests: PASS ({0} shared cases)' -f $cases.Count)
