<#
.SYNOPSIS
Release build serialized by a machine-wide mutex, for several people or agents sharing one checkout.

.DESCRIPTION
Waits for the "NoxBuild" mutex, runs the vs2022-x64-release CMake preset and releases the mutex.
Linking fails while build/vs2022-x64/Release/Nox_Engine.exe is running; long validation runs should
use a copy made with -Snapshot (Nox_Engine_<name>.exe next to the original, same runtime DLLs).

.EXAMPLE
tools\BuildLocked.ps1
tools\BuildLocked.ps1 -Snapshot traversal
#>
[CmdletBinding()]
param(
    [string]$Snapshot = '',
    [int]$TimeoutMinutes = 30
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot
$cmake = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = 'cmake' }

$mutex = New-Object System.Threading.Mutex($false, 'Global\NoxBuild')
$acquired = $false
try {
    try { $acquired = $mutex.WaitOne([TimeSpan]::FromMinutes($TimeoutMinutes)) }
    catch [System.Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) { Stop-Nox "Timed out waiting for the build lock." }
    Push-Location $repo
    try {
        & $cmake --build --preset vs2022-x64-release --parallel 2>&1 |
            Where-Object { $_ -match ' error |error [A-Z]+[0-9]+|warning C4(244|267|456|457)|-> ' } |
            ForEach-Object { Write-Host $_ }
        $code = $LASTEXITCODE
    }
    finally { Pop-Location }
    if ($code -ne 0) { Stop-Nox "Build failed (exit $code)." }
    if ($Snapshot -ne '') {
        $exe = Join-Path $repo 'build/vs2022-x64/Release/Nox_Engine.exe'
        $copy = Join-Path $repo "build/vs2022-x64/Release/Nox_Engine_$Snapshot.exe"
        Copy-Item -LiteralPath $exe -Destination $copy -Force
        Write-NoxStep "Snapshot: $copy"
    }
    Write-NoxStep 'Build succeeded.'
}
finally {
    if ($acquired) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
