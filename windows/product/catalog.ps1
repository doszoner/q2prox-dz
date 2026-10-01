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

$Q2DZ_LOG_MAX_BYTES = 5MB
$script:Q2DzSessionLog = $null

function Initialize-Q2DzSessionLog {
  param([Parameter(Mandatory = $true)][string]$StateDirectory)
  $log = [pscustomobject]@{ Directory = $null; Path = $null; Writer = $null; Failed = $false }
  try {
    $directory = Join-Path $StateDirectory 'logs'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $path = Join-Path $directory 'q2dz.log'
    $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    $stream = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, $share)
    $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true
    $log.Directory = $directory
    $log.Path = $path
    $log.Writer = $writer
  } catch {
    $log.Failed = $true
  }
  $script:Q2DzSessionLog = $log
}

function Close-Q2DzSessionLog {
  $log = $script:Q2DzSessionLog
  if ($null -ne $log -and $null -ne $log.Writer) {
    try { $log.Writer.Dispose() } catch {}
  }
  $script:Q2DzSessionLog = $null
}

function Write-Q2DzSessionLogLine {
  param([Parameter(Mandatory = $true)][string]$Text)
  $line = '{0} {1}' -f [DateTime]::UtcNow.ToString('o'), $Text
  [Console]::Error.WriteLine($line)
  $log = $script:Q2DzSessionLog
  if ($null -eq $log -or $log.Failed -or $null -eq $log.Writer) { return }
  try {
    if ($log.Writer.BaseStream.Position -ge $Q2DZ_LOG_MAX_BYTES) {
      $log.Writer.Dispose()
      $rotated = Join-Path $log.Directory 'q2dz.1.log'
      if (Test-Path -LiteralPath $rotated) { Remove-Item -LiteralPath $rotated -Force }
      [System.IO.File]::Move($log.Path, $rotated)
      $share = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
      $stream = New-Object System.IO.FileStream($log.Path, [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write, $share)
      $log.Writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
      $log.Writer.NewLine = "`n"
      $log.Writer.AutoFlush = $true
    }
    $log.Writer.WriteLine($line)
  } catch {
    try { $log.Writer.Dispose() } catch {}
    $log.Writer = $null
    $log.Failed = $true
  }
}

function Write-Q2DzCatalogEvent {
  param([Parameter(Mandatory = $true)][string]$Message)
  Write-Q2DzSessionLogLine ('LAUNCHER ' + $Message)
}

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
      $statusError = [Exception]::new('Dos.Zone endpoint returned a non-success status')
      $statusError.Data['q2dz.http'] = [int]$response.StatusCode
      throw $statusError
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

function Test-Q2DzIntegerKind {
  # Type-only check (no bounds): mirrors Linux's _is_int (isinstance(value,
  # int), true for a Python int of any magnitude), so a value of the right
  # kind but out of range is a 'range' fault, not folded into 'type'. Round-4
  # fix: [bigint] (ConvertFrom-Json's representation for an integer outside
  # signed 64-bit) is an integer kind too, not a 'type' fault — Test-Q2DzInteger
  # below still rejects it on range via the [decimal] cast/bounds check.
  param($Value)
  if ($Value -is [bool]) { return $false }
  if ($Value -is [bigint]) { return $true }
  $kind = [Convert]::GetTypeCode($Value)
  $integerKinds = @(
    [TypeCode]::Byte, [TypeCode]::SByte, [TypeCode]::Int16, [TypeCode]::UInt16,
    [TypeCode]::Int32, [TypeCode]::UInt32, [TypeCode]::Int64, [TypeCode]::UInt64
  )
  return $integerKinds -contains $kind
}

function Test-Q2DzInteger {
  param($Value, [long]$Minimum, [long]$Maximum)
  if (-not (Test-Q2DzIntegerKind $Value)) { return $false }
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

function Test-Q2DzRoomConfigChanged {
  param(
    [Parameter(Mandatory = $true)][string]$Previous,
    [Parameter(Mandatory = $true)][string]$Current
  )
  $previousValue = $Previous | ConvertFrom-Json -ErrorAction Stop
  $currentValue = $Current | ConvertFrom-Json -ErrorAction Stop
  foreach ($value in @($previousValue, $currentValue)) {
    if ($null -eq $value -or $value -is [array] -or $null -eq $value.PSObject) {
      throw 'room configuration must be a JSON object'
    }
    foreach ($property in @('iceServers','map')) {
      if ($value.PSObject.Properties.Name -contains $property) {
        [void]$value.PSObject.Properties.Remove($property)
      }
    }
  }
  $previousJson = ConvertTo-Json -InputObject (ConvertTo-Q2DzCanonicalJsonValue $previousValue) -Depth 8 -Compress
  $currentJson = ConvertTo-Json -InputObject (ConvertTo-Q2DzCanonicalJsonValue $currentValue) -Depth 8 -Compress
  return $previousJson -cne $currentJson
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
    $changed = Test-Q2DzRoomConfigChanged -Previous ([string]$target.Config) -Current $config
    $mapChanged = -not [string]::IsNullOrEmpty([string]$target.MapName) -and [string]$target.MapName -cne $map
    if ($mapChanged -and -not (& $MapAvailable $row)) {
      Write-Q2DzCatalogEvent ('CATALOG_MAP result=missing room={0} map={1}' -f $roomId,$map)
      [void]$drops.Add([string]$roomId)
      continue
    }
    $updated[$roomId] = New-Q2DzTarget $target.RoomId $config 0 $map
    if ($changed) { [void]$drops.Add([string]$roomId) }
  }

  foreach ($row in (Sort-Q2DzRoomRows $desired)) {
    $roomId = [string]$row.room
    if ($current.ContainsKey($roomId) -or $updated.ContainsKey($roomId)) { continue }
    if (-not (& $MapAvailable $row)) {
      Write-Q2DzCatalogEvent ('CATALOG_MAP result=missing room={0} map={1}' -f $roomId,[string]$row.createSettings.map)
      continue
    }
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

function Get-Q2DzRoomRowFault {
  param($Row)
  # Returns the first schema violation of a supported row as {Field, Reason},
  # in the same check order as Test-Q2DzRoomRow; $null when the row is valid.
  $fault = { param($field, $reason) return [pscustomobject]@{ Field = $field; Reason = $reason } }
  $settings = $Row.createSettings
  # A present-but-non-string prefix is a supported-row shape defect: a
  # [string] cast would coerce a one-element array into a supported kind.
  if ($settings.prefix -isnot [string]) { return (& $fault 'createSettings.prefix' 'type') }
  $kind = $settings.prefix
  foreach ($name in @('game','profile')) {
    if ($settings.PSObject.Properties.Name -contains $name) {
      if ($settings.$name -isnot [string]) {
        return (& $fault ('createSettings.' + $name) 'type')
      }
      if ($settings.$name -cne $Q2DZ_GAME) {
        return (& $fault ('createSettings.' + $name) 'range')
      }
    }
  }
  if ($settings.PSObject.Properties.Name -contains 'ordered') {
    if ($settings.ordered -isnot [bool]) { return (& $fault 'createSettings.ordered' 'type') }
    if ($settings.ordered -ne $Q2DZ_ORDERED) { return (& $fault 'createSettings.ordered' 'range') }
  }
  if ($Row.PSObject.Properties.Name -notcontains 'room') { return (& $fault 'room' 'missing') }
  if ($Row.room -isnot [string]) { return (& $fault 'room' 'type') }
  if (-not (Test-Q2DzSafeToken $Row.room)) { return (& $fault 'room' 'identity') }
  if ($kind -ceq 'q2') {
    # A required field whose value is JSON null counts as missing (the
    # producer had no value): the observed live defect is owner:null.
    if ($Row.PSObject.Properties.Name -notcontains 'owner' -or $null -eq $Row.owner) {
      return (& $fault 'owner' 'missing')
    }
    if ($Row.owner -is [bool] -or ($Row.owner -isnot [string] -and
        -not (Test-Q2DzIntegerKind $Row.owner))) {
      return (& $fault 'owner' 'type')
    }
    if ((Test-Q2DzIntegerKind $Row.owner) -and
        -not (Test-Q2DzInteger $Row.owner 0 $Q2DZ_MAX_JSON_SAFE_INTEGER)) {
      return (& $fault 'owner' 'range')
    }
    if ($Row.owner -is [string] -and -not (Test-Q2DzSafeToken $Row.owner 250)) {
      return (& $fault 'owner' 'format')
    }
    if ($Row.PSObject.Properties.Name -notcontains 'roomReady' -or $null -eq $Row.roomReady) {
      return (& $fault 'roomReady' 'missing')
    }
    if ($Row.roomReady -isnot [bool]) { return (& $fault 'roomReady' 'type') }
    if ($Row.PSObject.Properties.Name -notcontains 'rtcServer' -or $null -eq $Row.rtcServer) {
      return (& $fault 'rtcServer' 'missing')
    }
    if ($Row.rtcServer -isnot [string]) { return (& $fault 'rtcServer' 'type') }
    if (-not (Test-Q2DzHostname $Row.rtcServer)) { return (& $fault 'rtcServer' 'format') }
  } elseif (-not ([string]$Row.room).StartsWith('room.q2ded.', [StringComparison]::Ordinal) -or
            ([string]$Row.room).Length -le ('room.q2ded.').Length) {
    return (& $fault 'room' 'format')
  }
  if ($settings.PSObject.Properties.Name -notcontains 'name' -or $null -eq $settings.name) {
    return (& $fault 'createSettings.name' 'missing')
  }
  if (-not (Test-Q2DzText $settings.name)) { return (& $fault 'createSettings.name' 'format') }
  if ($settings.PSObject.Properties.Name -notcontains 'map' -or $null -eq $settings.map) {
    return (& $fault 'createSettings.map' 'missing')
  }
  if ($settings.map -isnot [string]) { return (& $fault 'createSettings.map' 'type') }
  if ($settings.map.Length -lt 1 -or $settings.map.Length -gt $Q2DZ_MAX_MAP -or
      $settings.map -notmatch '^[A-Za-z0-9._-]+\z') {
    return (& $fault 'createSettings.map' 'format')
  }
  if ($settings.PSObject.Properties.Name -notcontains 'maxPlayers' -or $null -eq $settings.maxPlayers) {
    return (& $fault 'createSettings.maxPlayers' 'missing')
  }
  if (-not (Test-Q2DzIntegerKind $settings.maxPlayers)) {
    return (& $fault 'createSettings.maxPlayers' 'type')
  }
  if (-not (Test-Q2DzInteger $settings.maxPlayers 1 $Q2DZ_MAX_PLAYERS)) {
    return (& $fault 'createSettings.maxPlayers' 'range')
  }
  if ($Row.PSObject.Properties.Name -notcontains 'players' -or $null -eq $Row.players) {
    return (& $fault 'players' 'missing')
  }
  if (-not (Test-Q2DzIntegerKind $Row.players)) {
    return (& $fault 'players' 'type')
  }
  if (-not (Test-Q2DzInteger $Row.players 0 ([long]$settings.maxPlayers))) {
    return (& $fault 'players' 'range')
  }
  return $null
}

function Get-Q2DzCatalogRowRejections {
  param($Catalog)
  # Classifies every rejected supported row of a room/get payload. Identities
  # are resolved first; every member of a duplicate group gets reason=duplicate.
  $rejections = [System.Collections.Generic.List[object]]::new()
  if ($null -eq $Catalog -or $null -eq $Catalog.PSObject -or
      $Catalog.PSObject.Properties.Name -notcontains 'rooms' -or $Catalog.rooms -isnot [array]) {
    return @($rejections)
  }
  $identifiable = [System.Collections.Generic.List[object]]::new()
  $index = -1
  foreach ($row in @($Catalog.rooms)) {
    $index++
    if (-not (Test-Q2DzCatalogRowClassifiable $row)) { continue }
    if ($row.PSObject.Properties.Name -notcontains 'room' -or $row.room -isnot [string] -or
        -not (Test-Q2DzSafeToken $row.room)) {
      [void]$rejections.Add([pscustomobject]@{ Index = $index; Room = $null; Field = 'room'; Reason = 'identity' })
      continue
    }
    [void]$identifiable.Add([pscustomobject]@{ Index = $index; Row = $row })
    $fault = Get-Q2DzRoomRowFault $row
    if ($null -ne $fault) {
      [void]$rejections.Add([pscustomobject]@{
        Index = $index; Room = [string]$row.room; Field = $fault.Field; Reason = $fault.Reason })
    }
  }
  # Duplicate groups are found among all identifiable rows, including ones
  # already rejected on a field fault (D5): the alias is derived safely so
  # a faulty row cannot crash or produce a phantom 'q2.' alias.
  $firstRoom = New-Q2DzTargetMap
  $firstAlias = New-Q2DzTargetMap
  foreach ($item in $identifiable) {
    $row = $item.Row
    $roomId = [string]$row.room
    if ($firstRoom.ContainsKey($roomId)) {
      foreach ($member in @($firstRoom[$roomId], [pscustomobject]@{ Index = $item.Index; Room = $roomId })) {
        [void]$rejections.Add([pscustomobject]@{ Index = [int]$member.Index; Room = [string]$member.Room; Field = 'room'; Reason = 'duplicate' })
      }
    } else {
      $firstRoom[$roomId] = [pscustomobject]@{ Index = $item.Index; Room = $roomId }
    }
    $alias = Get-Q2DzClassifiedRoomAlias $row
    if ($null -ne $alias) {
      if ($firstAlias.ContainsKey($alias)) {
        foreach ($member in @($firstAlias[$alias], [pscustomobject]@{ Index = $item.Index; Room = $roomId })) {
          [void]$rejections.Add([pscustomobject]@{ Index = [int]$member.Index; Room = [string]$member.Room; Field = 'alias'; Reason = 'duplicate' })
        }
      } else {
        $firstAlias[$alias] = [pscustomobject]@{ Index = $item.Index; Room = $roomId }
      }
    }
  }
  $merged = @{}
  foreach ($rejection in $rejections) {
    if ($null -eq $merged[$rejection.Index] -or $rejection.Reason -eq 'duplicate') {
      $merged[$rejection.Index] = $rejection
    }
  }
  return @($merged.Values | Sort-Object Index)
}

function Test-Q2DzCatalogRowClassifiable {
  param($Row)
  # Rows the tolerant gate inspects: supported rows plus rows whose
  # createSettings.prefix is present but not a string — a supported-row
  # shape defect the classifier reports with reason=type. A foreign game
  # carries a string prefix and stays skipped.
  if ($null -eq $Row -or $null -eq $Row.PSObject -or
      $Row.PSObject.Properties.Name -notcontains 'createSettings' -or
      $null -eq $Row.createSettings -or $null -eq $Row.createSettings.PSObject -or
      $Row.createSettings.PSObject.Properties.Name -notcontains 'prefix') { return $false }
  $prefix = $Row.createSettings.prefix
  if ($prefix -isnot [string]) { return $true }
  return @('q2','q2ded') -ccontains $prefix
}

function Get-Q2DzClassifiedRoomAlias {
  param($Row)
  # Alias derivation for classification on rows that may be invalid; $null
  # when no alias can be derived (keeps identity conflicts out of reach of
  # a missing/typed owner or a non-string prefix).
  if ($Row.createSettings.prefix -isnot [string]) { return $null }
  if ((Get-Q2DzRoomKind $Row) -ceq 'q2ded') { return [string]$Row.room }
  if ($Row.PSObject.Properties.Name -notcontains 'owner') { return $null }
  $owner = $Row.owner
  if ($null -eq $owner -or $owner -is [bool]) { return $null }
  if ($owner -isnot [string] -and -not (Test-Q2DzIntegerKind $owner)) { return $null }
  return ('q2.{0}' -f [string]$owner)
}

function Write-Q2DzCatalogRowRejections {
  param([AllowEmptyCollection()]$Rejections)
  foreach ($rejection in @($Rejections)) {
    $roomField = ''
    if ($null -ne $rejection.Room) { $roomField = ' room=' + $rejection.Room }
    Write-Q2DzCatalogEvent ('CATALOG_ROW result=rejected kind=room{0} index={1} field={2} reason={3}' -f `
      $roomField,$rejection.Index,$rejection.Field,$rejection.Reason)
  }
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
