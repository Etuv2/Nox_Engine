<#
.SYNOPSIS
Configures (if needed) and builds Nox_Engine with the vs2022-x64 CMake presets.

.DESCRIPTION
Runs "cmake --preset vs2022-x64" when build/vs2022-x64 has no CMakeCache.txt yet (or when
-Reconfigure is given), then "cmake --build --preset vs2022-x64-<config>". The POST_BUILD step in
CMake copies only runtime DLLs next to the exe. Development binaries use assets in the source tree.

.PARAMETER Configuration
Release (default) or Debug.

.PARAMETER Reconfigure
Run the configure step even if the build folder already exists.

.EXAMPLE
tools\build.cmd
tools\build.cmd -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$repo = Get-NoxRepoRoot
$buildDir = Join-Path $repo 'build/vs2022-x64'
$cacheFile = Join-Path $buildDir 'CMakeCache.txt'

$cmake = Find-NoxCMake
if (-not $cmake) {
    Stop-Nox 'cmake.exe not found. Checked PATH, the CMake bundled with Visual Studio 2022 and vswhere. Install the "C++ CMake tools for Windows" component.'
}
Write-NoxStep "Using CMake: $cmake"
Write-NoxProgress 0.0

Push-Location -LiteralPath $repo
try {
    if ($Reconfigure -or -not (Test-Path -LiteralPath $cacheFile)) {
        Write-NoxStep 'Configuring: cmake --preset vs2022-x64'
        & $cmake --preset vs2022-x64
        if ($LASTEXITCODE -ne 0) {
            Stop-Nox "CMake configure failed (exit code $LASTEXITCODE). See the first error above."
        }
    }
    Write-NoxProgress 0.05

    $preset = if ($Configuration -eq 'Debug') { 'vs2022-x64-debug' } else { 'vs2022-x64-release' }
    Write-NoxStep "Building ${Configuration}: cmake --build --preset $preset"
    Write-NoxProgress 0.1
    & $cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) {
        Stop-Nox "Build failed (exit code $LASTEXITCODE). The first line containing 'error' above is the usual cause."
    }

    $exe = Join-Path $buildDir (Join-Path $Configuration 'Nox_Engine.exe')
    if (-not (Test-Path -LiteralPath $exe)) {
        Stop-Nox "Build finished but $exe was not produced."
    }
    Write-NoxProgress 1.0
    Write-NoxStep "Build OK: $exe"
}
finally {
    Pop-Location
}
exit 0
