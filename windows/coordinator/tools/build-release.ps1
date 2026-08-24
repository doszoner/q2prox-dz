[CmdletBinding()]
param(
  [string]$BuildDir = "$PSScriptRoot/../out/release",
  [string]$Generator = 'Visual Studio 17 2022',
  [string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path

function Invoke-Checked([string]$File, [object[]]$Arguments) {
  & $File @Arguments
  if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $File" }
}

$generatorArgs = @('-G', $Generator)
if ($Generator -match 'Visual Studio') { $generatorArgs += @('-A', $Architecture) }
$configureArgs = @('-S', $root, '-B', $BuildDir) + $generatorArgs + @('-DQ2DZ_BUILD_TESTS=ON')
Invoke-Checked 'cmake' $configureArgs
Invoke-Checked 'cmake' @('--build', $BuildDir, '--config', 'Release')
Invoke-Checked 'ctest' @('--test-dir', $BuildDir, '-C', 'Release', '--output-on-failure')
if (Get-Command node -ErrorAction SilentlyContinue) {
  Invoke-Checked 'node' @('--test', "$root/web/managed-readiness-probe.test.mjs")
} else { Write-Warning 'Node.js is absent; JS contract test was not run.' }
Write-Host "BUILD PASS: $BuildDir"
