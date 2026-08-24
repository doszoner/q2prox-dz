Set-StrictMode -Version 2.0

function Normalize-GatePPath {
    param([AllowNull()][string]$Path)

    if ($null -eq $Path) { return '' }
    $normalized = $Path.Replace('\', '/')
    while ($normalized.StartsWith('./', [StringComparison]::Ordinal)) {
        $normalized = $normalized.Substring(2)
    }
    return $normalized
}

function Get-GatePConfigLines {
    param([AllowNull()][string]$Text)

    if ($null -eq $Text) { return @() }
    return @($Text -split "`r?`n")
}

function Get-GatePAddedLines {
    param(
        [AllowNull()][string]$BeforeText,
        [AllowNull()][string]$AfterText
    )

    $remaining = @{}
    foreach ($line in @(Get-GatePConfigLines $BeforeText)) {
        $key = [string]$line
        if ($remaining.ContainsKey($key)) { $remaining[$key]++ }
        else { $remaining[$key] = 1 }
    }

    $added = @()
    foreach ($line in @(Get-GatePConfigLines $AfterText)) {
        $key = [string]$line
        if ($remaining.ContainsKey($key) -and $remaining[$key] -gt 0) {
            $remaining[$key]--
        } else {
            $added += $line
        }
    }
    return $added
}

function Get-GatePRelevantSettings {
    param([AllowNull()][string]$Text)

    $settings = @{}
    $assignment = '^(?:\s*)(?:(?:set|seta)\s+)?(?:"(?<qkey>[^"]+)"|(?<bkey>[^\s]+))\s+(?:"(?<qval>.*)"|(?<bval>.+?))(?:\s*)$'
    foreach ($line in @(Get-GatePConfigLines $Text)) {
        $match = [regex]::Match([string]$line, $assignment)
        if (-not $match.Success) { continue }
        $key = if ($match.Groups['qkey'].Success) { $match.Groups['qkey'].Value } else { $match.Groups['bkey'].Value }
        $key = $key.ToLowerInvariant()
        if ($key -match '^adr(?:[0-9]|1[0-5])$' -or
            $key -match 'serverbrowser.*source' -or
            $key -match '(?:custom|default).*source') {
            $value = if ($match.Groups['qval'].Success) { $match.Groups['qval'].Value } else { $match.Groups['bval'].Value.Trim() }
            $settings[$key] = [string]$value
        }
    }
    return $settings
}

function Get-GatePRelevantSettingReason {
    param([string]$Key)

    if ($Key -match '^adr(?:[0-9]|1[0-5])$') { return 'favorites-change' }
    return 'custom-source-change'
}

function Get-GatePSensitiveContentPattern {
    $fields = '(?:credential|turn|turns|stun|ice|iceservers|ice_servers|username|token|access_token|accesstoken|player_token|playertoken|auth_token|authtoken|password|authorization|cookie|sdp|candidate|secret)'
    $harmless = '(?:no|yes|true|false|0|1|none|disabled|enabled)'
    $bareAssignment = '(?:^|[\s,{"''])' + $fields + '\s*[:=]\s*(?!(?:' + $harmless + ')(?:$|[\s,}\]]))'
    return ('(?i)(?:(?:^|[\r\n])\s*(?:(?:set|seta)\s+)?["'']?' + $fields + '["'']?\s+(?:["'']|=|:)|["'']' + $fields + '["'']\s*[:=]|' + $bareAssignment + '|(?:turns?|stun)\s*:)')
}

function Test-GatePSensitiveText {
    param([AllowNull()][string]$Text)

    if ($null -eq $Text) { return $false }
    return ($Text -match (Get-GatePSensitiveContentPattern))
}

function Get-GatePSanitizedOutput {
    param([AllowNull()][object[]]$Lines)

    $safeLines = @()
    $sensitiveCount = 0
    foreach ($line in @($Lines)) {
        $text = [string]$line
        if (Test-GatePSensitiveText $text) {
            $sensitiveCount++
            $safeLines += '[redacted sensitive coordinator output]'
        } else {
            $safeLines += $text
        }
    }
    return [pscustomobject]@{
        Lines = $safeLines
        SensitiveCount = $sensitiveCount
    }
}

function Test-GatePAllowedAutosavePath {
    param([string]$Path)

    $normalized = Normalize-GatePPath $Path
    if ($normalized -ieq 'q2config.cfg' -or $normalized -match '(?i)(^|/)q2config\.cfg$') {
        return $true
    }
    return ($normalized -match '(?i)(^|/)q2pro-x/q2pro-x\.cfg$')
}

function Get-GatePAddedContentReason {
    param([AllowNull()][string[]]$Lines)

    $text = [string]::Join("`n", @($Lines))
    if (Test-GatePSensitiveText $text) {
        return 'credential-shaped-content'
    }
    if ($text -match '(?i)(?:q2dz|dos\.?zone|webrtc|gateway|native-quake2-client)') {
        return 'addon-owned-content'
    }
    return $null
}

function New-GatePPolicyResult {
    param(
        [string]$Path,
        [string]$Kind,
        [bool]$Allowed,
        [string]$Reason
    )

    return [pscustomobject]@{
        Path = Normalize-GatePPath $Path
        Kind = $Kind
        Allowed = $Allowed
        Reason = $Reason
    }
}

function Invoke-GatePConfigPolicy {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Diffs)

    $results = @()
    foreach ($diff in @($Diffs)) {
        $path = Normalize-GatePPath ([string]$diff.Path)
        $kind = [string]$diff.Kind
        if ([string]::IsNullOrWhiteSpace($path) -or $path.StartsWith('/', [StringComparison]::Ordinal) -or
            $path -match '(?i)(^|/)\.\.?(/|$)' -or $path -match '^[A-Za-z]:/') {
            $results += New-GatePPolicyResult $path $kind $false 'unsafe-path'
            continue
        }
        if ($kind -ieq 'Deleted') {
            $results += New-GatePPolicyResult $path $kind $false 'deletion'
            continue
        }

        $isCfg = [IO.Path]::GetExtension($path) -ieq '.cfg'
        if (-not $isCfg) {
            if ($path -match '(?i)(?:q2dz|dos\.?zone|webrtc|gateway|native-quake2-client)') {
                $reason = 'addon-residue'
            } elseif ($path -match '(?i)(?:^|/)doszone(?:-|/|$)') {
                $reason = 'addon-residue'
            } elseif ($path -match '(?i)\.(?:exe|dll|sys|wasm|pak|pk3|so|dylib)$') {
                $reason = 'game-content-change'
            } elseif (($kind -ieq 'Added' -or $kind -ieq 'Changed') -and
                $path -match '(?i)(^|/)logs/console\.log$') {
                $reason = 'q2-owned-log'
            } elseif (($kind -ieq 'Added' -or $kind -ieq 'Changed') -and
                $path -match '(?i)(^|/)q2pro-x/shcache/(?:n_)?[0-9a-f]{32}\.bin$') {
                $reason = 'q2-owned-shcache'
            } elseif (($kind -ieq 'Added' -or $kind -ieq 'Changed') -and
                $path -match '(?i)(^|/)\.conhistory$') {
                $reason = 'q2-owned-console-history'
            } elseif (($kind -ieq 'Added' -or $kind -ieq 'Changed') -and
                $path -match '(?i)(^|/)q2pro-x/backups/q2pro-x(?:\.local)?\.cfg\.bak(?:\.[0-9]+)?$') {
                $reason = 'q2-owned-config-backup'
            } elseif (($kind -ieq 'Added' -or $kind -ieq 'Changed') -and
                $path -match '(?i)(^|/)q2pro-x/mods_cache\.dat$') {
                $reason = 'q2-owned-mods-cache'
            } else {
                $reason = 'unexpected-file-change'
            }
            $allowed = ($reason -in @('q2-owned-log', 'q2-owned-shcache', 'q2-owned-console-history', 'q2-owned-config-backup', 'q2-owned-mods-cache'))
            $results += New-GatePPolicyResult $path $kind $allowed $reason
            continue
        }

        if (-not (Test-GatePAllowedAutosavePath $path)) {
            $results += New-GatePPolicyResult $path $kind $false 'unexpected-cfg-change'
            continue
        }

        $before = Get-GatePRelevantSettings ([string]$diff.BeforeText)
        $after = Get-GatePRelevantSettings ([string]$diff.AfterText)
        $keys = @($before.Keys + $after.Keys | Sort-Object -Unique)
        $sensitiveReason = $null
        foreach ($key in $keys) {
            $beforeValue = if ($before.ContainsKey($key)) { [string]$before[$key] } else { $null }
            $afterValue = if ($after.ContainsKey($key)) { [string]$after[$key] } else { $null }
            if ($beforeValue -cne $afterValue) {
                $sensitiveReason = Get-GatePRelevantSettingReason $key
                break
            }
        }
        if ($null -ne $sensitiveReason) {
            $results += New-GatePPolicyResult $path $kind $false $sensitiveReason
            continue
        }

        $contentReason = Get-GatePAddedContentReason @(Get-GatePAddedLines ([string]$diff.BeforeText) ([string]$diff.AfterText))
        if ($null -ne $contentReason) {
            $results += New-GatePPolicyResult $path $kind $false $contentReason
            continue
        }

        $results += New-GatePPolicyResult $path $kind $true 'q2-owned-autosave'
    }
    return $results
}
