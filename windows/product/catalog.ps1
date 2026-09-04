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
$Q2DZ_MAX_GATEWAY_CONFIG_BYTES = 65535
$Q2DZ_MAX_TARGETS = 256
$Q2DZ_MAX_ICE_SNAPSHOT_BYTES = 65536
$Q2DZ_SIGNALING_URL = 'wss://net.js-dos.com:444'

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
  $seen = @()
  foreach ($alias in $aliases) {
    if (-not (Test-Q2DzSafeToken $alias) -or $seen -ccontains [string]$alias) { return $false }
    $seen += [string]$alias
  }
  return $true
}

function Test-Q2DzRoomRow {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject) { return $false }
  $settings = $Value.createSettings
  if ($null -eq $settings -or $null -eq $settings.PSObject -or
      @('q2','q2ded') -cnotcontains [string]$settings.prefix) { return $false }
  $kind = [string]$settings.prefix
  foreach ($name in @('game','profile')) {
    if ($settings.PSObject.Properties.Name -contains $name -and $settings.$name -cne $Q2DZ_GAME) { return $false }
  }
  if ($settings.PSObject.Properties.Name -contains 'ordered' -and
      ($settings.ordered -isnot [bool] -or $settings.ordered -ne $Q2DZ_ORDERED)) { return $false }
  if (-not (Test-Q2DzSafeToken $Value.room)) { return $false }
  if ($kind -ceq 'q2') {
    if ($Value.owner -is [bool] -or ($Value.owner -isnot [string] -and
        -not (Test-Q2DzInteger $Value.owner 0 $Q2DZ_MAX_JSON_SAFE_INTEGER))) { return $false }
    if ($Value.owner -is [string] -and -not (Test-Q2DzSafeToken $Value.owner 250)) { return $false }
    if ($Value.roomReady -isnot [bool]) { return $false }
    if (-not (Test-Q2DzHostname $Value.rtcServer)) { return $false }
  } elseif (-not ([string]$Value.room).StartsWith('room.q2ded.', [StringComparison]::Ordinal) -or
            ([string]$Value.room).Length -le ('room.q2ded.').Length) { return $false }
  if (-not (Test-Q2DzText $settings.name)) { return $false }
  if ($settings.map -isnot [string] -or $settings.map.Length -lt 1 -or $settings.map.Length -gt $Q2DZ_MAX_MAP -or
      $settings.map -notmatch '^[A-Za-z0-9._-]+\z') { return $false }
  if (-not (Test-Q2DzInteger $settings.maxPlayers 1 $Q2DZ_MAX_PLAYERS)) { return $false }
  if (-not (Test-Q2DzInteger $Value.players 0 ([long]$settings.maxPlayers))) { return $false }
  return $true
}

function New-Q2DzTarget {
  param(
    [string]$RoomId = '',
    [string]$Config = '',
    [int]$Misses = 0,
    [string]$MapName = ''
  )
  return [pscustomobject]@{ RoomId = $RoomId; Config = $Config; Misses = $Misses; MapName = $MapName }
}

function New-Q2DzTargetMap {
  return [System.Collections.Hashtable]::new([StringComparer]::Ordinal)
}

function Get-Q2DzRoomKind { param($Room) return [string]$Room.createSettings.prefix }
function Get-Q2DzRoomAlias {
  param($Room)
  if ((Get-Q2DzRoomKind $Room) -ceq 'q2ded') { return [string]$Room.room }
  return 'q2.{0}' -f ([string]$Room.owner)
}

function Sort-Q2DzStrings {
  param([string[]]$Values)
  $result = @($Values)
  if ($result.Count -gt 1) { [Array]::Sort($result, [StringComparer]::Ordinal) }
  return $result
}

function Sort-Q2DzRoomRows {
  param([object[]]$Rows)
  $result = @($Rows)
  if ($result.Count -gt 1) {
    $comparer = [System.Collections.Generic.Comparer[object]]::Create(
      [System.Comparison[object]]{
        param($Left, $Right)
        return [StringComparer]::Ordinal.Compare([string]$Left.room, [string]$Right.room)
      })
    [Array]::Sort($result, $comparer)
  }
  return $result
}

function ConvertTo-Q2DzCanonicalJsonValue {
  param($Value)
  if ($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]) { return $Value }
  if ($Value -is [array]) {
    $items = @($Value | ForEach-Object { ConvertTo-Q2DzCanonicalJsonValue $_ })
    Write-Output -NoEnumerate $items
    return
  }
  $result = [ordered]@{}
  foreach ($property in @($Value.PSObject.Properties | Sort-Object Name)) {
    $result[$property.Name] = ConvertTo-Q2DzCanonicalJsonValue $property.Value
  }
  return $result
}

function Get-Q2DzDedicatedIceServers {
  param([Parameter(Mandatory = $true)]$RtcPayload)
  if (-not (Test-Q2DzRtcPayload $RtcPayload)) { throw 'RTC configuration has an invalid schema' }
  $result = @()
  $seen = @()
  foreach ($row in @($RtcPayload)) {
    foreach ($server in @($row.iceservers)) {
      $key = ConvertTo-Q2DzCanonicalJsonValue $server | ConvertTo-Json -Depth 8 -Compress
      if ($seen -cnotcontains $key) {
        $seen += $key
        $result += $server
      }
    }
  }
  if ($result.Count -lt 1 -or $result.Count -gt $Q2DZ_MAX_ICE_SERVERS) {
    throw 'global RTC configuration exceeds the supported limit'
  }
  $serialized = $null
  $bytes = $null
  try {
    $serialized = ConvertTo-Json -InputObject ([array]@($result)) -Depth 8 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($serialized)
    if ($bytes.Length -gt $Q2DZ_MAX_ICE_SNAPSHOT_BYTES) {
      throw 'global ICE snapshot exceeds 65536 bytes'
    }
  }
  finally {
    if ($bytes) { [Array]::Clear($bytes, 0, $bytes.Length) }
    $serialized = $null
  }
  return @($result)
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
  $seenAliases = @()
  foreach ($row in @($Value.rooms)) {
    if (-not (Test-Q2DzRoomRow $row)) { continue }
    if ($seenRooms -ccontains [string]$row.room) { throw 'room catalog has duplicate room ids' }
    $seenRooms += [string]$row.room
    $alias = Get-Q2DzRoomAlias $row
    if ($seenAliases -ccontains $alias) { throw 'room catalog has duplicate Q2 aliases' }
    $seenAliases += $alias
    if ((Get-Q2DzRoomKind $row) -ceq 'q2ded' -or $row.roomReady) { $result += $row }
  }
  return @(Sort-Q2DzRoomRows $result)
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
    if ($aliases -ccontains (Get-Q2DzRoomAlias $row)) { $result += $row }
  }
  return @($result)
}

function Copy-Q2DzTargetMap {
  param([Parameter(Mandatory = $true)]$Targets)
  if ($Targets -isnot [hashtable]) { throw 'hub target state must be a room map' }
  $result = New-Q2DzTargetMap
  foreach ($key in @($Targets.Keys)) {
    $target = $Targets[$key]
    if ($null -eq $target -or [string]$target.RoomId -cne [string]$key -or
        [string]::IsNullOrEmpty([string]$target.Config)) {
      throw 'hub target state key does not match room id'
    }
    $result[[string]$key] = New-Q2DzTarget ([string]$target.RoomId) ([string]$target.Config) ([int]$target.Misses) ([string]$target.MapName)
  }
  if ($result.Count -gt $Q2DZ_MAX_TARGETS) { throw 'catalog_capacity_exceeded' }
  return $result
}

function Get-Q2DzTargetPlan {
  param(
    [Parameter(Mandatory = $true)]$Targets,
    [object[]]$Rows = @(),
    [Parameter(Mandatory = $true)][scriptblock]$MakeConfig,
    [scriptblock]$MapAvailable = { param($row) return $true }
  )
  $current = Copy-Q2DzTargetMap $Targets
  $rows = @($Rows)
  $seenRooms = New-Q2DzTargetMap
  $seenAliases = New-Q2DzTargetMap
  foreach ($row in $rows) {
    if (-not (Test-Q2DzRoomRow $row)) { throw 'room catalog has an invalid schema' }
    $roomId = [string]$row.room
    if ($seenRooms.ContainsKey($roomId)) { throw 'room catalog has duplicate room ids' }
    $seenRooms[$roomId] = $true
    $alias = Get-Q2DzRoomAlias $row
    if ($seenAliases.ContainsKey($alias)) { throw 'room catalog has duplicate Q2 aliases' }
    $seenAliases[$alias] = $true
  }
  $desired = @($rows | Where-Object { $_.players -lt $_.createSettings.maxPlayers })
  if ($desired.Count -gt $Q2DZ_MAX_TARGETS) { throw 'catalog_capacity_exceeded' }

  $updated = New-Q2DzTargetMap
  $drops = [System.Collections.Generic.List[string]]::new()
  foreach ($roomId in (Sort-Q2DzStrings @($current.Keys))) {
    $target = $current[$roomId]
    $row = @($rows | Where-Object { [string]$_.room -ceq [string]$roomId }) | Select-Object -First 1
    if ($null -eq $row) {
      if ([int]$target.Misses -lt 1) {
        $updated[$roomId] = New-Q2DzTarget $target.RoomId $target.Config ([int]$target.Misses + 1) $target.MapName
      } else {
        [void]$drops.Add([string]$roomId)
      }
      continue
    }
    $map = [string]$row.createSettings.map
    $values = @(& $MakeConfig $row)
    if ($values.Count -ne 1 -or $values[0] -isnot [string]) { throw 'room configuration must be one string' }
    $config = [string]$values[0]
    $changed = $config -cne [string]$target.Config
    $mapChanged = -not [string]::IsNullOrEmpty([string]$target.MapName) -and [string]$target.MapName -cne $map
    if ($changed -and $mapChanged -and -not (& $MapAvailable $row)) { throw 'map content check failed' }
    $savedConfig = if ($changed) { $config } else { [string]$target.Config }
    $updated[$roomId] = New-Q2DzTarget $target.RoomId $savedConfig 0 $map
    if ($changed) { [void]$drops.Add([string]$roomId) }
  }

  foreach ($row in (Sort-Q2DzRoomRows $desired)) {
    $roomId = [string]$row.room
    if ($updated.ContainsKey($roomId)) { continue }
    if (-not (& $MapAvailable $row)) { throw 'map content check failed' }
    if ($updated.Count -ge $Q2DZ_MAX_TARGETS) { continue }
    $values = @(& $MakeConfig $row)
    if ($values.Count -ne 1 -or $values[0] -isnot [string]) { throw 'room configuration must be one string' }
    $config = [string]$values[0]
    if ([string]::IsNullOrEmpty($config) -or $config.Contains("`r") -or $config.Contains("`n")) {
      throw 'room configuration must fit one line'
    }
    $updated[$roomId] = New-Q2DzTarget $roomId $config 0 ([string]$row.createSettings.map)
  }
  if ($updated.Count -gt $Q2DZ_MAX_TARGETS) { throw 'catalog_capacity_exceeded' }
  return [pscustomobject]@{
    Targets = $updated
    IceServers = @()
    Drops = @(Sort-Q2DzStrings $drops.ToArray())
  }
}

function Test-Q2DzRoomPayload {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject -or
      $Value.PSObject.Properties.Name -notcontains 'rooms') { return $false }
  if ($Value.rooms -isnot [array]) { return $false }
  $rows = @($Value.rooms)
  $supportedCount = 0
  $seenRooms = @()
  $seenAliases = @()
  foreach ($row in $rows) {
    if ($null -eq $row -or $null -eq $row.PSObject) { continue }
    if ($null -ne $row.createSettings -and @('q2','q2ded') -ccontains [string]$row.createSettings.prefix) {
      $supportedCount++
      if (-not (Test-Q2DzRoomRow $row)) { return $false }
      if ($seenRooms -ccontains [string]$row.room) { return $false }
      $seenRooms += [string]$row.room
      $alias = Get-Q2DzRoomAlias $row
      if ($seenAliases -ccontains $alias) { return $false }
      $seenAliases += $alias
    }
  }
  return $supportedCount -gt 0
}

function Test-Q2DzEmptyRoomCatalog {
  param($Value)
  if ($null -eq $Value -or $null -eq $Value.PSObject -or
      $Value.PSObject.Properties.Name -notcontains 'rooms' -or $Value.rooms -isnot [array]) {
    return $false
  }
  return @($Value.rooms).Count -eq 0
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
