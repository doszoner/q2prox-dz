$null = Add-Type -AssemblyName System.Net.Http

$Q2DZ_ROOM_ENDPOINT = 'https://cloud.dos.zone/room/get'
$Q2DZ_LOOKUP_ENDPOINT = 'https://net.dos.zone/lookup'
$Q2DZ_RTC_ENDPOINT = 'https://cloud.dos.zone/rtc/servers'
$Q2DZ_GAME = '3zb2'
$Q2DZ_ORDERED = $false
$Q2DZ_MAX_BODY = 2MB
$Q2DZ_MAX_TOKEN = 255
$Q2DZ_MAX_JSON_SAFE_INTEGER = 9007199254740991
$Q2DZ_MAX_MAP = 128
$Q2DZ_MAX_PLAYERS = 64
$Q2DZ_MAX_ICE_SERVERS = 16
$Q2DZ_MAX_ICE_URL = 2048
$Q2DZ_MAX_ICE_FIELD = 1024
$Q2DZ_MAX_HUB_COMMAND_BYTES = 256KB
$Q2DZ_HUB_MAX_CONTENT_CHECKS = 8

function Test-Q2DzEndpoint {
  param([Parameter(Mandatory = $true)][string]$Uri)
  if (@($Q2DZ_ROOM_ENDPOINT,$Q2DZ_LOOKUP_ENDPOINT,$Q2DZ_RTC_ENDPOINT) -cnotcontains $Uri) {
    throw 'unapproved Dos.Zone endpoint'
  }
  return $true
}

function New-Q2DzHttpClient {
  $handler = New-Object System.Net.Http.HttpClientHandler
  $handler.AllowAutoRedirect = $false
  $handler.AutomaticDecompression = [System.Net.DecompressionMethods]::None
  $client = New-Object -TypeName System.Net.Http.HttpClient -ArgumentList @($handler)
  $client.Timeout = [TimeSpan]::FromSeconds(15)
  $client.DefaultRequestHeaders.Accept.Clear()
  [void]$client.DefaultRequestHeaders.Accept.ParseAdd('application/json')
  [void]$client.DefaultRequestHeaders.AcceptEncoding.ParseAdd('gzip')
  [void]$client.DefaultRequestHeaders.UserAgent.ParseAdd('q2pro-x-doszone/1.0')
  return $client
}

function Read-Q2DzJson {
  param(
    [Parameter(Mandatory = $true)][System.Net.Http.HttpClient]$Client,
    [Parameter(Mandatory = $true)][string]$Uri,
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 15
  )
  $response = $null
  $stream = $null
  $rawMemory = $null
  $rawStream = $null
  $decoder = $null
  $decodedMemory = $null
  $cancellation = New-Object -TypeName System.Threading.CancellationTokenSource -ArgumentList @(
    [TimeSpan]::FromSeconds($TimeoutSeconds)
  )
  try {
    $response = $Client.GetAsync(
      $Uri,
      [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
      $cancellation.Token
    ).GetAwaiter().GetResult()
    if (-not $response.IsSuccessStatusCode) {
      throw 'Dos.Zone endpoint returned a non-success status'
    }
    $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
    $encodingCount = 0
    $encoding = ''
    foreach ($value in $response.Content.Headers.ContentEncoding) {
      $encodingCount++
      $encoding = [string]$value
    }
    if ($encodingCount -gt 1 -or ($encodingCount -eq 1 -and
        -not [string]::Equals($encoding, 'gzip', [StringComparison]::OrdinalIgnoreCase))) {
      throw 'Dos.Zone endpoint returned an unsupported content encoding'
    }
    $isGzip = $encodingCount -eq 1
    $rawMemory = New-Object System.IO.MemoryStream
    $buffer = New-Object byte[] 65536
    while (($read = $stream.ReadAsync($buffer, 0, $buffer.Length, $cancellation.Token).GetAwaiter().GetResult()) -gt 0) {
      if (($rawMemory.Length + $read) -gt $Q2DZ_MAX_BODY) {
        throw 'Dos.Zone response is too large before decompression'
      }
      $rawMemory.Write($buffer, 0, $read)
    }
    $rawBytes = $rawMemory.ToArray()
    if ($isGzip) {
      $rawStream = New-Object System.IO.MemoryStream
      $rawStream.Write($rawBytes, 0, $rawBytes.Length)
      $rawStream.Position = 0
      $decoder = New-Object -TypeName System.IO.Compression.GZipStream -ArgumentList @(
        $rawStream,
        [System.IO.Compression.CompressionMode]::Decompress,
        $true
      )
      $decodedMemory = New-Object System.IO.MemoryStream
      while (($read = $decoder.ReadAsync($buffer, 0, $buffer.Length, $cancellation.Token).GetAwaiter().GetResult()) -gt 0) {
        if (($decodedMemory.Length + $read) -gt $Q2DZ_MAX_BODY) {
          throw 'Dos.Zone response is too large after decompression'
        }
        $decodedMemory.Write($buffer, 0, $read)
      }
      $bytes = $decodedMemory.ToArray()
    } else {
      $bytes = $rawBytes
    }
    $utf8 = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList @($false, $true)
    $text = $utf8.GetString($bytes)
    $isJsonArray = $text.TrimStart().StartsWith('[')
    try {
      $value = $text | ConvertFrom-Json -ErrorAction Stop
    } catch {
      throw 'Dos.Zone endpoint returned invalid JSON'
    }
    if ($isJsonArray -and $null -eq $value) { $value = @() }
    if ($isJsonArray -and $value -isnot [array]) { $value = @($value) }
    if ($null -eq $value) { throw 'Dos.Zone endpoint returned an empty JSON value' }
    Write-Output -NoEnumerate $value
  } finally {
    if ($stream) { $stream.Dispose() }
    if ($response) { $response.Dispose() }
    if ($decoder) { $decoder.Dispose() }
    if ($decodedMemory) { $decodedMemory.Dispose() }
    if ($rawStream) { $rawStream.Dispose() }
    if ($rawMemory) { $rawMemory.Dispose() }
    $cancellation.Dispose()
  }
}

function Get-Q2DzJson {
  param([Parameter(Mandatory = $true)][string]$Endpoint)
  Test-Q2DzEndpoint $Endpoint | Out-Null
  $client = New-Q2DzHttpClient
  try {
    $value = Read-Q2DzJson -Client $client -Uri $Endpoint
    Write-Output $value
  }
  finally { $client.Dispose() }
}

function Test-Q2DzSafeToken {
  param($Value, [int]$Maximum = $Q2DZ_MAX_TOKEN)
  if ($Value -isnot [string] -or $Value.Length -lt 1 -or $Value.Length -gt $Maximum) { return $false }
  return $Value -cmatch '^[A-Za-z0-9._-]+\z'
}

function Test-Q2DzText {
  param($Value, [int]$Maximum = $Q2DZ_MAX_TOKEN)
  if ($Value -isnot [string] -or $Value.Length -lt 1 -or $Value.Length -gt $Maximum) { return $false }
  return $Value -notmatch '[\r\n\t]'
}

function Test-Q2DzInteger {
  param($Value, [long]$Minimum, [long]$Maximum)
  if ($Value -is [bool]) { return $false }
  $kind = [Convert]::GetTypeCode($Value)
  $integerKinds = @(
    [TypeCode]::Byte, [TypeCode]::SByte, [TypeCode]::Int16, [TypeCode]::UInt16,
    [TypeCode]::Int32, [TypeCode]::UInt32, [TypeCode]::Int64, [TypeCode]::UInt64
  )
  if ($integerKinds -notcontains $kind) { return $false }
  try { $number = [decimal]$Value } catch { return $false }
  return $number -ge $Minimum -and $number -le $Maximum
}

function Test-Q2DzHostname {
  param($Value)
  if ($Value -isnot [string] -or $Value.Length -lt 1 -or $Value.Length -gt 253) { return $false }
  if ($Value.Contains('/') -or $Value.Contains(':') -or $Value.Contains('@') -or $Value.Contains(' ')) { return $false }
  return $Value -cmatch '^(?=.{1,253}\z)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\z'
}

function Test-Q2DzIceUrl {
  param($Value)
  if ($Value -isnot [string] -or $Value.Length -lt 7 -or $Value.Length -gt $Q2DZ_MAX_ICE_URL) { return $false }
  if ($Value -match '\s' -or $Value.Contains('@')) { return $false }
  $match = [regex]::Match($Value, '(?i)^(stun|stuns|turn|turns):(.+)$')
  if (-not $match.Success) { return $false }
  $target = ($match.Groups[2].Value -split '\?', 2)[0]
  if ([string]::IsNullOrEmpty($target)) { return $false }
  $iceHost = $target
  $port = ''
  if ($target.StartsWith('[')) {
    $closing = $target.IndexOf(']')
    if ($closing -lt 0) { return $false }
    $iceHost = $target.Substring(1, $closing - 1)
    $suffix = $target.Substring($closing + 1)
    if ($suffix -and $suffix -notmatch '^:\d{1,5}$') { return $false }
    $port = $suffix.TrimStart(':')
  } else {
    $colonCount = 0
    foreach ($character in $target.ToCharArray()) { if ($character -eq ':') { $colonCount++ } }
    if ($colonCount -gt 1) { return $false }
    $parts = $target.Split(':', 2)
    $iceHost = $parts[0]
    $partCount = 0
    foreach ($part in $parts) { $partCount++ }
    if ($partCount -eq 2) {
      $port = $parts[1]
      if ($port -notmatch '^\d{1,5}$') { return $false }
    }
  }
  if ([string]::IsNullOrEmpty($iceHost)) { return $false }
  $hostOk = $false
  try { [System.Net.IPAddress]::Parse($iceHost) | Out-Null; $hostOk = $true } catch { $hostOk = Test-Q2DzHostname $iceHost }
  if (-not $hostOk) { return $false }
  return (-not $port) -or ([int]$port -ge 1 -and [int]$port -le 65535)
}

function Test-Q2DzIceServer {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject) { return $false }
  if ($Value.PSObject.Properties.Name -notcontains 'urls') { return $false }
  if ($Value.PSObject.Properties.Name -contains 'ordered' -and
      ($Value.ordered -isnot [bool] -or $Value.ordered -ne $false)) { return $false }
  $urls = @($Value.urls)
  $urlCount = 0
  foreach ($url in $urls) { $urlCount++ }
  if ($urlCount -lt 1 -or $urlCount -gt $Q2DZ_MAX_ICE_SERVERS) { return $false }
  foreach ($url in $urls) { if (-not (Test-Q2DzIceUrl $url)) { return $false } }
  foreach ($name in @('username','credential')) {
    if ($Value.PSObject.Properties.Name -contains $name) {
      $field = $Value.$name
      if ($field -isnot [string] -or $field.Length -gt $Q2DZ_MAX_ICE_FIELD) { return $false }
    }
  }
  return $true
}

function Test-Q2DzRtcRow {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject) { return $false }
  if ($Value.PSObject.Properties.Name -notcontains 'domain' -or
      $Value.PSObject.Properties.Name -notcontains 'iceservers') { return $false }
  if ($Value.PSObject.Properties.Name -contains 'ordered' -and
      ($Value.ordered -isnot [bool] -or $Value.ordered -ne $false)) { return $false }
  if (-not (Test-Q2DzHostname $Value.domain)) { return $false }
  if ($Value.iceservers -isnot [array]) { return $false }
  $servers = @($Value.iceservers)
  $serverCount = 0
  foreach ($server in $servers) { $serverCount++ }
  if ($serverCount -lt 1 -or $serverCount -gt $Q2DZ_MAX_ICE_SERVERS) { return $false }
  foreach ($server in $servers) { if (-not (Test-Q2DzIceServer $server)) { return $false } }
  return $true
}

function Test-Q2DzRtcPayload {
  param($Value)
  if ($null -eq $Value -or $Value -isnot [array]) { return $false }
  $rows = @($Value)
  $rowCount = 0
  foreach ($row in $rows) {
    $rowCount++
    if (-not (Test-Q2DzRtcRow $row)) { return $false }
  }
  if ($rowCount -lt 1) { return $false }
  return $true
}

function Test-Q2DzAliasPayload {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject -or
      $Value.PSObject.Properties.Name -notcontains 'aliases') { return $false }
  if ($Value.aliases -isnot [array]) { return $false }
  $aliases = @($Value.aliases)
  foreach ($alias in $aliases) { if (-not (Test-Q2DzSafeToken $alias)) { return $false } }
  return $true
}

function Test-Q2DzRoomRow {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject) { return $false }
  $settings = $Value.createSettings
  if ($null -eq $settings -or $null -eq $settings.PSObject -or $settings.prefix -cne 'q2') { return $false }
  foreach ($name in @('game','profile')) {
    if ($settings.PSObject.Properties.Name -contains $name -and $settings.$name -cne $Q2DZ_GAME) { return $false }
  }
  if ($settings.PSObject.Properties.Name -contains 'ordered' -and
      ($settings.ordered -isnot [bool] -or $settings.ordered -ne $Q2DZ_ORDERED)) { return $false }
  if (-not (Test-Q2DzSafeToken $Value.room)) { return $false }
  if ($Value.owner -is [bool] -or ($Value.owner -isnot [string] -and
      -not (Test-Q2DzInteger $Value.owner 0 $Q2DZ_MAX_JSON_SAFE_INTEGER))) { return $false }
  if ($Value.owner -is [string] -and -not (Test-Q2DzSafeToken $Value.owner 250)) { return $false }
  if ($Value.roomReady -isnot [bool]) { return $false }
  if (-not (Test-Q2DzHostname $Value.rtcServer)) { return $false }
  if (-not (Test-Q2DzText $settings.name)) { return $false }
  if ($settings.map -isnot [string] -or $settings.map.Length -lt 1 -or $settings.map.Length -gt $Q2DZ_MAX_MAP -or
      $settings.map -notmatch '^[A-Za-z0-9._-]+\z') { return $false }
  if (-not (Test-Q2DzInteger $settings.maxPlayers 1 $Q2DZ_MAX_PLAYERS)) { return $false }
  if (-not (Test-Q2DzInteger $Value.players 0 ([long]$settings.maxPlayers))) { return $false }
  return $true
}

function New-Q2DzHubSlot {
  param(
    [string]$RoomId = '',
    [string]$Config = '',
    [int]$Misses = 0
  )
  return [pscustomobject]@{ RoomId = $RoomId; Config = $Config; Misses = $Misses }
}

function Get-Q2DzReadyRooms {
  param([Parameter(Mandatory = $true)]$Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject -or
      $Value.PSObject.Properties.Name -notcontains 'rooms') {
    throw 'room catalog has an invalid schema'
  }
  if ($Value.rooms -isnot [array]) { throw 'room catalog has an invalid schema' }
  $result = @()
  $seenRooms = @()
  foreach ($row in @($Value.rooms)) {
    if (-not (Test-Q2DzRoomRow $row)) { continue }
    if ($seenRooms -ccontains [string]$row.room) { throw 'room catalog has duplicate room ids' }
    $seenRooms += [string]$row.room
    if ($row.roomReady) { $result += $row }
  }
  return @($result | Sort-Object `
    @{ Expression = { $_.createSettings.name.ToLowerInvariant() } }, `
    @{ Expression = { [string]$_.room } })
}

function Select-Q2DzRegisteredRooms {
  param(
    [Parameter(Mandatory = $true)]$Rows,
    [Parameter(Mandatory = $true)]$LookupPayload
  )
  if (-not (Test-Q2DzAliasPayload $LookupPayload)) { throw 'alias lookup has an invalid schema' }
  $aliases = @($LookupPayload.aliases)
  $result = @()
  foreach ($row in @($Rows)) {
    if ($aliases -ccontains ('q2.{0}' -f ([string]$row.owner))) { $result += $row }
  }
  return @($result)
}

function ConvertTo-Q2DzHubCommand {
  param(
    [int]$Index,
    [Parameter(Mandatory = $true)]$Slot
  )
  if ($Index -lt 0 -or $Index -gt 1) { throw 'invalid hub slot' }
  $value = if ([string]::IsNullOrEmpty($Slot.RoomId)) {
    '-'
  } else {
    ('{0} {1}' -f $Slot.RoomId, $Slot.Config)
  }
  $text = ('{0} {1}' -f $Index, $value) + "`n"
  if ([Text.Encoding]::UTF8.GetByteCount($text) -gt $Q2DZ_MAX_HUB_COMMAND_BYTES) {
    throw 'hub slot command exceeds 256 KiB'
  }
  return $text
}

function Update-Q2DzHubSlots {
  param(
    [Parameter(Mandatory = $true)][object[]]$Slots,
    [object[]]$Rows = @(),
    [Parameter(Mandatory = $true)][scriptblock]$MakeConfig,
    [int]$MaxCandidates = $Q2DZ_HUB_MAX_CONTENT_CHECKS
  )
  if ($Slots.Count -ne 2) { throw 'hub requires exactly two slots' }
  if ($MaxCandidates -lt 0) { throw 'invalid hub candidate limit' }

  $updated = @()
  foreach ($slot in $Slots) {
    if ([string]::IsNullOrEmpty($slot.RoomId)) {
      $updated += New-Q2DzHubSlot
      continue
    }
    $retained = $null
    foreach ($row in @($Rows)) {
      if ([string]$row.room -ceq [string]$slot.RoomId) { $retained = $row; break }
    }
    if ($null -ne $retained) {
      $updated += New-Q2DzHubSlot ([string]$slot.RoomId) ([string]$slot.Config) 0
    } elseif ([int]$slot.Misses -lt 1) {
      $updated += New-Q2DzHubSlot ([string]$slot.RoomId) ([string]$slot.Config) ([int]$slot.Misses + 1)
    } else {
      $updated += New-Q2DzHubSlot
    }
  }

  $activeIds = @($updated | Where-Object { -not [string]::IsNullOrEmpty($_.RoomId) } |
    ForEach-Object { [string]$_.RoomId })
  $attempted = 0
  foreach ($row in @($Rows)) {
    if ($attempted -ge $MaxCandidates) { break }
    $roomId = [string]$row.room
    if ($activeIds -ccontains $roomId) { continue }
    if ($row.players -ge $row.createSettings.maxPlayers) { continue }
    $emptyIndex = $null
    for ($index = 0; $index -lt $updated.Count; $index++) {
      if ([string]::IsNullOrEmpty($updated[$index].RoomId)) { $emptyIndex = $index; break }
    }
    if ($null -eq $emptyIndex) { break }
    $attempted++
    try {
      $configValues = @(& $MakeConfig $row)
      if ($configValues.Count -ne 1 -or $configValues[0] -isnot [string]) { throw 'room configuration must be one string' }
      $config = [string]$configValues[0]
      if ([string]::IsNullOrEmpty($config) -or $config.Contains("`r") -or $config.Contains("`n")) {
        throw 'room configuration must fit one line'
      }
      $candidate = New-Q2DzHubSlot $roomId $config 0
      ConvertTo-Q2DzHubCommand $emptyIndex $candidate | Out-Null
    } catch {
      continue
    }
    $updated[$emptyIndex] = $candidate
    $activeIds += $roomId
  }
  return @($updated)
}

function Test-Q2DzRoomPayload {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject -or
      $Value.PSObject.Properties.Name -notcontains 'rooms') { return $false }
  if ($Value.rooms -isnot [array]) { return $false }
  $rows = @($Value.rooms)
  $q2Count = 0
  $seenRooms = @()
  foreach ($row in $rows) {
    if ($null -eq $row -or $null -eq $row.PSObject) { continue }
    if ($null -ne $row.createSettings -and $row.createSettings.prefix -ceq 'q2') {
      $q2Count++
      if (-not (Test-Q2DzRoomRow $row)) { return $false }
      if ($seenRooms -ccontains [string]$row.room) { return $false }
      $seenRooms += [string]$row.room
    }
  }
  return $q2Count -gt 0
}

function Get-Q2DzJoinableRooms {
  param($Value)
  return @(Get-Q2DzReadyRooms $Value | Where-Object {
    $_.players -lt $_.createSettings.maxPlayers
  })
}

function Get-Q2DzRegisteredRooms {
  param(
    [Parameter(Mandatory = $true)]$Catalog,
    [Parameter(Mandatory = $true)]$LookupPayload
  )
  return @(Select-Q2DzRegisteredRooms -Rows (Get-Q2DzJoinableRooms $Catalog) -LookupPayload $LookupPayload)
}

function Test-Q2DzSelectedRoom {
  param(
    [Parameter(Mandatory = $true)]$Room,
    [Parameter(Mandatory = $true)]$LookupPayload,
    [Parameter(Mandatory = $true)]$RtcPayload
  )
  if (-not (Test-Q2DzRoomRow $Room) -or -not $Room.roomReady -or $Room.players -ge $Room.createSettings.maxPlayers) {
    throw 'The selected room is no longer available'
  }
  if (-not (Test-Q2DzAliasPayload $LookupPayload)) { throw 'alias lookup has an invalid schema' }
  $alias = 'q2.{0}' -f ([string]$Room.owner)
  if ($LookupPayload.aliases -cnotcontains $alias) { throw 'selected room alias is no longer registered' }
  if (-not (Test-Q2DzRtcPayload $RtcPayload)) { throw 'RTC configuration has an invalid schema' }
  $matchCount = 0
  foreach ($row in $RtcPayload) {
    if ($row.domain -eq $Room.rtcServer) { $matchCount++ }
  }
  if ($matchCount -ne 1) { throw 'fresh RTC configuration is unavailable' }
  return $true
}

function Get-Q2DzSelectedTransport {
  param(
    [Parameter(Mandatory = $true)]$Room,
    [scriptblock]$FetchJson = { param($Endpoint) Get-Q2DzJson $Endpoint }
  )
  if (-not (Test-Q2DzRoomRow $Room) -or -not $Room.roomReady -or $Room.players -ge $Room.createSettings.maxPlayers) {
    throw 'The selected room is no longer available'
  }
  $lookup = & $FetchJson $Q2DZ_LOOKUP_ENDPOINT
  if (-not (Test-Q2DzAliasPayload $lookup)) { throw 'alias lookup has an invalid schema' }
  $alias = 'q2.{0}' -f ([string]$Room.owner)
  if ($lookup.aliases -cnotcontains $alias) { throw 'selected room alias is no longer registered' }
  $rtcRows = & $FetchJson $Q2DZ_RTC_ENDPOINT
  if ($rtcRows -isnot [array]) { $rtcRows = @($rtcRows) }
  Test-Q2DzSelectedRoom -Room $Room -LookupPayload $lookup -RtcPayload $rtcRows | Out-Null
  $rtc = @($rtcRows | Where-Object { $_.domain -eq $Room.rtcServer })[0]
  return [pscustomobject]@{ Alias = $alias; Rtc = $rtc }
}
