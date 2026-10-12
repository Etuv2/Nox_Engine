<#
.SYNOPSIS
Measures launch-to-exit time and peak memory of Nox_Engine for a list of scenes.

.DESCRIPTION
Each run starts the executable with NOX_EXIT_AFTER_FRAMES (default 5) so the time covers process
start, device creation, configuration, scene load and a few frames. Peak working set and peak
private bytes come from the process object after exit. The first run of each scene is reported as
"first" (closest to cold that is possible without flushing the OS file cache, which needs admin
rights); the median of the remaining runs is "warm". Results are written as JSON.

.EXAMPLE
tools\MeasureStartup.ps1 -Exe build\baseline\bin\Nox_Engine_baseline.exe -Out build\baseline\startup.json
tools\MeasureStartup.ps1 -Exe Package\NOX_Racing\Nox_Engine.exe -Scenes scenes/nfs_sprint.json -Runs 5
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string[]]$Scenes = @('scenes/cornell_box.json|--no-game --game', 'scenes/nfs_sprint.json|--game', 'scenes/shuto_buildings.json|--game'),
    [int]$Runs = 4,
    [int]$Frames = 5,
    [string]$Out = '',
    [string]$WorkingDirectory = ''
)

$ErrorActionPreference = 'Stop'
$exePath = [IO.Path]::GetFullPath($Exe)
if (-not (Test-Path -LiteralPath $exePath)) { throw "Executable not found: $exePath" }
if (-not $WorkingDirectory) { $WorkingDirectory = Split-Path -Parent $exePath }

$results = @()
foreach ($spec in $Scenes) {
    $parts = $spec.Split('|')
    $scene = $parts[0]
    $extra = if ($parts.Count -gt 1) { $parts[1] } else { '--game' }
    $samples = @()
    for ($i = 0; $i -lt $Runs; ++$i) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exePath
        $psi.Arguments = "$extra --scene $scene --window windowed --width 1280 --height 720"
        $psi.WorkingDirectory = $WorkingDirectory
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables['NOX_EXIT_AFTER_FRAMES'] = "$Frames"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $proc = [Diagnostics.Process]::Start($psi)
        $stdout = $proc.StandardOutput.ReadToEndAsync()
        $stderr = $proc.StandardError.ReadToEndAsync()
        $peakWs = 0; $peakPrivate = 0
        while (-not $proc.HasExited) {
            try {
                $proc.Refresh()
                if ($proc.PeakWorkingSet64 -gt $peakWs) { $peakWs = $proc.PeakWorkingSet64 }
                if ($proc.PrivateMemorySize64 -gt $peakPrivate) { $peakPrivate = $proc.PrivateMemorySize64 }
            } catch {}
            Start-Sleep -Milliseconds 20
        }
        $proc.WaitForExit()
        $sw.Stop()
        $samples += [pscustomobject]@{
            ms = [math]::Round($sw.Elapsed.TotalMilliseconds, 1)
            exit = $proc.ExitCode
            peak_working_set_mib = [math]::Round($peakWs / 1MB, 1)
            peak_private_mib = [math]::Round($peakPrivate / 1MB, 1)
            log_tail = (($stdout.Result + $stderr.Result) -split "`n" | Select-Object -Last 3) -join ' | '
        }
    }
    $warm = @($samples | Select-Object -Skip 1 | Sort-Object ms)
    $median = if ($warm.Count) { $warm[[int][math]::Floor($warm.Count / 2)].ms } else { $samples[0].ms }
    $results += [pscustomobject]@{
        scene = $scene
        args = $extra
        first_ms = $samples[0].ms
        warm_median_ms = $median
        peak_working_set_mib = ($samples | Measure-Object peak_working_set_mib -Maximum).Maximum
        peak_private_mib = ($samples | Measure-Object peak_private_mib -Maximum).Maximum
        exit_codes = @($samples | ForEach-Object { $_.exit })
        samples = $samples
    }
    Write-Host ('{0,-34} first {1,8:N0} ms  warm {2,8:N0} ms  peakWS {3,7:N1} MiB  exits {4}' -f $scene, $samples[0].ms, $median,
        ($samples | Measure-Object peak_working_set_mib -Maximum).Maximum, ((@($samples | ForEach-Object { $_.exit })) -join ','))
}
$doc = [pscustomobject]@{ exe = $exePath; date = (Get-Date).ToString('s'); frames = $Frames; runs = $Runs; results = $results }
if ($Out) {
    $json = $doc | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText([IO.Path]::GetFullPath($Out), $json + "`n", (New-Object Text.UTF8Encoding($false)))
}
