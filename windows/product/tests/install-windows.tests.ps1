[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$product = (Resolve-Path "$PSScriptRoot/..").Path
$install = Join-Path $product 'install.ps1'
$uninstall = Join-Path $product 'uninstall.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("q2dz-install-test-" + [Guid]::NewGuid().ToString('N'))
$localAppData = Join-Path $work 'localappdata'
$appData = Join-Path $work 'appdata'

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERTION FAILED: $Message" }
}

function Relative-Files([string]$Root) {
    @(Get-ChildItem -LiteralPath $Root -File -Recurse -Force | ForEach-Object {
        $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
    } | Sort-Object)
}

function Write-Manifest([string]$Root) {
    $lines = @(Relative-Files $Root | Where-Object { $_ -ne 'SHA256SUMS.txt' } | ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Root ($_ -replace '/', '\'))).Hash.ToLowerInvariant()
        "$hash  $_"
    })
    [IO.File]::WriteAllLines((Join-Path $Root 'SHA256SUMS.txt'), $lines, (New-Object Text.UTF8Encoding($false)))
}

function New-Setup([string]$Version, [string]$Name) {
    $root = Join-Path $work $Name
    $payload = Join-Path $root 'payload'
    New-Item -ItemType Directory -Force $payload | Out-Null
    foreach ($relative in @('VERSION', 'q2dz-launch.ps1', 'q2dz-session.exe', 'gateway/q2-doszone-webview2-gateway.exe')) {
        $path = Join-Path $payload ($relative -replace '/', '\')
        New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path, ("fixture:{0}:{1}" -f $Version, $relative), (New-Object Text.UTF8Encoding($false)))
    }
    [IO.File]::WriteAllText((Join-Path $payload 'VERSION'), $Version, (New-Object Text.UTF8Encoding($false)))
    foreach ($name in @('install.ps1', 'uninstall.ps1')) {
        Copy-Item -LiteralPath (Join-Path $product $name) -Destination (Join-Path $root $name)
    }
    [IO.File]::WriteAllText((Join-Path $root 'launch-installed.ps1'), 'fixture launcher', (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $root 'README.txt'), 'fixture setup', (New-Object Text.UTF8Encoding($false)))
    Write-Manifest $root
    return $root
}

function New-TestShortcut([string]$Path, [string]$Target, [string]$Arguments, [string]$WorkingDirectory) {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    $shortcut.Arguments = $Arguments
    $shortcut.WorkingDirectory = $WorkingDirectory
    $shortcut.Save()
}

function Invoke-Install([string]$SetupRoot) {
    & $install
}

function Read-Pointer {
    $path = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone/current.json'
    Assert (Test-Path -LiteralPath $path -PathType Leaf) 'current.json is missing'
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
}

function Active-Directory {
    $pointer = Read-Pointer
    Assert ($pointer.directory -match '^versions[\\/][^\\/]+$') 'current pointer directory is unsafe'
    return Join-Path (Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone') $pointer.directory
}

$oldLocal = $env:LOCALAPPDATA
$oldApp = $env:APPDATA
try {
    New-Item -ItemType Directory -Force $localAppData, $appData | Out-Null
    $env:LOCALAPPDATA = $localAppData
    $env:APPDATA = $appData

    $setupV1 = New-Setup '1.0.0' 'setup-v1'
    # The installer is invoked from a disposable setup directory, exactly as from an extracted ZIP.
    $setupScript = Join-Path $work 'setup-v1-run'
    New-Item -ItemType Directory -Force $setupScript | Out-Null
    Copy-Item -Path (Join-Path $setupV1 '*') -Destination $setupScript -Recurse -Force
    $install = Join-Path $setupScript 'install.ps1'
    $uninstall = Join-Path $setupScript 'uninstall.ps1'
    & $install

    $root = Join-Path $localAppData 'Q2PRO-X Dos.Zone'
    $activeV1 = Active-Directory
    Assert ((Get-Content -LiteralPath (Join-Path $activeV1 'VERSION') -Raw).Trim() -eq '1.0.0') 'first install did not activate v1'
    Assert (Test-Path -LiteralPath (Join-Path $activeV1 '.q2dz-owned') -PathType Leaf) 'owned marker is missing'
    Assert (Test-Path -LiteralPath (Join-Path $root 'launch.ps1') -PathType Leaf) 'stable launch.ps1 is missing'
    Assert (Test-Path -LiteralPath (Join-Path $root 'uninstall.ps1') -PathType Leaf) 'stable uninstall.ps1 is missing'

    # A legacy root path is migrated when no newer state file exists.
    $legacy = Join-Path $root 'q2-path.txt'
    [IO.File]::WriteAllText($legacy, 'legacy-path', (New-Object Text.UTF8Encoding($false)))
    & $install
    Assert ((Get-Content -LiteralPath (Join-Path $root 'state/q2-path.txt') -Raw).Trim() -eq 'legacy-path') 'legacy path was not migrated'
    Assert (-not (Test-Path -LiteralPath $legacy)) 'migrated legacy path was not removed'

    New-Item -ItemType Directory -Force (Join-Path $root 'state') | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'state/q2-path.txt'), 'newer-q2-path', (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $root 'unknown-sentinel.txt'), 'preserve me', (New-Object Text.UTF8Encoding($false)))

    $setupV2 = New-Setup '2.0.0' 'setup-v2'
    $setupScriptV2 = Join-Path $work 'setup-v2-run'
    New-Item -ItemType Directory -Force $setupScriptV2 | Out-Null
    Copy-Item -Path (Join-Path $setupV2 '*') -Destination $setupScriptV2 -Recurse -Force
    $install = Join-Path $setupScriptV2 'install.ps1'
    $uninstall = Join-Path $setupScriptV2 'uninstall.ps1'
    $env:Q2DZ_TEST_INTERRUPT_AFTER_STAGE = '1'
    $failed = $false
    try { & $install } catch { $failed = $true }
    Assert $failed 'interrupted update failpoint was not exercised'
    Assert ((Get-Content -LiteralPath (Join-Path (Active-Directory) 'VERSION') -Raw).Trim() -eq '1.0.0') 'failed update changed active version'
    Assert ((Read-Pointer).version -eq '1.0.0') 'failed update changed current pointer'
    Remove-Item Env:Q2DZ_TEST_INTERRUPT_AFTER_STAGE -ErrorAction SilentlyContinue

    & $install
    $activeV2 = Active-Directory
    Assert ((Get-Content -LiteralPath (Join-Path $activeV2 'VERSION') -Raw).Trim() -eq '2.0.0') 'successful update did not activate v2'
    Assert ((Get-ChildItem -LiteralPath (Join-Path $root 'versions') -Directory | Where-Object { Test-Path (Join-Path $_.FullName '.q2dz-owned') }).Count -ge 2) 'prior complete version was not retained'

    $outside = Join-Path $work 'outside'
    New-Item -ItemType Directory -Force $outside | Out-Null
    [IO.File]::WriteAllText((Join-Path $outside 'sentinel.txt'), 'outside', (New-Object Text.UTF8Encoding($false)))

    $setupV3 = New-Setup '3.0.0' 'setup-v3'
    $setupScriptV3 = Join-Path $work 'setup-v3-run'
    New-Item -ItemType Directory -Force $setupScriptV3 | Out-Null
    Copy-Item -Path (Join-Path $setupV3 '*') -Destination $setupScriptV3 -Recurse -Force
    $installV3 = Join-Path $setupScriptV3 'install.ps1'

    # An unrelated same-name file must not be overwritten, and activation must
    # remain on v2 when shortcut preparation fails.
    $startMenu = Join-Path $appData 'Microsoft\Windows\Start Menu\Programs'
    $primaryLink = Join-Path $startMenu 'Q2PRO-X - Dos.Zone.lnk'
    $ownedLinkBackup = Join-Path $work 'owned-launch.lnk'
    Move-Item -LiteralPath $primaryLink -Destination $ownedLinkBackup
    [IO.File]::WriteAllText($primaryLink, 'user-owned shortcut placeholder', (New-Object Text.UTF8Encoding($false)))
    $failed = $false
    try { & $installV3 } catch { $failed = $true }
    Assert $failed 'unrelated same-name shortcut file was overwritten'
    Assert ((Read-Pointer).version -eq '2.0.0') 'shortcut failure changed current pointer'
    Remove-Item -LiteralPath $primaryLink -Force
    Move-Item -LiteralPath $ownedLinkBackup -Destination $primaryLink

    # A reparse in the Start Menu path must fail before any release activation.
    $programsBackup = Join-Path $work 'programs-backup'
    Move-Item -LiteralPath $startMenu -Destination $programsBackup
    & cmd.exe /c "mklink /J `"$startMenu`" `"$outside`"" | Out-Null
    $failed = $false
    try { & $installV3 } catch { $failed = $true }
    Assert $failed 'Start Menu reparse path was accepted'
    Assert ((Read-Pointer).version -eq '2.0.0') 'Start Menu failure changed current pointer'
    Assert (Test-Path -LiteralPath (Join-Path $outside 'sentinel.txt') -PathType Leaf) 'Start Menu reparse target was touched'
    & cmd.exe /c "rmdir `"$startMenu`"" | Out-Null
    Move-Item -LiteralPath $programsBackup -Destination $startMenu

    # An exact legacy install is migratable; its old app tree remains intact.
    $legacyApp = Join-Path $root 'app'
    New-Item -ItemType Directory -Force $legacyApp | Out-Null
    [IO.File]::WriteAllText((Join-Path $legacyApp 'legacy-sentinel.txt'), 'keep legacy tree', (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $legacyApp 'q2dz-launch.ps1'), 'legacy launcher', (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $legacyApp 'uninstall.ps1'), 'legacy uninstaller', (New-Object Text.UTF8Encoding($false)))
    $currentLaunchBackup = Join-Path $work 'current-launch.lnk'
    $currentUninstallBackup = Join-Path $work 'current-uninstall.lnk'
    Move-Item -LiteralPath (Join-Path $startMenu 'Q2PRO-X - Dos.Zone.lnk') -Destination $currentLaunchBackup
    Move-Item -LiteralPath (Join-Path $startMenu 'Uninstall Q2PRO-X - Dos.Zone.lnk') -Destination $currentUninstallBackup
    $powershellPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    New-TestShortcut (Join-Path $startMenu 'Q2PRO-X - Dos.Zone.lnk') $powershellPath "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $legacyApp 'q2dz-launch.ps1')`"" $legacyApp
    New-TestShortcut (Join-Path $startMenu 'Uninstall Q2PRO-X - Dos.Zone.lnk') $powershellPath "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $legacyApp 'uninstall.ps1')`"" $legacyApp
    & $installV3
    Assert ((Read-Pointer).version -eq '3.0.0') 'exact legacy shortcuts were not migrated'
    Assert (Test-Path -LiteralPath (Join-Path $legacyApp 'legacy-sentinel.txt') -PathType Leaf) 'legacy app tree was removed during migration'

    # A near-match of a legacy shortcut remains unrelated and blocks activation.
    $setupV4 = New-Setup '4.0.0' 'setup-v4'
    $setupScriptV4 = Join-Path $work 'setup-v4-run'
    New-Item -ItemType Directory -Force $setupScriptV4 | Out-Null
    Copy-Item -Path (Join-Path $setupV4 '*') -Destination $setupScriptV4 -Recurse -Force
    $installV4 = Join-Path $setupScriptV4 'install.ps1'
    $nearMatch = Join-Path $startMenu 'Q2PRO-X - Dos.Zone.lnk'
    Move-Item -LiteralPath $nearMatch -Destination (Join-Path $work 'migrated-launch.lnk')
    New-TestShortcut $nearMatch $powershellPath "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $legacyApp 'q2dz-launch.ps1')`" -Extra" $legacyApp
    $failed = $false
    try { & $installV4 } catch { $failed = $true }
    Assert $failed 'near-match legacy shortcut was accepted'
    Assert ((Read-Pointer).version -eq '3.0.0') 'near-match legacy shortcut changed current pointer'
    Remove-Item -LiteralPath $nearMatch -Force
    Move-Item -LiteralPath (Join-Path $work 'migrated-launch.lnk') -Destination $nearMatch
    Remove-Item -LiteralPath (Join-Path $startMenu 'Uninstall Q2PRO-X - Dos.Zone.lnk') -Force
    Move-Item -LiteralPath $currentUninstallBackup -Destination (Join-Path $startMenu 'Uninstall Q2PRO-X - Dos.Zone.lnk')
    $install = $installV3

    # Legacy migration must not overwrite a newer state file.
    $legacy = Join-Path $root 'q2-path.txt'
    [IO.File]::WriteAllText($legacy, 'legacy-path', (New-Object Text.UTF8Encoding($false)))
    & $install
    Assert ((Get-Content -LiteralPath (Join-Path $root 'state/q2-path.txt') -Raw).Trim() -eq 'newer-q2-path') 'legacy path overwrote newer state'
    Assert (Test-Path -LiteralPath $legacy -PathType Leaf) 'legacy path was deleted while newer state existed'

    # Uninstall must preserve a same-name shortcut whose arguments are not exact.
    $uninstallLink = Join-Path $startMenu 'Uninstall Q2PRO-X - Dos.Zone.lnk'
    $ownedUninstallBackup = Join-Path $work 'owned-uninstall.lnk'
    Move-Item -LiteralPath $uninstallLink -Destination $ownedUninstallBackup
    $shell = New-Object -ComObject WScript.Shell
    $wrongShortcut = $shell.CreateShortcut($uninstallLink)
    $wrongShortcut.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $wrongShortcut.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $root 'uninstall.ps1')`" -Extra"
    $wrongShortcut.WorkingDirectory = $env:TEMP
    $wrongShortcut.Save()

    # Refuse a reparse point before deleting any owned path and never follow it.
    $userStage = Join-Path $root 'versions/.staging-user-data'
    New-Item -ItemType Directory -Force $userStage | Out-Null
    [IO.File]::WriteAllText((Join-Path $userStage 'sentinel.txt'), 'preserve staging', (New-Object Text.UTF8Encoding($false)))
    $junction = Join-Path $root 'versions/reparse-junction'
    & cmd.exe /c "mklink /J `"$junction`" `"$outside`"" | Out-Null
    Assert (Test-Path -LiteralPath $junction) 'test junction was not created'
    $failed = $false
    try { & $uninstall } catch { $failed = $true }
    Assert $failed 'uninstall followed or ignored a reparse point'
    Assert (Test-Path -LiteralPath (Join-Path $outside 'sentinel.txt') -PathType Leaf) 'outside reparse target was touched'
    Assert (Test-Path -LiteralPath (Join-Path $root 'state/q2-path.txt') -PathType Leaf) 'failed uninstall removed state'

    & cmd.exe /c "rmdir `"$junction`"" | Out-Null
    & $uninstall
    Assert (Test-Path -LiteralPath (Join-Path $root 'state/q2-path.txt') -PathType Leaf) 'ordinary uninstall removed state'
    Assert (Test-Path -LiteralPath (Join-Path $root 'unknown-sentinel.txt') -PathType Leaf) 'ordinary uninstall removed unknown sentinel'
    Assert (Test-Path -LiteralPath (Join-Path $userStage 'sentinel.txt') -PathType Leaf) 'ordinary uninstall removed unknown staging directory'
    Assert (Test-Path -LiteralPath $uninstallLink -PathType Leaf) 'uninstall removed a shortcut with non-exact arguments'
    Assert (-not (Test-Path -LiteralPath (Join-Path $root 'current.json'))) 'ordinary uninstall left current pointer'
    Assert (-not (Test-Path -LiteralPath (Join-Path $root 'launch.ps1'))) 'ordinary uninstall left stable launch script'
    Assert (-not (Get-ChildItem -LiteralPath (Join-Path $root 'versions') -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'reparse-junction' -and (Test-Path (Join-Path $_.FullName '.q2dz-owned')) })) 'ordinary uninstall left owned version'

    [IO.File]::WriteAllText((Join-Path $root 'state/unknown-state.txt'), 'preserve state', (New-Object Text.UTF8Encoding($false)))
    & $uninstall -ResetState
    Assert (-not (Test-Path -LiteralPath (Join-Path $root 'state/q2-path.txt'))) 'ResetState did not remove owned q2 path'
    Assert (Test-Path -LiteralPath (Join-Path $root 'state/unknown-state.txt')) 'ResetState removed unknown state'
    Assert (Test-Path -LiteralPath (Join-Path $userStage 'sentinel.txt')) 'ResetState removed unknown staging directory'

    Write-Host 'windows transactional install/uninstall contract: PASS'
} finally {
    if ($oldLocal) { $env:LOCALAPPDATA = $oldLocal } else { Remove-Item Env:LOCALAPPDATA -ErrorAction SilentlyContinue }
    if ($oldApp) { $env:APPDATA = $oldApp } else { Remove-Item Env:APPDATA -ErrorAction SilentlyContinue }
    Remove-Item Env:Q2DZ_TEST_INTERRUPT_AFTER_STAGE -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
