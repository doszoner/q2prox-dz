[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$policy = Join-Path $PSScriptRoot 'gate-p-config-policy.ps1'
if (-not (Test-Path -LiteralPath $policy -PathType Leaf)) {
    throw 'RED: gate-p-config-policy.ps1 is not implemented yet; ordinary q2config autosave cannot be classified'
}

. $policy

$tokens = $null
$parseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'gate-p.ps1'), [ref]$tokens, [ref]$parseErrors) | Out-Null
if (@($parseErrors).Count -ne 0) { throw 'gate-p.ps1 has PowerShell syntax errors' }

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

$probeRoot = Join-Path ([IO.Path]::GetTempPath()) ('q2dz-gate-p-pipeline-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $probeRoot | Out-Null
try {
    $gate = Join-Path $PSScriptRoot 'gate-p.ps1'
    $probeErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $probe = @('   ' | & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $gate `
            -Coordinator (Join-Path $probeRoot 'missing-coordinator.exe') `
            -Gateway (Join-Path $probeRoot 'missing-gateway.exe') `
            -Q2 (Join-Path $probeRoot 'missing-q2.exe') `
            -EvidenceDir (Join-Path $probeRoot 'evidence') 2>&1)
    } finally {
        $ErrorActionPreference = $probeErrorAction
    }
    $probeText = [string]::Join("`n", @($probe))
    Assert ($probeText -match 'Fresh room JSON was not supplied on stdin') 'gate-p must reject whitespace stdin before touching runtime paths'
} finally {
    Remove-Item -LiteralPath $probeRoot -Recurse -Force -ErrorAction SilentlyContinue
}

function New-Diff {
    param(
        [string]$Path,
        [string]$Kind = 'Changed',
        [AllowNull()][string]$BeforeText,
        [AllowNull()][string]$AfterText
    )
    return [pscustomobject]@{
        Path = $Path
        Kind = $Kind
        BeforeText = $BeforeText
        AfterText = $AfterText
    }
}

function One-Result {
    param([object]$Diff)
    $rows = @(Invoke-GatePConfigPolicy -Diffs @($Diff))
    Assert ($rows.Count -eq 1) "expected one policy result for $($Diff.Path)"
    return $rows[0]
}

function Assert-Allowed {
    param([object]$Diff, [string]$Reason)
    $row = One-Result $Diff
    Assert ([bool]$row.Allowed) "expected allowed: $($Diff.Path)"
    Assert ($row.Reason -eq $Reason) "expected reason $Reason for $($Diff.Path), got $($row.Reason)"
}

function Assert-Forbidden {
    param([object]$Diff, [string]$Reason)
    $row = One-Result $Diff
    Assert (-not [bool]$row.Allowed) "expected forbidden: $($Diff.Path)"
    Assert ($row.Reason -eq $Reason) "expected reason $Reason for $($Diff.Path), got $($row.Reason)"
    Assert (-not ($row.PSObject.Properties.Name -contains 'BeforeText')) 'policy result must not expose file content'
    Assert (-not ($row.PSObject.Properties.Name -contains 'AfterText')) 'policy result must not expose file content'
    return $row
}

# RED evidence: the old Gate P compared cfg hashes and rejected every changed
# q2config.cfg, including this ordinary Q2-owned autosave.
$ordinary = New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta cl_maxfps "125"' 'seta cl_maxfps "144"'
$legacyHasher = [Security.Cryptography.SHA256]::Create()
try {
    $legacyBeforeHash = ([BitConverter]::ToString($legacyHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes([string]$ordinary.BeforeText)))).Replace('-', '').ToLowerInvariant()
    $legacyAfterHash = ([BitConverter]::ToString($legacyHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes([string]$ordinary.AfterText)))).Replace('-', '').ToLowerInvariant()
} finally {
    $legacyHasher.Dispose()
}
$legacyRejected = ($legacyBeforeHash -ne $legacyAfterHash)
Assert $legacyRejected 'baseline fixture must demonstrate the old blanket rejection'
Assert-Allowed $ordinary 'q2-owned-autosave'
Write-Host 'RED baseline: blanket cfg hash policy rejects ordinary q2config autosave'

$commonBefore = @'
seta cl_maxfps "125"
seta adr0 "q2.PlayGround.ru:27910"
seta adr15 ""
set cl_serverbrowser_custom_source ""
set cl_serverbrowser_default_source "1"
'@
$commonAfter = $commonBefore.Replace('"125"', '"144"')
Assert-Allowed (New-Diff 'baseq2/q2config.cfg' 'Changed' $commonBefore $commonAfter) 'q2-owned-autosave'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/q2pro-x.cfg' 'Changed' 'set cl_maxfps "125"' 'set cl_maxfps "144"') 'q2-owned-autosave'
Assert-Allowed (New-Diff 'baseq2/q2config.cfg' 'Added' '' 'seta cl_maxfps "144"') 'q2-owned-autosave'
Assert-Allowed (New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta name "Old"' 'seta name "Ice"') 'q2-owned-autosave'

Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Changed' $commonBefore ($commonBefore.Replace('q2.PlayGround.ru:27910', '127.0.0.1:27999'))) 'favorites-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Changed' $commonBefore ($commonBefore.Replace('set cl_serverbrowser_custom_source ""', 'set cl_serverbrowser_custom_source "http://127.0.0.1:1234"'))) 'custom-source-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/q2pro-x.cfg' 'Changed' 'set cl_serverbrowser_default_source "1"' 'set cl_serverbrowser_default_source "custom"') 'custom-source-change' | Out-Null

$credentialAfter = @'
seta name "player"
seta password "turn:secret-value"
'@
Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta name "player"' $credentialAfter) 'credential-shaped-content' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta name "player"' 'seta name "dos.zone-player"') 'addon-owned-content' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/autoexec.cfg' 'Changed' 'echo old' 'echo new') 'unexpected-cfg-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/Q2PRO-X.exe' 'Changed' '' '') 'game-content-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/q2pro-x.dll' 'Changed' '' '') 'game-content-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/pak99.pak' 'Added' '' '') 'game-content-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2dz-session.json' 'Added' '' '{"room":"opaque"}') 'addon-residue' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Deleted' $commonBefore '') 'deletion' | Out-Null
Assert-Allowed (New-Diff 'baseq2/q2pro-x/logs/console.log' 'Added' '' 'new') 'q2-owned-log'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/logs/console.log' 'Changed' 'old' 'new') 'q2-owned-log'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/shcache/01256ff7a4f73997b439772903f47416.bin' 'Added' '' 'binary') 'q2-owned-shcache'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/shcache/01256ff7a4f73997b439772903f47416.bin' 'Changed' 'old' 'new') 'q2-owned-shcache'
Assert-Allowed (New-Diff 'baseq2/.conhistory' 'Added' '' 'history') 'q2-owned-console-history'
Assert-Allowed (New-Diff '3zb2/.conhistory' 'Changed' 'old' 'new') 'q2-owned-console-history'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/backups/q2pro-x.cfg.bak' 'Added' '' 'backup') 'q2-owned-config-backup'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/backups/q2pro-x.cfg.bak.3' 'Changed' 'old' 'new') 'q2-owned-config-backup'
Assert-Allowed (New-Diff '3zb2/q2pro-x/backups/q2pro-x.local.cfg.bak.12' 'Added' '' 'backup') 'q2-owned-config-backup'
Assert-Allowed (New-Diff 'baseq2/q2pro-x/mods_cache.dat' 'Changed' 'old' 'new') 'q2-owned-mods-cache'
Assert-Forbidden (New-Diff 'baseq2/.conhistory.bak' 'Added' '' 'history') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/console.conhistory' 'Added' '' 'history') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/backup/q2pro-x.cfg.bak' 'Added' '' 'backup') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/backups/other.cfg.bak' 'Added' '' 'backup') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/backups/q2dz.cfg.bak' 'Added' '' 'addon backup') 'addon-residue' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/mods_cache.tmp' 'Added' '' 'cache') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/mods-cache.dat' 'Added' '' 'cache') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/addon/q2pro-x.cfg.bak' 'Added' '' 'backup') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/qconsole.log' 'Changed' 'old' 'new') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2config.log' 'Added' '' 'new') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/some-addon/q2config.log' 'Changed' 'old' 'new') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/helper.txt' 'Added' '' 'new') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/helper.txt' 'Changed' 'old' 'new') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/addon.dat' 'Added' '' 'new') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/doszone-helper.log' 'Added' '' 'new') 'addon-residue' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/shcache/bad-name.bin' 'Added' '' 'binary') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/shcache/addon.bin' 'Added' '' 'binary') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/q2pro-x/shcache/q2dz_payload.bin' 'Added' '' 'binary') 'addon-residue' | Out-Null
Assert-Forbidden (New-Diff 'baseq2/cache.bin' 'Added' '' 'binary') 'unexpected-file-change' | Out-Null
Assert-Forbidden (New-Diff '../outside/q2config.cfg' 'Changed' '' '') 'unsafe-path' | Out-Null
Assert-Forbidden (New-Diff '/outside/q2config.cfg' 'Changed' '' '') 'unsafe-path' | Out-Null
Assert-Forbidden (New-Diff '//server/q2config.cfg' 'Changed' '' '') 'unsafe-path' | Out-Null
Assert-Forbidden (New-Diff 'C:/outside/q2config.cfg' 'Changed' '' '') 'unsafe-path' | Out-Null

foreach ($value in @('credential=fixture', 'turn:fixture', 'turns:fixture', 'stun:fixture', 'ice=fixture', 'iceServers=fixture', 'ice_servers=fixture', 'username=fixture', 'token=fixture', 'access_token=fixture', 'accessToken=fixture', 'player_token=fixture', 'playerToken=fixture', 'authToken=fixture', 'password=fixture', 'authorization=fixture', 'cookie=fixture', 'sdp=fixture', 'candidate=fixture', 'secret=fixture')) {
    Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta name "player"' ("seta q2_fixture `"$value`"")) 'credential-shaped-content' | Out-Null
}
Assert-Forbidden (New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta name "player"' 'seta q2_fixture "doszone=fixture"') 'addon-owned-content' | Out-Null
Assert-Allowed (New-Diff 'baseq2/q2config.cfg' 'Changed' 'seta name "player"' 'Persistent streaming: candidate=no ...') 'q2-owned-autosave'
Assert (-not (Test-GatePSensitiveText 'Persistent streaming: candidate=no ...')) 'ordinary scalar status output must not be treated as sensitive'
Assert (Test-GatePSensitiveText 'Persistent streaming: candidate=realish-fixture') 'nontrivial bare candidate value must remain sensitive'
Assert (Test-GatePSensitiveText '{"candidate":"no"}') 'JSON candidate field must remain sensitive'
Assert (Test-GatePSensitiveText '{"token":"no"}') 'JSON token field must remain sensitive'

$sanitized = Get-GatePSanitizedOutput @('ordinary output', 'token=fixture', 'line after')
Assert ([int]$sanitized.SensitiveCount -eq 1) 'sensitive coordinator output must set a failure count'
Assert (($sanitized.Lines -join "`n") -notmatch 'token=fixture') 'sanitized output must not contain the sensitive value'
Assert (($sanitized.Lines -join "`n") -match 'redacted') 'sanitized output must retain only a redaction marker'

$empty = @(Invoke-GatePConfigPolicy -Diffs @())
Assert ($empty.Count -eq 0) 'empty diff must produce no policy rows'

$row = One-Result $ordinary
Assert (($row.PSObject.Properties.Name | Sort-Object) -join ',' -eq 'Allowed,Kind,Path,Reason') 'policy result must contain only safe summary fields'
Assert ((($row | Out-String) -notmatch 'secret|turn:|credential')) 'policy output must not contain sensitive content'

Write-Host 'gate-p config policy tests: PASS'
