[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
  Write-Host 'launcher stdin contract: SKIP (Windows-only executable test)'
  exit 0
}

function Assert([bool]$Condition, [string]$Message) {
  if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function New-TestExecutable([string]$Path, [bool]$Capture) {
  $typeName = if ($Capture) { 'Q2DzTestCaptureStub' } else { 'Q2DzTestNoopStub' }
  $body = if ($Capture) {
    @'
        var capture = Environment.GetEnvironmentVariable("Q2DZ_TEST_CAPTURE");
        if (String.IsNullOrEmpty(capture)) return 20;
        using (var input = Console.OpenStandardInput())
        using (var output = File.Create(capture))
        {
            input.CopyTo(output);
        }
        return 0;
'@
  } else {
    '        return 0;'
  }
  $source = @"
using System;
using System.IO;

public static class $typeName
{
    public static int Main()
    {
$body
    }
}
"@
  Add-Type -TypeDefinition $source -OutputAssembly $Path -OutputType ConsoleApplication
}

$product = (Resolve-Path "$PSScriptRoot/..").Path
$work = Join-Path ([IO.Path]::GetTempPath()) ("q2dz-launcher-stdin-" + [Guid]::NewGuid().ToString('N'))
$oldLocalAppData = $env:LOCALAPPDATA
$oldCapture = $env:Q2DZ_TEST_CAPTURE
New-Item -ItemType Directory -Force $work | Out-Null
try {
  $app = Join-Path $work 'app'
  $state = Join-Path $work 'localappdata/Q2PRO-X Dos.Zone/state'
  $gatewayDir = Join-Path $app 'gateway'
  New-Item -ItemType Directory -Force $app,$gatewayDir,$state | Out-Null

  Copy-Item -LiteralPath (Join-Path $product 'q2dz-launch.ps1') -Destination (Join-Path $app 'q2dz-launch.ps1')
  $catalog = @'
$Q2DZ_ROOM_ENDPOINT = 'test://rooms'
$Q2DZ_LOOKUP_ENDPOINT = 'test://lookup'
$Q2DZ_RTC_ENDPOINT = 'test://rtc'
$Q2DZ_GAME = '3zb2'
$Q2DZ_ORDERED = $false
function Get-Q2DzJson { param([string]$Endpoint)
  if ($Endpoint -ceq $Q2DZ_ROOM_ENDPOINT) { return [pscustomobject]@{ rooms = @(
    [pscustomobject]@{ room = 'room.q2.a'; owner = '1'; roomReady = $true; players = 0; rtcServer = 'lvc.example.invalid'; createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'A'; map = 'q2dm1'; maxPlayers = 16 } },
    [pscustomobject]@{ room = 'room.q2.b'; owner = '2'; roomReady = $true; players = 0; rtcServer = 'lvc.example.invalid'; createSettings = [pscustomobject]@{ prefix = 'q2'; name = 'B'; map = 'q2dm2'; maxPlayers = 16 } }
  ) } }
  if ($Endpoint -ceq $Q2DZ_LOOKUP_ENDPOINT) { return [pscustomobject]@{ aliases = @('q2.1','q2.2') } }
  if ($Endpoint -ceq $Q2DZ_RTC_ENDPOINT) { return @([pscustomobject]@{ domain = 'lvc.example.invalid'; iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') }) }) }
  throw "unexpected endpoint: $Endpoint"
}
function Get-Q2DzRegisteredRooms { param($Catalog, $LookupPayload) return @($Catalog.rooms) }
function Get-Q2DzSelectedTransport { param($Room) return [pscustomobject]@{ Alias = ('q2.' + $Room.owner); Rtc = [pscustomobject]@{ iceservers = @([pscustomobject]@{ urls = @('stun:example.invalid') }) } } }
function Test-Q2DzEndpoint { param([string]$Uri) return $true }
function Test-Q2DzRoomRow { param($Value) return $true }
'@
  Set-Content -LiteralPath (Join-Path $app 'catalog.ps1') -Value $catalog -Encoding UTF8

  $q2 = Join-Path $work 'Q2PRO-X.exe'
  [IO.File]::WriteAllBytes($q2, [byte[]](0x4d,0x5a))
  Set-Content -LiteralPath (Join-Path $state 'q2-path.txt') -Value $q2 -Encoding UTF8

  New-TestExecutable (Join-Path $app 'q2dz-session.exe') $true
  New-TestExecutable (Join-Path $app 'q2dz-content-check.exe') $false
  [IO.File]::WriteAllText((Join-Path $gatewayDir 'q2-doszone-webview2-gateway.exe'), 'test gateway')

  $capture = Join-Path $work 'coordinator-stdin.bin'
  $env:LOCALAPPDATA = (Join-Path $work 'localappdata')
  $env:Q2DZ_TEST_CAPTURE = $capture
  $powershell = (Get-Command powershell.exe -ErrorAction SilentlyContinue).Source
  if (-not $powershell) { $powershell = (Get-Command pwsh.exe -ErrorAction Stop).Source }
  & $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $app 'q2dz-launch.ps1') *> (Join-Path $work 'launcher.log')
  Assert ($LASTEXITCODE -eq 0) ('launcher returned {0}; log: {1}' -f $LASTEXITCODE, [IO.File]::ReadAllText((Join-Path $work 'launcher.log')))
  Assert (Test-Path -LiteralPath $capture -PathType Leaf) 'coordinator stub did not capture stdin'

  $bytes = [IO.File]::ReadAllBytes($capture)
  $utf8 = New-Object System.Text.UTF8Encoding($false, $true)
  $text = $utf8.GetString($bytes)
  $roundTrip = $utf8.GetBytes($text)
  Assert ([System.Collections.StructuralComparisons]::StructuralEqualityComparer.Equals($bytes, $roundTrip)) 'stdin is not exact UTF-8'
  Assert (-not $text.StartsWith([char]0xFEFF)) 'stdin contains a UTF-8 BOM'
  Assert (-not $text.EndsWith([Environment]::NewLine)) 'stdin has an extra trailing newline'
  $lines = $text.Split([Environment]::NewLine, [StringSplitOptions]::None)
  Assert ($lines.Count -eq 2) "expected exactly two JSON lines, got $($lines.Count)"
  Assert (($lines | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) 'stdin contains an empty JSON line'
  $rooms = @($lines | ForEach-Object { $_ | ConvertFrom-Json })
  Assert ($rooms.Count -eq 2) 'coordinator received an unexpected number of room objects'
  Assert ($rooms[0].roomId -eq 'room.q2.a' -and $rooms[1].roomId -eq 'room.q2.b') 'room order or identifiers changed'
  Assert ($rooms[0].alias -eq 'q2.1' -and $rooms[1].alias -eq 'q2.2') 'room aliases were not preserved'
  Write-Host 'launcher stdin contract: PASS (exact UTF-8, two nonempty JSON lines)'
}
finally {
  if ($null -eq $oldLocalAppData) { Remove-Item Env:LOCALAPPDATA -ErrorAction SilentlyContinue } else { $env:LOCALAPPDATA = $oldLocalAppData }
  if ($null -eq $oldCapture) { Remove-Item Env:Q2DZ_TEST_CAPTURE -ErrorAction SilentlyContinue } else { $env:Q2DZ_TEST_CAPTURE = $oldCapture }
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
