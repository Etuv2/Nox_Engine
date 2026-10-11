param(
    [ValidateSet('capture', 'timing')][string]$Mode = 'capture',
    [int]$Width = 3440,
    [int]$Height = 1440,
    [string]$Scene = 'scenes/shuto_buildings.json',
    [string]$Output = '',
    [string]$LogPrefix = 'building_section',
    [switch]$SkipOptimization
)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskExe = Join-Path $taskRoot 'build/vs2022-x64/Release/Nox_Engine.exe'
if (-not $SkipOptimization) {
    & python (Join-Path $taskRoot 'tools/shuto/optimize_building_glbs.py')
    if ($LASTEXITCODE -ne 0) { throw 'Building vertex optimization failed' }
    & python (Join-Path $taskRoot 'tools/shuto/validate_buildings.py')
    if ($LASTEXITCODE -ne 0) { throw 'Optimized building validation failed' }
}
$env:NOX_BENCHMARK = 'all'
$env:NOX_BENCHMARK_MODE = $Mode
if ($Output -eq '') { $Output = "captures/shuto/buildings/section_$Mode" }
$env:NOX_BENCHMARK_OUT = $Output
$taskProcess = Start-Process -FilePath $taskExe -ArgumentList @(
    '--game', '--scene', $Scene, '--quality', 'high',
    '--window', 'windowed', '--width', $Width, '--height', $Height
) -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -Wait `
    -RedirectStandardOutput (Join-Path $taskRoot "logs/${LogPrefix}_$Mode.log") `
    -RedirectStandardError (Join-Path $taskRoot "logs/${LogPrefix}_$Mode.err")
if ($taskProcess.ExitCode -ne 0) { throw "NOX exited with code $($taskProcess.ExitCode)" }
