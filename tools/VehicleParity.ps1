<#
.SYNOPSIS
Vehicle parity: runs the recorded vehicle scenarios with a build and compares the per-sub-step telemetry
(NOX_VEHICLE_TELEMETRY JSONL) against the baseline recordings.

.DESCRIPTION
Scenarios: maneuvers and gearbox at NOX_FIXED_DT 1/60, 1/144, 1/30 (nfs_sprint, NOX_DRIVE=1), and the
autopilot races nfs_sprint and shuto_buildings at 1/60 (NOX_DRIVE_AUTOPILOT=1, NOX_EXIT_ON_FINISH=1).
Each run writes <name>.jsonl and <name>.log into -OutDir. The comparison reads both JSONL files line by
line (one record per fixed sub-step):

  * identical lines: byte-equal text (values are written with %.9g, which round-trips float exactly);
  * other lines: parsed and compared field by field; a field violates the tolerance when
    |run - base| > AbsTolerance + RelTolerance * |base|.

Default tolerances are zero: the target is bit-identical telemetry. The verdict per scenario is
IDENTICAL, WITHIN_TOLERANCE, DIVERGED (first differing sub-step and the largest field difference
reported) or MISSING. Console logs are not compared (the timing lines are not deterministic).

.PARAMETER Exe
Engine executable to test. Default: build\vs2022-x64\Release\Nox_Engine_vehicle.exe (BuildLocked -Snapshot vehicle).

.PARAMETER BaselineDir
Baseline JSONL directory. Default: build\baseline\vehicle\jsonl.

.PARAMETER OutDir
Where the candidate recordings go. Default: %TEMP%\nox_team\impl\vehicle\parity.

.PARAMETER SkipRun
Compare existing recordings in OutDir without running the engine.

.EXAMPLE
tools\VehicleParity.ps1
tools\VehicleParity.ps1 -Scenarios maneuvers_60,gearbox_30 -AbsTolerance 1e-6
#>
[CmdletBinding()]
param(
    [string]$Exe = '',
    [string]$BaselineDir = '',
    [string]$OutDir = '',
    [string[]]$Scenarios = @('maneuvers_30', 'maneuvers_60', 'maneuvers_144', 'gearbox_30', 'gearbox_60', 'gearbox_144', 'autopilot_nfs_sprint_60', 'autopilot_shuto_60'),
    [double]$AbsTolerance = 0.0,
    [double]$RelTolerance = 0.0,
    [switch]$SkipRun,
    # Also writes every policy call (NOX_VEHICLE_POLICY_TRACE) to <OutDir>\<scenario>.policy.jsonl.
    [switch]$PolicyTrace,
    [switch]$FailOnDiff
)

# Continue: the engine writes benign loader warnings to stderr (also in the baseline logs), which
# PowerShell would otherwise turn into terminating errors. Failures are judged by the JSONL comparison.
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot

if (-not $Exe) { $Exe = Join-Path $repo 'build\vs2022-x64\Release\Nox_Engine_vehicle.exe' }
if (-not $BaselineDir) { $BaselineDir = Join-Path $repo 'build\baseline\vehicle\jsonl' }
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP 'nox_team\impl\vehicle\parity' }
$Exe = (Resolve-Path -LiteralPath $Exe).Path
if (-not (Test-Path -LiteralPath $BaselineDir)) { Stop-Nox "Baseline directory not found: $BaselineDir" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Scenario definitions (same parameters as the baseline recordings).
$frames = @{ '60' = 2600; '144' = 6300; '30' = 1300 }
$dts = @{ '60' = '0.0166666667'; '144' = '0.0069444444'; '30' = '0.0333333333' }
$common = @('--game', '--scene', 'scenes/nfs_sprint.json', '--window', 'windowed', '--width', '1280', '--height', '720')
$shuto = @('--game', '--scene', 'scenes/shuto_buildings.json', '--window', 'windowed', '--width', '1280', '--height', '720')
$driveEnv = @('NOX_DRIVE', 'NOX_DRIVE_SCRIPT', 'NOX_FIXED_DT', 'NOX_EXIT_AFTER_FRAMES', 'NOX_DRIVE_AUTOPILOT', 'NOX_EXIT_ON_FINISH', 'NOX_VEHICLE_TELEMETRY', 'NOX_VEHICLE_POLICY_TRACE')

function Get-ScenarioSpec([string]$name) {
    if ($name -match '^(maneuvers|gearbox)_(\d+)$') {
        $script = $Matches[1]; $rate = $Matches[2]
        if (-not $dts.ContainsKey($rate)) { Stop-Nox "Unknown rate in scenario $name" }
        return @{ Env = @{ NOX_DRIVE = '1'; NOX_DRIVE_SCRIPT = $script; NOX_FIXED_DT = $dts[$rate]; NOX_EXIT_AFTER_FRAMES = "$($frames[$rate])" }; Args = $common }
    }
    if ($name -eq 'autopilot_nfs_sprint_60') {
        return @{ Env = @{ NOX_DRIVE = '1'; NOX_DRIVE_AUTOPILOT = '1'; NOX_EXIT_ON_FINISH = '1'; NOX_FIXED_DT = $dts['60']; NOX_EXIT_AFTER_FRAMES = '30000' }; Args = $common }
    }
    if ($name -eq 'autopilot_shuto_60') {
        return @{ Env = @{ NOX_DRIVE = '1'; NOX_DRIVE_AUTOPILOT = '1'; NOX_EXIT_ON_FINISH = '1'; NOX_FIXED_DT = $dts['60']; NOX_EXIT_AFTER_FRAMES = '30000' }; Args = $shuto }
    }
    Stop-Nox "Unknown scenario $name"
}

function Invoke-Recording([string]$name) {
    $spec = Get-ScenarioSpec $name
    foreach ($k in $driveEnv) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $spec.Env.Keys) { Set-Item "Env:$k" $spec.Env[$k] }
    $jsonl = Join-Path $OutDir "$name.jsonl"
    $log = Join-Path $OutDir "$name.log"
    if (Test-Path -LiteralPath $jsonl) { Remove-Item -LiteralPath $jsonl -Force }
    $env:NOX_VEHICLE_TELEMETRY = $jsonl
    if ($PolicyTrace) { $env:NOX_VEHICLE_POLICY_TRACE = Join-Path $OutDir "$name.policy.jsonl" }
    Push-Location $repo
    try {
        $t = Get-Date
        & $Exe @($spec.Args) *> $log
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        foreach ($k in $driveEnv) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    }
    [pscustomobject]@{ Scenario = $name; ExitCode = $code; Seconds = [int]((Get-Date) - $t).TotalSeconds; Log = $log }
}

function Get-RecordFields($line) {
    $obj = $line | ConvertFrom-Json
    $map = @{}
    foreach ($p in $obj.PSObject.Properties) { $map[$p.Name] = [double]$p.Value }
    return $map
}

function Compare-Recording([string]$basePath, [string]$runPath, [double]$absTol, [double]$relTol) {
    $result = [ordered]@{ Scenario = ''; BaseLines = 0; RunLines = 0; Identical = 0; Violations = 0; FirstDiffLine = 0; MaxAbs = 0.0; MaxField = ''; MaxFieldLine = 0; Verdict = '' }
    if (-not (Test-Path -LiteralPath $runPath)) { $result.Verdict = 'MISSING'; return [pscustomobject]$result }
    $b = [System.IO.File]::ReadAllLines($basePath)
    $r = [System.IO.File]::ReadAllLines($runPath)
    $result.BaseLines = $b.Length
    $result.RunLines = $r.Length
    $n = [Math]::Min($b.Length, $r.Length)
    for ($i = 0; $i -lt $n; $i++) {
        if ($b[$i] -ceq $r[$i]) { $result.Identical++; continue }
        if ($result.FirstDiffLine -eq 0) { $result.FirstDiffLine = $i + 1 }
        $bf = Get-RecordFields $b[$i]
        $rf = Get-RecordFields $r[$i]
        foreach ($k in $bf.Keys) {
            $bv = $bf[$k]; $rv = $rf[$k]
            $d = [Math]::Abs($bv - $rv)
            if ([double]::IsNaN($d)) { $d = [double]::PositiveInfinity }
            if ($d -gt $result.MaxAbs) { $result.MaxAbs = $d; $result.MaxField = $k; $result.MaxFieldLine = $i + 1 }
            if ($d -gt ($absTol + $relTol * [Math]::Abs($bv))) { $result.Violations++ }
        }
    }
    if ($b.Length -ne $r.Length) { $result.Violations++ }
    if ($result.Identical -eq $b.Length -and $b.Length -eq $r.Length) { $result.Verdict = 'IDENTICAL' }
    elseif ($result.Violations -eq 0 -and $result.FirstDiffLine -eq 0 -and $b.Length -eq $r.Length) { $result.Verdict = 'IDENTICAL' }
    elseif ($result.Violations -eq 0) { $result.Verdict = 'WITHIN_TOLERANCE' }
    else { $result.Verdict = 'DIVERGED' }
    return [pscustomobject]$result
}

$runs = @()
if (-not $SkipRun) {
    foreach ($name in $Scenarios) {
        Write-NoxStep "Recording $name"
        $run = Invoke-Recording $name
        $runs += $run
        Write-Host ("  exit={0} secs={1}" -f $run.ExitCode, $run.Seconds)
    }
}

$rows = @()
foreach ($name in $Scenarios) {
    $base = Join-Path $BaselineDir "$name.jsonl"
    $run = Join-Path $OutDir "$name.jsonl"
    if (-not (Test-Path -LiteralPath $base)) { $rows += [pscustomobject]@{ Scenario = $name; Verdict = 'NO_BASELINE' }; continue }
    $c = Compare-Recording $base $run $AbsTolerance $RelTolerance
    $c.Scenario = $name
    $rows += [pscustomobject]$c
}

Write-Host ''
Write-Host ("Exe: {0}" -f $Exe)
Write-Host ("Baseline: {0}" -f $BaselineDir)
Write-Host ("Tolerance: abs {0} rel {1}" -f $AbsTolerance, $RelTolerance)
$rows | Format-Table -AutoSize @{ L = 'Scenario'; E = { $_.Scenario } }, @{ L = 'Verdict'; E = { $_.Verdict } },
    @{ L = 'Substeps(base)'; E = { $_.BaseLines } }, @{ L = 'Substeps(run)'; E = { $_.RunLines } },
    @{ L = 'Identical'; E = { $_.Identical } }, @{ L = 'FirstDiff'; E = { $_.FirstDiffLine } },
    @{ L = 'Violations'; E = { $_.Violations } }, @{ L = 'MaxAbs'; E = { if ($_.MaxField) { '{0:g6} ({1}@{2})' -f $_.MaxAbs, $_.MaxField, $_.MaxFieldLine } else { '0' } } } | Out-String -Width 220 | Write-Host

$bad = @($rows | Where-Object { $_.Verdict -notin @('IDENTICAL', 'WITHIN_TOLERANCE') })
if ($bad.Count -gt 0) {
    Write-NoxWarning ("{0} scenario(s) not identical or within tolerance" -f $bad.Count)
    if ($FailOnDiff) { exit 1 }
}
else {
    Write-NoxStep 'All scenarios match the baseline.'
}
