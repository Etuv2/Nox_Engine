<#
.SYNOPSIS
Checks that the generic engine executable contains no compiled racing game code.

.DESCRIPTION
Nox_Engine.exe links NoxEngineCore only. The legacy racing game (GameSession and its files) and the procedural
street generators are linked into Nox_RacingLegacy.exe and nox_cook only (CMakeLists.txt, docs/engine_boundary.md).
This script scans the binary for identifiers and log tags that only the racing code contains. Names of the
racing code are not allowed in Nox_Engine.exe, and the legacy executable must still contain them (sanity check
that the markers are real).

.PARAMETER Exe
Path to Nox_Engine.exe. Default: build/vs2022-x64/Release/Nox_Engine.exe.

.EXAMPLE
powershell -NoProfile -File tools\CheckGenericExe.ps1
#>
[CmdletBinding()]
param(
    [string]$Exe = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$repo = Get-NoxRepoRoot
if (-not $Exe) { $Exe = Join-Path $repo 'build/vs2022-x64/Release/Nox_Engine.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { Stop-Nox "Executable not found: $Exe" }

# Identifiers and tags from the racing sources (GameSession, DriveMode, RacingHUD, WorldMapUI, LegacyNfsPolicy,
# NightStreetPacks, ProceduralStreetKit, NoxRacingGame). Any of them in the generic executable is a failure.
$forbidden = @(
    'GameSession', 'DriveMode', 'RacingHUD', 'WorldMapUI', 'LegacyNfsPolicy', 'NightStreetPacks',
    'ProceduralStreetKit', 'NoxRacingGame', 'RegisterNoxRacing', 'street_kit', 'nfs_district', '[Race]'
)
$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Exe).Path))
$found = @($forbidden | Where-Object { $text.Contains($_) })
if ($found.Count -gt 0) {
    Stop-Nox ("Generic executable contains racing code markers: " + ($found -join ', ') + " ($Exe)")
}
Write-NoxStep "Generic executable has no racing code markers: $Exe"
exit 0
