[CmdletBinding()]
param([string]$BuildDir = "$PSScriptRoot/../out/release")
$ErrorActionPreference='Stop'
$coord=(Resolve-Path "$BuildDir/q2dz-session.exe").Path
$gateway=(Resolve-Path "$BuildDir/q2dz-fake-gateway.exe").Path
$q2=(Resolve-Path "$BuildDir/q2dz-fake-q2.exe").Path
$root=Join-Path $env:TEMP ("q2dz-matrix-"+[guid]::NewGuid())
New-Item -ItemType Directory -Force $root | Out-Null
function Assert-NoOrphans([string]$caseDir){Start-Sleep -Milliseconds 700;foreach($p in @(Get-ChildItem $caseDir -Filter '*.pid' -File -ErrorAction SilentlyContinue)){ $id=[int](Get-Content $p.FullName);if(Get-Process -Id $id -ErrorAction SilentlyContinue){throw "orphan process $id from $($p.Name)"}};$sessions=Join-Path $caseDir 'sessions';if(Test-Path $sessions){$left=Get-ChildItem $sessions -Force -ErrorAction SilentlyContinue;if($left){throw "orphan session data: $($left.FullName -join ', ')"}}}
function Quote-ProcessArgument([string]$value){if($value -notmatch '[\s"]'){return $value};return '"'+$value.Replace('"','\"')+'"'}
function Wait-ForPath([string]$path,[bool]$present,[int]$timeoutMs=10000){$deadline=[Environment]::TickCount+$timeoutMs;do{if((Test-Path -LiteralPath $path)-eq $present){return $true};Start-Sleep -Milliseconds 100}while([Environment]::TickCount -lt $deadline);return $false}
function Start-HubCoordinator([string]$caseDir,[string]$payload){
  $psi=New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName=$coord
  $args=@('--gateway',$gateway,'--q2',$q2,'--session-root',(Join-Path $caseDir 'sessions'),'--ready-timeout-ms','5000','--graceful-timeout-ms','500')
  $psi.Arguments=($args|ForEach-Object{Quote-ProcessArgument $_})-join ' '
  $psi.UseShellExecute=$false;$psi.RedirectStandardInput=$true
  $process=New-Object System.Diagnostics.Process;$process.StartInfo=$psi
  if(-not $process.Start()){throw 'could not start coordinator for hard-kill case'}
  $process.StandardInput.Write($payload);$process.StandardInput.Close()
  return $process
}
function Run-HubCase([string]$name,[string]$modeA,[string]$modeB,[string]$q2Mode){
  $dir=Join-Path $root $name;New-Item -ItemType Directory -Force $dir|Out-Null
  $env:Q2DZ_PID_DIR=$dir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_GATEWAY_MODE_A=$modeA;$env:Q2DZ_FAKE_GATEWAY_MODE_B=$modeB;$env:Q2DZ_FAKE_Q2_MODE=$q2Mode
  $hubPayload='{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}' + [Environment]::NewLine + '{"version":1,"roomId":"room-b","alias":"q2.b","ordered":false,"iceServers":[]}'
  try {
    $hubPayload | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $dir 'sessions') --ready-timeout-ms 5000 --graceful-timeout-ms 500
    $code=$LASTEXITCODE;if($code -ne 0){throw "$name returned $code, expected 0"}
    if(-not (Test-Path (Join-Path $dir 'q2.pid'))){throw "$name did not start Q2"}
    if(-not (Test-Path (Join-Path $dir 'hub-q2-observed-b-alive'))){throw "$name did not observe unaffected gateway B alive"}
    if(-not (Test-Path (Join-Path $dir 'gateway-room-a.pid'))){throw "$name did not start gateway A"}
    if(-not (Test-Path (Join-Path $dir 'gateway-room-b.pid'))){throw "$name did not start gateway B"}
    Assert-NoOrphans $dir;Write-Host "PASS $name (exit=$code unaffected-gateway=B q2=alive-during-room-close)"
  } finally {
    Remove-Item Env:Q2DZ_FAKE_GATEWAY_MODE_A,Env:Q2DZ_FAKE_GATEWAY_MODE_B -ErrorAction SilentlyContinue
  }
}
Run-HubCase hub-room-close room-close normal hub-room-close
Run-HubCase hub-gateway-crash exit-after-ready normal hub-room-crash
Run-HubCase hub-inactive-room-close room-close-immediate normal hub-room-close
Run-HubCase hub-fatal-room fatal-immediate normal hub-room-close
Run-HubCase hub-malformed-room malformed-ready normal hub-room-close
Run-HubCase hub-no-newline-room no-newline-flood normal hub-room-close
Run-HubCase hub-line-flood-room line-flood normal hub-room-close

$timeoutDir=Join-Path $root 'hub-readiness-timeout';New-Item -ItemType Directory -Force $timeoutDir|Out-Null
$env:Q2DZ_PID_DIR=$timeoutDir;$env:Q2DZ_FAKE_GATEWAY_MODE_A='never-ready';$env:Q2DZ_FAKE_GATEWAY_MODE_B='never-ready';$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='long-running'
try {
  $timeoutPayload='{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}' + [Environment]::NewLine + '{"version":1,"roomId":"room-b","alias":"q2.b","ordered":false,"iceServers":[]}'
  $timeoutPayload | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $timeoutDir 'sessions') --ready-timeout-ms 800 --graceful-timeout-ms 500
  if($LASTEXITCODE -ne 12){throw "hub-readiness-timeout returned $LASTEXITCODE, expected 12"}
  if(Test-Path (Join-Path $timeoutDir 'q2.pid')){throw 'hub-readiness-timeout started Q2'}
  Assert-NoOrphans $timeoutDir;Write-Host 'PASS hub-readiness-timeout'
} finally {
  Remove-Item Env:Q2DZ_FAKE_GATEWAY_MODE_A,Env:Q2DZ_FAKE_GATEWAY_MODE_B,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue
}

$oneHubDir=Join-Path $root 'hub-one-room';New-Item -ItemType Directory -Force $oneHubDir|Out-Null
$env:Q2DZ_PID_DIR=$oneHubDir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='exit-normal'
try { '{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}' | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $oneHubDir 'sessions');if($LASTEXITCODE -ne 0){throw "hub-one-room returned $LASTEXITCODE, expected 0"};if(-not (Test-Path (Join-Path $oneHubDir 'q2.pid'))){throw 'hub-one-room did not start Q2'};Assert-NoOrphans $oneHubDir;Write-Host 'PASS hub-one-room' } finally { Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue }

$q2CrashDir=Join-Path $root 'hub-q2-crash';New-Item -ItemType Directory -Force $q2CrashDir|Out-Null
$env:Q2DZ_PID_DIR=$q2CrashDir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='crash'
try { '{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}' | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $q2CrashDir 'sessions');if($LASTEXITCODE -ne 74){throw "hub-q2-crash returned $LASTEXITCODE, expected 74"};Assert-NoOrphans $q2CrashDir;Write-Host 'PASS hub-q2-crash' } finally { Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue }

$hardKillDir=Join-Path $root 'hub-hard-kill';New-Item -ItemType Directory -Force $hardKillDir|Out-Null
$env:Q2DZ_PID_DIR=$hardKillDir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='long-running'
$hardProcess=$null;$hardSession=$null
try {
  $hardPayload='{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}'
  $hardProcess=Start-HubCoordinator $hardKillDir $hardPayload
  $hardSessions=Join-Path $hardKillDir 'sessions'
  if(-not (Wait-ForPath (Join-Path $hardKillDir 'q2.pid') $true 10000)){throw 'hub-hard-kill did not start Q2'}
  if(-not (Wait-ForPath $hardSessions $true 10000)){throw 'hub-hard-kill did not create session root'}
  $entries=@(Get-ChildItem -LiteralPath $hardSessions -Directory -ErrorAction SilentlyContinue)
  if($entries.Count -ne 1){throw "hub-hard-kill expected one session directory, got $($entries.Count)"}
  $hardSession=$entries[0].FullName
  Stop-Process -Id $hardProcess.Id -Force
  if(-not $hardProcess.WaitForExit(5000)){throw 'hub-hard-kill coordinator did not exit'}
  if(-not (Wait-ForPath $hardSession $false 10000)){throw 'hub-hard-kill left session directory after owner termination'}
  Assert-NoOrphans $hardKillDir;Write-Host 'PASS hub-hard-kill (owner termination removes session)'
} finally {
  if($hardProcess -and -not $hardProcess.HasExited){Stop-Process -Id $hardProcess.Id -Force -ErrorAction SilentlyContinue;$hardProcess.WaitForExit(5000)|Out-Null}
  Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue
}

$cleanupNegativeDir=Join-Path $root 'cleanup-guard-negative';$cleanupRoot=Join-Path $cleanupNegativeDir 'sessions';$outsideRoot=Join-Path $cleanupNegativeDir 'outside';New-Item -ItemType Directory -Force $cleanupRoot,$outsideRoot|Out-Null
function New-CleanupFixture([string]$path,[string]$marker){New-Item -ItemType Directory -Force $path|Out-Null;[IO.File]::WriteAllText((Join-Path $path 'session.v1'),$marker,(New-Object Text.UTF8Encoding($false)));New-Item -ItemType File -Force (Join-Path $path 'owner.lock')|Out-Null}
function Invoke-CleanupNegative([string]$name,[string]$rootPath,[string]$sessionPath){& $coord --cleanup-session $rootPath $sessionPath --owner-handle 1;if($LASTEXITCODE -ne 2){throw "$name returned $LASTEXITCODE, expected refusal (2)"};if(-not (Test-Path -LiteralPath $sessionPath)){throw "$name unexpectedly deleted its target"};Write-Host "PASS $name (refused)"}
$validCleanupSession=Join-Path $cleanupRoot 'session-valid';$badNameCleanupSession=Join-Path $cleanupRoot 'not-a-session';$badMarkerCleanupSession=Join-Path $cleanupRoot 'session-bad-marker';$missingOwnerCleanupSession=Join-Path $cleanupRoot 'session-missing-owner';$outsideCleanupSession=Join-Path $outsideRoot 'session-outside'
New-CleanupFixture $validCleanupSession "q2dz-session-v1`n";New-CleanupFixture $badNameCleanupSession "q2dz-session-v1`n";New-CleanupFixture $badMarkerCleanupSession 'q2dz-session-v1';New-CleanupFixture $missingOwnerCleanupSession "q2dz-session-v1`n";Remove-Item -LiteralPath (Join-Path $missingOwnerCleanupSession 'owner.lock') -Force;New-CleanupFixture $outsideCleanupSession "q2dz-session-v1`n"
Invoke-CleanupNegative cleanup-guard-invalid-owner $cleanupRoot $validCleanupSession
Invoke-CleanupNegative cleanup-guard-bad-name $cleanupRoot $badNameCleanupSession
Invoke-CleanupNegative cleanup-guard-bad-marker $cleanupRoot $badMarkerCleanupSession
Invoke-CleanupNegative cleanup-guard-missing-owner $cleanupRoot $missingOwnerCleanupSession
Invoke-CleanupNegative cleanup-guard-outside-root $cleanupRoot $outsideCleanupSession
$reparseCleanupSession=Join-Path $cleanupRoot 'session-reparse';New-CleanupFixture $reparseCleanupSession "q2dz-session-v1`n"
$reparseCreated=$false
try {
  New-Item -ItemType SymbolicLink -Path (Join-Path $reparseCleanupSession 'reparse') -Target $outsideRoot -ErrorAction Stop|Out-Null;$reparseCreated=$true
} catch { Write-Host 'SKIP cleanup-guard-reparse (symbolic-link creation requires Windows permission)' }
if($reparseCreated){Invoke-CleanupNegative cleanup-guard-reparse $cleanupRoot $reparseCleanupSession}
Remove-Item -LiteralPath $cleanupNegativeDir -Recurse -Force

$emptyHubDir=Join-Path $root 'hub-empty-input';New-Item -ItemType Directory -Force $emptyHubDir|Out-Null
$env:Q2DZ_PID_DIR=$emptyHubDir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='long-running'
try { '' | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $emptyHubDir 'sessions');if($LASTEXITCODE -ne 2){throw "hub-empty-input returned $LASTEXITCODE, expected 2"};Assert-NoOrphans $emptyHubDir;Write-Host 'PASS hub-empty-input' } finally { Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue }

$obsoleteFlagDir=Join-Path $root 'obsolete-hub-flag';New-Item -ItemType Directory -Force $obsoleteFlagDir|Out-Null
$env:Q2DZ_PID_DIR=$obsoleteFlagDir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='long-running'
try {
  '{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}' | & $coord --hub --gateway $gateway --q2 $q2 --session-root (Join-Path $obsoleteFlagDir 'sessions')
  if($LASTEXITCODE -ne 2){throw "obsolete-hub-flag returned $LASTEXITCODE, expected 2"}
  Assert-NoOrphans $obsoleteFlagDir;Write-Host 'PASS obsolete-hub-flag'
} finally { Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue }

function Run-AssignmentFailure([string]$name,[string]$role){
  $dir=Join-Path $root $name;New-Item -ItemType Directory -Force $dir|Out-Null
  $env:Q2DZ_PID_DIR=$dir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='long-running';$env:Q2DZ_TEST_FAIL_ASSIGNMENT=$role
  try {
    '{"version":1,"roomId":"room-a","alias":"q2.a","ordered":false,"iceServers":[]}' | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $dir 'sessions')
    if($LASTEXITCODE -ne 13){throw "$name returned $LASTEXITCODE, expected 13"}
    Assert-NoOrphans $dir;Write-Host "PASS $name"
  } finally { Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE,Env:Q2DZ_TEST_FAIL_ASSIGNMENT -ErrorAction SilentlyContinue }
}
Run-AssignmentFailure assignment-failure-gateway gateway
Run-AssignmentFailure assignment-failure-gateway-room gateway-room
Run-AssignmentFailure assignment-failure-q2 q2

function Run-HubInputFailure([string]$name,[string]$payload,[int]$expected){
  $dir=Join-Path $root $name;New-Item -ItemType Directory -Force $dir|Out-Null
  $env:Q2DZ_PID_DIR=$dir;$env:Q2DZ_FAKE_GATEWAY_MODE='normal';$env:Q2DZ_FAKE_Q2_MODE='long-running'
  try { $payload | & $coord --gateway $gateway --q2 $q2 --session-root (Join-Path $dir 'sessions');if($LASTEXITCODE -ne $expected){throw "$name returned $LASTEXITCODE, expected $expected"};Assert-NoOrphans $dir;Write-Host "PASS $name" } finally { Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue }
}
Run-HubInputFailure hub-third-line ('{"roomId":"room-a"}' + [Environment]::NewLine + '{"roomId":"room-b"}' + [Environment]::NewLine + '{"roomId":"room-c"}') 13
Run-HubInputFailure hub-non-object '[]' 2
Run-HubInputFailure hub-oversized-line ('{' + ('x' * 70000) + '}') 13

Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue
Write-Host "LIFECYCLE MATRIX PASS: $root"
