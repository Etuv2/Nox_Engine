<#
.SYNOPSIS
Runs a packed distribution from a copied install outside the checkout and checks that it stays read-only.
.DESCRIPTION
Copies a package to a unique folder under TEMP, launches its executable from an unrelated working directory with
NOX_EXIT_AFTER_FRAMES, and verifies that:
- the bootstrap selects packed mode and both packs are mounted (log lines naming engine.pak/game.pak);
- the run never reports loose mode and its log does not mention the checkout path;
- the install folder's files and hashes are unchanged and no extraction folder appeared.
The engine reads the packs through AssetIO and the bootstrap (docs/asset_io.md).
.EXAMPLE
tools\TestPackagedRun.ps1 -Package Package\NOX_Engine -Frames 200
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [int]$Frames = 200,
    [int]$TimeoutSec = 180
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot
$source = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($Package)) { $Package } else { Join-Path $repo $Package }))
foreach ($leaf in @('Nox_Engine.exe', 'nox_bootstrap.json', 'package-manifest.json', 'Play.cmd')) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $leaf) -PathType Leaf)) { Stop-Nox "Package is missing ${leaf}: $source" }
}
$bootstrap = [IO.File]::ReadAllText((Join-Path $source 'nox_bootstrap.json')) | ConvertFrom-Json
if ($bootstrap.mode -ne 'packed') { Stop-Nox "Bootstrap mode is '$($bootstrap.mode)', expected packed." }
foreach ($pak in @($bootstrap.packs)) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $pak) -PathType Leaf)) { Stop-Nox "Bootstrap pack missing: ${pak}" }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('nox-packed-run-' + [Guid]::NewGuid().ToString('N'))
if (-not (Test-NoxPathWithin $work ([IO.Path]::GetTempPath())) -or (Test-NoxPathWithin $work $repo)) { Stop-Nox 'Unsafe work folder.' }
$install = Join-Path $work 'install'
$cwd = Join-Path $work 'elsewhere'
New-Item -ItemType Directory -Path $install, $cwd | Out-Null

function Get-InstallSnapshot {
    param([string]$Root)
    $snapshot = @{}
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        $rel = $file.FullName.Substring($Root.Length + 1).Replace('\', '/')
        if ($file.PSIsContainer) { $snapshot[$rel + '/'] = 'dir'; continue }
        $snapshot[$rel] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $snapshot
}

try {
    Copy-Item -Path (Join-Path $source '*') -Destination $install -Recurse
    Write-NoxStep "Copied package to $install"
    $before = Get-InstallSnapshot $install

    # Arguments exactly as the packaged Play.cmd passes them.
    $playLine = (Get-Content -LiteralPath (Join-Path $install 'Play.cmd') | Where-Object { $_ -match 'Nox_Engine\.exe' } | Select-Object -First 1)
    $runArgs = @()
    if ($playLine -match 'Nox_Engine\.exe"\s*(.*)$') { $runArgs = @($Matches[1].Replace('%*', '').Trim() -split '\s+' | Where-Object { $_ }) }

    $log = Join-Path $work 'run.log'
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path $install 'Nox_Engine.exe'
    $psi.Arguments = ($runArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.WorkingDirectory = $cwd
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables['NOX_EXIT_AFTER_FRAMES'] = [string]$Frames
    $process = [Diagnostics.Process]::Start($psi)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSec * 1000)) {
        $process.Kill()
        Stop-Nox "Packaged run exceeded $TimeoutSec s."
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    [IO.File]::WriteAllText($log, $stdoutTask.Result)
    [IO.File]::WriteAllText((Join-Path $work 'run.err.log'), $stderrTask.Result)

    $text = (Get-Content -LiteralPath $log -Raw -ErrorAction SilentlyContinue) + (Get-Content -LiteralPath (Join-Path $work 'run.err.log') -Raw -ErrorAction SilentlyContinue)
    $failures = @()
    if ($exitCode -ne 0) { $failures += "exit code $exitCode" }
    foreach ($pak in @($bootstrap.packs)) {
        if ($text -notmatch [regex]::Escape($pak)) { $failures += "no mount/read log line for $pak" }
    }
    if ($text -match '(?i)loose mode') { $failures += 'run reported loose mode' }
    $unreadable = @($text -split "`n" | Where-Object { $_ -match '(?i)could not (open|load)|missing asset|\[NOX\] ERROR' })
    if ($unreadable.Count) { $failures += "run reported unreadable assets ($($unreadable.Count) lines, first: $($unreadable[0].Trim()))" }
    if ($text.IndexOf($repo, [StringComparison]::OrdinalIgnoreCase) -ge 0) { $failures += 'run log mentions the checkout path' }
    $after = Get-InstallSnapshot $install
    foreach ($key in $before.Keys) {
        if (-not $after.ContainsKey($key)) { $failures += "install file removed: $key" }
        elseif ($after[$key] -ne $before[$key]) { $failures += "install file changed: $key" }
    }
    foreach ($key in $after.Keys) {
        if (-not $before.ContainsKey($key)) { $failures += "install gained entry: $key" }
    }
    if ($failures.Count) {
        Write-NoxStep "Packaged run checks failed (log: $log)"
        foreach ($failure in $failures) { Write-NoxStep "  - $failure" }
        Get-Content -LiteralPath $log -Tail 15 -ErrorAction SilentlyContinue | ForEach-Object { Write-NoxStep "  log: $_" }
        Stop-Nox 'Packaged run verification failed.'
    }
    Write-NoxStep "Packaged run passed: exit 0, packs mounted, no checkout path, install unchanged ($($before.Count) entries)."
}
finally {
    # Only the uniquely named work folder under TEMP is removed.
    if ((Split-Path -Leaf $work) -match '^nox-packed-run-[0-9a-f]{32}$' -and (Test-NoxPathWithin $work ([IO.Path]::GetTempPath())) -and (Test-Path -LiteralPath $work)) {
        Remove-Item -LiteralPath $work -Recurse -Force
    }
}
