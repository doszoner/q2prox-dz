[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Coordinator,
  [Parameter(Mandatory)][string]$Gateway,
  [Parameter(Mandatory)][string]$Q2,
  [Parameter(ValueFromPipeline,Mandatory)][string]$RoomConfigJson,
  [string]$EvidenceDir = "$PWD/gate-p-evidence"
)

begin {
$ErrorActionPreference = 'Stop'
$policyScript = Join-Path $PSScriptRoot 'gate-p-config-policy.ps1'
if (-not (Test-Path -LiteralPath $policyScript -PathType Leaf)) {
  throw 'Gate P config policy is missing'
}
. $policyScript

function Test-GatePPathUnderRoot {
  param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Path
  )

  $rootPath = [IO.Path]::GetFullPath($Root) -replace '[\\/]+$',''
  $fullPath = [IO.Path]::GetFullPath($Path)
  return ($fullPath.Equals($rootPath, [StringComparison]::OrdinalIgnoreCase) -or
    $fullPath.StartsWith($rootPath + '\', [StringComparison]::OrdinalIgnoreCase))
}

function Get-GatePSafeEntries {
  param([Parameter(Mandatory)][string]$Root)

  if (-not (Test-Path -LiteralPath $Root)) { return @() }
  $rootItem = Get-Item -LiteralPath $Root -Force -ErrorAction Stop
  $rootPath = [IO.Path]::GetFullPath($rootItem.FullName) -replace '[\\/]+$',''
  if (-not $rootItem.PSIsContainer) {
    return @([pscustomobject]@{
      FullName = $rootPath
      IsDirectory = $false
      IsReparse = [bool]($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)
    })
  }
  if ([bool]($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    return @([pscustomobject]@{
      FullName = $rootPath
      IsDirectory = $true
      IsReparse = $true
    })
  }
  $rootPath = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $rootItem.FullName).Path) -replace '[\\/]+$',''

  $pending = New-Object 'System.Collections.Generic.Stack[string]'
  $pending.Push($rootPath)
  $entries = @()
  while ($pending.Count -gt 0) {
    $directory = $pending.Pop()
    foreach ($child in @(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop)) {
      $isReparse = [bool]($child.Attributes -band [IO.FileAttributes]::ReparsePoint)
      $lexicalPath = [IO.Path]::GetFullPath($child.FullName)
      if (-not $isReparse) {
        $lexicalPath = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $child.FullName).Path)
      }
      if (-not (Test-GatePPathUnderRoot $rootPath $lexicalPath)) {
        throw 'Gate P enumeration escaped the Q2 root'
      }
      $entry = [pscustomobject]@{
        FullName = $lexicalPath
        IsDirectory = [bool]$child.PSIsContainer
        IsReparse = $isReparse
      }
      $entries += $entry
      if ($entry.IsDirectory -and -not $entry.IsReparse) {
        $pending.Push($lexicalPath)
      }
    }
  }
  return $entries
}

function Get-GatePRelativePath {
  param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Path
  )

  $rootPath = [IO.Path]::GetFullPath($Root) -replace '[\\/]+$',''
  $fullPath = [IO.Path]::GetFullPath($Path)
  if (-not (Test-GatePPathUnderRoot $rootPath $fullPath)) {
    throw 'Gate P path is outside its canonical root'
  }
  if ($fullPath.Equals($rootPath, [StringComparison]::OrdinalIgnoreCase)) { return '.' }
  return ($fullPath.Substring($rootPath.Length) -replace '^[\\/]+','').Replace('\','/')
}

function Get-GatePFileSnapshot {
  param([Parameter(Mandatory)][string]$Root)

  $rootPath = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Root).Path) -replace '[\\/]+$',''
  $snapshot = @{}
  foreach ($entry in @(Get-GatePSafeEntries $rootPath)) {
    if ($entry.IsReparse) { throw 'Gate P refuses a reparse point under the Q2 root' }
    if ($entry.IsDirectory) { continue }
    $relative = Get-GatePRelativePath $rootPath $entry.FullName
    $content = $null
    if ([IO.Path]::GetExtension($relative) -ieq '.cfg') {
      $content = [IO.File]::ReadAllText($entry.FullName)
    }
    $snapshot[$relative] = [pscustomobject]@{
      Path = $relative
      Hash = (Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
      Content = $content
    }
  }
  return $snapshot
}

function Get-GatePFileDiffs {
  param(
    [Parameter(Mandatory)][hashtable]$Before,
    [Parameter(Mandatory)][hashtable]$After
  )

  $paths = @($Before.Keys + $After.Keys | Sort-Object -Unique)
  $diffs = @()
  foreach ($path in $paths) {
    $hasBefore = $Before.ContainsKey($path)
    $hasAfter = $After.ContainsKey($path)
    if (-not $hasBefore) {
      $diffs += [pscustomobject]@{
        Path = $path
        Kind = 'Added'
        BeforeText = $null
        AfterText = $After[$path].Content
        BeforeHash = $null
        AfterHash = $After[$path].Hash
      }
    } elseif (-not $hasAfter) {
      $diffs += [pscustomobject]@{
        Path = $path
        Kind = 'Deleted'
        BeforeText = $Before[$path].Content
        AfterText = $null
        BeforeHash = $Before[$path].Hash
        AfterHash = $null
      }
    } elseif ($Before[$path].Hash -cne $After[$path].Hash) {
      $diffs += [pscustomobject]@{
        Path = $path
        Kind = 'Changed'
        BeforeText = $Before[$path].Content
        AfterText = $After[$path].Content
        BeforeHash = $Before[$path].Hash
        AfterHash = $After[$path].Hash
      }
    }
  }
  return $diffs
}

function Write-GatePPolicyReport {
  param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows
  )

  $lines = @($Rows | ForEach-Object {
    '{0}|{1}|{2}' -f (Normalize-GatePPath ([string]$_.Path)), ([string]$_.Kind), ([string]$_.Reason)
  })
  $encoding = New-Object Text.UTF8Encoding($false)
  if ($lines.Count -eq 0) {
    [IO.File]::WriteAllText($Path, '', $encoding)
  } else {
    [IO.File]::WriteAllLines($Path, $lines, $encoding)
  }
}

function Get-GatePSessionResidue {
  param([Parameter(Mandatory)][string]$Root)

  if (-not (Test-Path -LiteralPath $Root)) {
      return [pscustomobject]@{
      Status = 'absent'
      InspectionFailed = $false
      Entries = @()
    }
  }
  $rootItem=Get-Item -LiteralPath $Root -Force -ErrorAction Stop
  if(-not $rootItem.PSIsContainer){
    return [pscustomobject]@{
      Status='residual'
      InspectionFailed=$false
      Entries=@([pscustomobject]@{Path='.';Kind='file';Reparse=[bool]($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)})
    }
  }
  try {
    $safeEntries = @(Get-GatePSafeEntries $Root)
  } catch {
    return [pscustomobject]@{
      Status = 'inspection-failed'
      InspectionFailed = $true
      Entries = @()
    }
  }

  $rootPath = [IO.Path]::GetFullPath($rootItem.FullName) -replace '[\\/]+$',''
  $entries = @($safeEntries | ForEach-Object {
    [pscustomobject]@{
      Path = Get-GatePRelativePath $rootPath $_.FullName
      Kind = if ($_.IsDirectory) { 'directory' } else { 'file' }
      Reparse = [bool]$_.IsReparse
    }
  })
  return [pscustomobject]@{
    Status = if ($entries.Count -eq 0) { 'empty' } else { 'residual' }
    InspectionFailed = $false
    Entries = $entries
  }
}

function Write-GatePSessionResidueReport {
  param(
    [Parameter(Mandatory)][string]$Path,
    [Parameter(Mandatory)][object]$Residue
  )

  $lines = @("status=$($Residue.Status)", "count=$(@($Residue.Entries).Count)")
  foreach ($entry in @($Residue.Entries)) {
    $lines += "path=$($entry.Path)|kind=$($entry.Kind)|reparse=$($entry.Reparse)"
  }
  [IO.File]::WriteAllLines($Path, $lines, (New-Object Text.UTF8Encoding($false)))
}

function Get-GatePSensitiveFileFindings {
  param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Label
  )

  if (-not (Test-Path -LiteralPath $Root)) {
    return [pscustomobject]@{
      Findings = @()
      InspectionFailed = $false
    }
  }

  try {
    $entries = @(Get-GatePSafeEntries $Root)
  } catch {
    return [pscustomobject]@{
      Findings = @()
      InspectionFailed = $true
    }
  }
  $rootPath = [IO.Path]::GetFullPath((Get-Item -LiteralPath $Root -Force).FullName) -replace '[\\/]+$',''
  $findings = @()
  $inspectionFailed = $false
  foreach ($entry in $entries) {
    if ($entry.IsDirectory) {
      if ($entry.IsReparse) { $inspectionFailed = $true }
      continue
    }
    if ($entry.IsReparse) {
      $inspectionFailed = $true
      continue
    }
    try {
      $matches = @(Select-String -LiteralPath $entry.FullName -Pattern (Get-GatePSensitiveContentPattern) -ErrorAction Stop)
    } catch {
      $inspectionFailed = $true
      continue
    }
    if ($matches.Count -gt 0) {
      $findings += [pscustomobject]@{
        Label = $Label
        Path = Get-GatePRelativePath $rootPath $entry.FullName
        FullName = $entry.FullName
        Count = $matches.Count
      }
    }
  }
  return [pscustomobject]@{
    Findings = $findings
    InspectionFailed = $inspectionFailed
  }
}

function Remove-GatePSensitiveFiles {
  param([Parameter(Mandatory)][object]$Findings)

  $failed = $false
  foreach ($finding in @($Findings.Findings)) {
    try {
      Remove-Item -LiteralPath $finding.FullName -Force -ErrorAction Stop
    } catch {
      $failed = $true
    }
  }
  return $failed
}

New-Item -ItemType Directory -Force $EvidenceDir|Out-Null;$chunks=@()
}
process {$chunks += $RoomConfigJson}
end {
  $json=$chunks -join "`n";if([string]::IsNullOrWhiteSpace($json)){throw 'Fresh room JSON was not supplied on stdin'}
  $sessionRoot=Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone\sessions'
  $q2Path=(Resolve-Path -LiteralPath $Q2).Path
  $q2Root=(Split-Path -Parent $q2Path)
  Get-ComputerInfo | Select-Object WindowsProductName,WindowsVersion,OsBuildNumber,OsArchitecture | Format-List | Out-File "$EvidenceDir/windows.txt"
  Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\EdgeUpdate\Clients\*' -ErrorAction SilentlyContinue | Select-Object name,pv | Format-Table | Out-File "$EvidenceDir/webview2.txt"
  Get-FileHash $Coordinator,$Gateway,$q2Path -Algorithm SHA256 | Format-Table -AutoSize | Out-File "$EvidenceDir/hashes.txt"
  $treeBefore=Get-GatePFileSnapshot $q2Root
  $childOutput=@($json | & $Coordinator --gateway $Gateway --q2 $Q2 --session-root $sessionRoot 2>&1)
  $exit=$LASTEXITCODE
  $sanitizedOutput=Get-GatePSanitizedOutput $childOutput
  $childOutput=$null
  $sessionLog="$EvidenceDir/session.log"
  $encoding=New-Object Text.UTF8Encoding($false)
  if(@($sanitizedOutput.Lines).Count -eq 0){[IO.File]::WriteAllText($sessionLog,'',$encoding)}
  else{[IO.File]::WriteAllLines($sessionLog,@($sanitizedOutput.Lines),$encoding)}
  @($sanitizedOutput.Lines)|ForEach-Object{Write-Host $_}
  $treeAfter=Get-GatePFileSnapshot $q2Root
  $diffs=@(Get-GatePFileDiffs $treeBefore $treeAfter)
  $policyRows=@(Invoke-GatePConfigPolicy -Diffs $diffs)
  $cfgRows=@($policyRows | Where-Object { [IO.Path]::GetExtension([string]$_.Path) -ieq '.cfg' })
  $cfgAllowed=@($cfgRows | Where-Object Allowed)
  $cfgForbidden=@($cfgRows | Where-Object { -not $_.Allowed })
  $fileForbidden=@($policyRows | Where-Object {
    [IO.Path]::GetExtension([string]$_.Path) -ine '.cfg' -and -not $_.Allowed
  })
  Write-GatePPolicyReport (Join-Path $EvidenceDir 'cfg-allowed-diff.txt') $cfgAllowed
  Write-GatePPolicyReport (Join-Path $EvidenceDir 'cfg-forbidden-diff.txt') $cfgForbidden
  Write-GatePPolicyReport (Join-Path $EvidenceDir 'file-forbidden-diff.txt') $fileForbidden
  Write-GatePPolicyReport (Join-Path $EvidenceDir 'cfg-diff.txt') $cfgRows
  $sessionResidue=Get-GatePSessionResidue $sessionRoot
  Write-GatePSessionResidueReport (Join-Path $EvidenceDir 'sessions-after.txt') $sessionResidue
  $left=Get-CimInstance Win32_Process | Where-Object {
    $_.Name -match '^q2dz|^q2-doszone|^Q2PRO-X' -or
    ($_.Name -eq 'msedgewebview2.exe' -and $_.CommandLine -like "*$sessionRoot*")
  }
  $left | Select-Object ProcessId,ParentProcessId,Name,CommandLine | Format-List | Out-File "$EvidenceDir/processes-after.txt"
  Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object {$_.LocalAddress -eq '127.0.0.1'} | Sort-Object LocalPort | Format-Table | Out-File "$EvidenceDir/udp-after.txt"
  $evidenceFindings=Get-GatePSensitiveFileFindings $EvidenceDir 'evidence'
  $sessionFindings=Get-GatePSensitiveFileFindings $sessionRoot 'session-root'
  $evidenceRemovalFailed=Remove-GatePSensitiveFiles $evidenceFindings
  $sessionRemovalFailed=Remove-GatePSensitiveFiles $sessionFindings
  $removalFailed=$evidenceRemovalFailed -or $sessionRemovalFailed
  $leakSummary=@()
  if([int]$sanitizedOutput.SensitiveCount -gt 0){$leakSummary+="source=coordinator-output|path=session.log|count=$([int]$sanitizedOutput.SensitiveCount)"}
  foreach($finding in @($evidenceFindings.Findings)){$leakSummary+="source=$($finding.Label)|path=$($finding.Path)|count=$($finding.Count)"}
  foreach($finding in @($sessionFindings.Findings)){$leakSummary+="source=$($finding.Label)|path=$($finding.Path)|count=$($finding.Count)"}
  if($evidenceFindings.InspectionFailed){$leakSummary+='source=evidence|status=inspection-failed'}
  if($sessionFindings.InspectionFailed){$leakSummary+='source=session-root|status=inspection-failed'}
  if($removalFailed){$leakSummary+='source=cleanup|status=removal-failed'}
  if($leakSummary.Count -gt 0){
    [IO.File]::WriteAllLines("$EvidenceDir/credential-leaks.txt",@("match-count=$($leakSummary.Count)")+$leakSummary,$encoding)
  }
  if($exit -ne 0){throw "Gate P session failed with exit $exit"}
  if($cfgForbidden.Count -or $fileForbidden.Count){throw 'Q2 installation changes violate Gate P config/content policy'}
  if($left){throw 'Managed session processes remained after exit'}
  if($sessionResidue.InspectionFailed -or @($sessionResidue.Entries).Count -gt 0){throw 'session root contained residual files or reparse points'}
  if($leakSummary.Count -gt 0){throw 'credential-shaped data persisted or could not be inspected'}
  Write-Host "GATE P LOCAL CHECKS PASS; verify visible movement and firing evidence manually: $EvidenceDir"
  $json=$null;[GC]::Collect()
}
