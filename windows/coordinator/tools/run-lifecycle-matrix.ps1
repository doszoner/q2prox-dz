[CmdletBinding()]
param([string]$BuildDir = "$PSScriptRoot/../out/release")
$ErrorActionPreference='Stop'
$coord=(Resolve-Path "$BuildDir/q2dz-session.exe").Path
$gateway=(Resolve-Path "$BuildDir/q2dz-fake-gateway.exe").Path
$q2=(Resolve-Path "$BuildDir/q2dz-fake-q2.exe").Path
$root=Join-Path $env:TEMP ("q2dz-matrix-"+[guid]::NewGuid())
New-Item -ItemType Directory -Force $root | Out-Null
$config='{"version":1,"roomId":"test","alias":"q2.test","ordered":false,"iceServers":[]}'
function Assert-NoOrphans([string]$caseDir){Start-Sleep -Milliseconds 700;foreach($f in 'gateway.pid','q2.pid'){ $p=Join-Path $caseDir $f;if(Test-Path $p){$id=[int](Get-Content $p);if(Get-Process -Id $id -ErrorAction SilentlyContinue){throw "orphan process $id from $f"}}};$sessions=Join-Path $caseDir 'sessions';if(Test-Path $sessions){$left=Get-ChildItem $sessions -Force -ErrorAction SilentlyContinue;if($left){throw "orphan session data: $($left.FullName -join ', ')"}}}
function Run-Case([string]$name,[string]$gwMode,[string]$q2Mode,[int]$expected,[bool]$expectQ2,[int]$ready=5000){$dir=Join-Path $root $name;New-Item -ItemType Directory -Force $dir|Out-Null;$q2ForCase=$q2;$deleteQ2Path=$null;if($gwMode -eq 'delete-q2-after-ready'){$deleteQ2Path=Join-Path $dir 'q2-copy.exe';Copy-Item -LiteralPath $q2 -Destination $deleteQ2Path -Force;$q2ForCase=$deleteQ2Path;$env:Q2DZ_TEST_DELETE_Q2_AFTER_READY=$deleteQ2Path}else{Remove-Item Env:Q2DZ_TEST_DELETE_Q2_AFTER_READY -ErrorAction SilentlyContinue};$env:Q2DZ_PID_DIR=$dir;$env:Q2DZ_FAKE_GATEWAY_MODE=$gwMode;$env:Q2DZ_FAKE_Q2_MODE=$q2Mode;try{$config | & $coord --gateway $gateway --q2 $q2ForCase --session-root (Join-Path $dir 'sessions') --ready-timeout-ms $ready --graceful-timeout-ms 500;$code=$LASTEXITCODE;if($code -ne $expected){throw "$name returned $code, expected $expected"};$q2Started=Test-Path (Join-Path $dir 'q2.pid');if($q2Started -ne $expectQ2){throw "$name q2-started=$q2Started, expected $expectQ2"};if($deleteQ2Path -and (Test-Path $deleteQ2Path)){throw "$name fake gateway did not delete the Q2 copy"};Assert-NoOrphans $dir;Write-Host "PASS $name (exit=$code q2-started=$q2Started)"}finally{Remove-Item Env:Q2DZ_TEST_DELETE_Q2_AFTER_READY -ErrorAction SilentlyContinue}}
Run-Case normal-q2-exit normal exit-normal 0 $true
Run-Case gateway-exit exit-after-ready long-running 11 $true
Run-Case room-close room-close long-running 10 $true
Run-Case room-close-immediate room-close-immediate long-running 10 $true
Run-Case fatal-immediate fatal-immediate long-running 13 $true
Run-Case q2-launch-failure delete-q2-after-ready long-running 2 $false
Run-Case no-newline-flood no-newline-flood long-running 13 $false
Run-Case line-flood line-flood long-running 13 $false
Run-Case readiness-timeout never-ready long-running 12 $false 800
Run-Case malformed-ready malformed-ready long-running 13 $false
Remove-Item Env:Q2DZ_PID_DIR,Env:Q2DZ_FAKE_GATEWAY_MODE,Env:Q2DZ_FAKE_Q2_MODE -ErrorAction SilentlyContinue
Write-Host "LIFECYCLE MATRIX PASS: $root"
