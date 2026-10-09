<#
.SYNOPSIS
Launches Nox_Engine.exe from the build output or the packaged folder.

.DESCRIPTION
Builds the command line for the engine CLI contract:
  Nox_Engine.exe [--preset <name>] [--game | --editor] [--scene <path>] [--quality high|ultra]
                 [--window windowed|borderless|fullscreen] [--width N --height N]
Presets are read from config/launch_presets.json. Unknown preset names fail before launching.
The exe is searched in build/vs2022-x64/<Config>/ and Package/NOX_Racing/ (see -Source). The engine
resolves its own asset root, so the working directory is set to the exe folder anyway.

.PARAMETER Preset
Preset name from config/launch_presets.json (for example game, game_ultra, game_windowed, editor).

.PARAMETER Scene
Scene path passed as --scene.

.PARAMETER Mode
game or editor (--game / --editor). Overrides the preset's mode.

.PARAMETER Quality
high or ultra (--quality).

.PARAMETER Window
windowed, borderless or fullscreen (--window).

.PARAMETER Width
.PARAMETER Height
Window size (--width / --height).

.PARAMETER Source
auto (default): the most recently written exe among the candidates.
build: build output only. package: Package/NOX_Racing only.

.PARAMETER Configuration
Build configuration folder to use (Release or Debug). Default: whichever is newest.

.PARAMETER Wait
Wait for the engine to exit and return its exit code.

.PARAMETER Passthrough
Any other arguments are appended to the engine command line unchanged.

.EXAMPLE
tools\run.cmd -Preset game
tools\run.cmd -Preset game_windowed -Source package
tools\run.cmd -Scene scenes/nfs_night_rain.json -Mode game -Quality ultra
#>
[CmdletBinding()]
param(
    [string]$Preset,
    [string]$Scene,
    [ValidateSet('game', 'editor')]
    [string]$Mode,
    [ValidateSet('high', 'ultra')]
    [string]$Quality,
    [ValidateSet('windowed', 'borderless', 'fullscreen')]
    [string]$Window,
    [int]$Width,
    [int]$Height,
    [ValidateSet('auto', 'build', 'package')]
    [string]$Source = 'auto',
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration,
    [switch]$Wait,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Passthrough
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$repo = Get-NoxRepoRoot

if ($Preset) {
    $presetFile = Join-Path $repo 'config/launch_presets.json'
    if (-not (Test-Path -LiteralPath $presetFile)) {
        Stop-Nox "Preset file missing: $presetFile"
    }
    $presetData = Get-Content -LiteralPath $presetFile -Raw | ConvertFrom-Json
    $names = @($presetData.presets.PSObject.Properties.Name)
    if ($names -notcontains $Preset) {
        Stop-Nox ("Unknown preset '$Preset'. Available: " + ($names -join ', ') + '.')
    }
}

# Candidate executables, newest first when -Source auto.
$candidates = @()
if ($Source -ne 'package') {
    $configs = if ($Configuration) { @($Configuration) } else { @('Release', 'Debug') }
    foreach ($config in $configs) {
        $path = Join-Path $repo "build/vs2022-x64/$config/Nox_Engine.exe"
        if (Test-Path -LiteralPath $path) { $candidates += Get-Item -LiteralPath $path }
    }
}
if ($Source -ne 'build') {
    $packagedExe = Join-Path $repo 'Package/NOX_Racing/Nox_Engine.exe'
    if (Test-Path -LiteralPath $packagedExe) { $candidates += Get-Item -LiteralPath $packagedExe }
}
if ($candidates.Count -eq 0) {
    Stop-Nox "No Nox_Engine.exe found for -Source $Source. Run tools\build.cmd or tools\package.cmd first."
}
$exeInfo = $candidates | Sort-Object -Property LastWriteTime -Descending | Select-Object -First 1
$exe = $exeInfo.FullName
$exeDir = Split-Path -Parent $exe

$arguments = @()
if ($Preset) { $arguments += @('--preset', $Preset) }
if ($Mode) { $arguments += "--$Mode" }
if ($Scene) { $arguments += @('--scene', $Scene) }
if ($Quality) { $arguments += @('--quality', $Quality) }
if ($Window) { $arguments += @('--window', $Window) }
if ($PSBoundParameters.ContainsKey('Width')) { $arguments += @('--width', [string]$Width) }
if ($PSBoundParameters.ContainsKey('Height')) { $arguments += @('--height', [string]$Height) }
if ($Passthrough) { $arguments += $Passthrough }

# Start-Process joins arguments without quoting, so quote anything with whitespace.
$commandArgs = @($arguments | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    })

Write-NoxStep "Launching: $exe $($commandArgs -join ' ')"
$process = Start-Process -FilePath $exe -ArgumentList ($commandArgs -join ' ') -WorkingDirectory $exeDir -PassThru -Wait:$Wait
if ($Wait) {
    exit $process.ExitCode
}
exit 0
