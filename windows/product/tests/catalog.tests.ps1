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
Assert-True ($launcherText.Contains('The launcher has no room picker')) 'launcher starts the Custom browser directly'
Assert-True (-not $launcherText.Contains('Open in Q2PRO-X')) 'launcher has no duplicate room action'
Assert-True (-not $launcherText.Contains('ListBox')) 'launcher has no preliminary room chooser'
Assert-True (-not $launcherText.Contains('--hub')) 'launcher uses the coordinator''s only supported mode'
Assert-True ($launcherText.Contains("'--gateway', " + '$gateway')) 'launcher invokes the single hub coordinator mode'
Assert-True ($launcherText.Contains('$hubInput')) 'launcher sends bounded hub configurations through stdin'
Assert-True ($launcherText.Contains('$hubMaxContentChecks = 8')) 'launcher bounds content-check work for a large catalog'
Assert-True ($launcherText.Contains('$rtcRows = Get-Q2DzJson $Q2DZ_RTC_ENDPOINT')) 'launcher fetches one RTC snapshot before checking rooms'
Assert-True (-not $launcherText.Contains('Get-Q2DzSelectedTransport -Room $candidate')) 'launcher does not refetch transport data per room'
Assert-True ($launcherText.Contains('[Environment]::NewLine')) 'launcher separates hub room configurations by newline'
Assert-True ($launcherText.Contains('RedirectStandardInput')) 'launcher uses a real redirected coordinator stdin'
Assert-True ($launcherText.Contains('StandardInput.BaseStream')) 'launcher writes coordinator stdin as explicit bytes'
Assert-True ($launcherText.Contains('UTF8Encoding')) 'launcher encodes coordinator stdin as UTF-8 without BOM'

Write-Output ('catalog contract tests: PASS ({0} shared cases)' -f $cases.Count)
