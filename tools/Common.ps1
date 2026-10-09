# Shared helpers for tools/*.ps1. Dot-source this file; do not run it directly.
#
# Output conventions (the editor's ProcessRunner parses these):
#   [NOX-PROGRESS] 0.42   best-effort progress in 0..1
#   [NOX] ...             status message
#   [NOX] ERROR: ...      failure (the editor shows the first line containing "error")

$script:NoxRepoRoot = Split-Path -Parent $PSScriptRoot

function Get-NoxRepoRoot {
    return $script:NoxRepoRoot
}

function Write-NoxProgress {
    param([double]$Value)
    $clamped = [Math]::Max(0.0, [Math]::Min(1.0, $Value))
    Write-Host ('[NOX-PROGRESS] ' + $clamped.ToString('0.00', [Globalization.CultureInfo]::InvariantCulture))
}

function Write-NoxStep {
    param([string]$Message)
    Write-Host "[NOX] $Message"
}

function Write-NoxWarning {
    param([string]$Message)
    Write-Host "[NOX] WARNING: $Message"
}

function Stop-Nox {
    param([string]$Message, [int]$ExitCode = 1)
    Write-Host "[NOX] ERROR: $Message"
    exit $ExitCode
}

# Finds cmake.exe: PATH, the CMake bundled with VS 2022 Community, then vswhere.
function Find-NoxCMake {
    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $bundled = 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    if (Test-Path -LiteralPath $bundled) { return $bundled }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/Installer/vswhere.exe')
    )
    foreach ($vswhere in $vswhereCandidates) {
        if (-not (Test-Path -LiteralPath $vswhere)) { continue }
        $installs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
        foreach ($install in @($installs)) {
            if (-not $install) { continue }
            $candidate = Join-Path $install 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    return $null
}

# Copies a file only when its size or timestamp differs (copy_if_different semantics).
# Returns $true when a copy was made.
function Copy-NoxFileIfChanged {
    param([string]$Source, [string]$Destination)

    $destDir = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $destDir)) {
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    }
    $src = Get-Item -LiteralPath $Source
    if (Test-Path -LiteralPath $Destination) {
        $dst = Get-Item -LiteralPath $Destination
        if ($dst.Length -eq $src.Length -and $dst.LastWriteTimeUtc -eq $src.LastWriteTimeUtc) {
            return $false
        }
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    (Get-Item -LiteralPath $Destination).LastWriteTimeUtc = $src.LastWriteTimeUtc
    return $true
}

# Mirrors a directory with robocopy (refreshes changed files, removes files that no longer exist
# in the source). Throws on robocopy failure.
function Sync-NoxDirectory {
    param([string]$Source, [string]$Destination)

    & robocopy.exe $Source $Destination /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    # robocopy exit codes 0-7 are success (bit flags for copied/extra/mismatched files).
    if ($LASTEXITCODE -ge 8) {
        throw "robocopy failed (exit code $LASTEXITCODE) while syncing '$Source' to '$Destination'."
    }
}

