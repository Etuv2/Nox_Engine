<#
.SYNOPSIS
End-to-end check of the new-project templates: create, run, package and run the package.
.DESCRIPTION
For each template in templates/:
- creates a project with the command line (Nox_Engine.exe --new-project) in a unique projects/<name> folder and
  checks that a second create without --force is refused, and that a path-traversal name is refused;
- runs the project's main scene for NOX_EXIT_AFTER_FRAMES frames and checks the exit code, that begin_play ran and
  that the log has no "[Lua] error" line.
The third_person_3d project is then packaged with tools/Package.ps1 (its generated profile, -SkipBuild) and the
package is run by tools/TestPackagedRun.ps1.

The engine reads projects through the checkout's asset root, so the test projects live under projects/ with a unique
name and are removed afterwards (also on failure), together with packaging/<name>.json and Package/<name>.
Requires a built Release executable: build/vs2022-x64/Release/Nox_Engine.exe.
.EXAMPLE
powershell -NoProfile -File tests\NewProjectTests.ps1
#>
[CmdletBinding()]
param(
    [int]$Frames = 120,
    [int]$PackageFrames = 200,
    [int]$TimeoutSec = 300,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/Common.ps1')
$repo = Get-NoxRepoRoot
$exe = Join-Path $repo 'build/vs2022-x64/Release/Nox_Engine.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build the Release executable first: $exe is missing." }

$stamp = ([Guid]::NewGuid().ToString('N')).Substring(0, 8)
$work = Join-Path ([IO.Path]::GetTempPath()) ("nox-newproject-$stamp")
New-Item -ItemType Directory -Path $work | Out-Null
$templates = @('empty_3d', 'third_person_3d')
$cleanup = New-Object System.Collections.Generic.List[string]
$failures = New-Object System.Collections.Generic.List[string]

function Invoke-Nox {
    param([string[]]$Arguments, [hashtable]$Environment = @{}, [string]$LogName, [string]$FilePath = $exe)
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = (@($Arguments) | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.WorkingDirectory = $work
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($key in $Environment.Keys) { $psi.EnvironmentVariables[$key] = [string]$Environment[$key] }
    $process = [Diagnostics.Process]::Start($psi)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSec * 1000)) {
        $process.Kill()
        throw "Nox_Engine.exe $($Arguments -join ' ') exceeded $TimeoutSec s."
    }
    $process.WaitForExit()
    $text = $stdout.Result + "`n" + $stderr.Result
    [IO.File]::WriteAllText((Join-Path $work $LogName), $text)
    return [pscustomobject]@{ ExitCode = $process.ExitCode; Text = $text }
}

function Check {
    param([bool]$Condition, [string]$Message)
    if ($Condition) { Write-NoxStep "PASS $Message" } else { Write-NoxStep "FAIL $Message"; $failures.Add($Message) }
}

try {
    foreach ($template in $templates) {
        $name = "nptest_${template}_$stamp"
        $folder = "projects/$name"
        $projectDir = Join-Path $repo $folder
        $profile = Join-Path $repo "packaging/$name.json"
        $cleanup.Add($projectDir); $cleanup.Add($profile); $cleanup.Add((Join-Path $repo "Package/$name"))
        Write-NoxStep "== $template -> $folder"

        $create = Invoke-Nox -Arguments @('--new-project', $name, '--template', $template, '--path', $folder) -LogName "$template.create.log"
        Check ($create.ExitCode -eq 0) "create $template exits 0 (got $($create.ExitCode))"
        foreach ($leaf in @('project.json', 'input.json', 'scripts/main.lua', 'scenes/main.json', 'README.md')) {
            Check (Test-Path -LiteralPath (Join-Path $projectDir $leaf) -PathType Leaf) "$template creates $leaf"
        }
        Check (Test-Path -LiteralPath $profile -PathType Leaf) "$template writes packaging/$name.json"
        if (Test-Path -LiteralPath $profile -PathType Leaf) {
            $profileText = [IO.File]::ReadAllText($profile)
            Check (-not $profileText.Contains('{{')) "$template profile has no unsubstituted tokens"
            Check ($profileText.Contains("`"$folder/project.json`"")) "$template profile names $folder/project.json"
        }
        if (Test-Path -LiteralPath (Join-Path $projectDir 'project.json')) {
            $descriptor = Get-Content -LiteralPath (Join-Path $projectDir 'project.json') -Raw | ConvertFrom-Json
            Check ($descriptor.name -eq $name) "$template project.json carries the name"
        }

        $again = Invoke-Nox -Arguments @('--new-project', $name, '--template', $template, '--path', $folder) -LogName "$template.again.log"
        Check ($again.ExitCode -ne 0 -and $again.Text -match 'already exists') "$template refuses to overwrite without --force"

        $run = Invoke-Nox -Arguments @('--game', '--scene', "$folder/scenes/main.json") -Environment @{
            NOX_EXIT_AFTER_FRAMES = [string]$Frames; NOX_FIXED_DT = '0.0166666667' } -LogName "$template.run.log"
        Check ($run.ExitCode -eq 0) "$template runs $Frames frames and exits 0 (got $($run.ExitCode))"
        Check ($run.Text -match "\[Lua\] $([regex]::Escape($name)): begin_play") "$template runs begin_play"
        $luaErrors = @($run.Text -split "`n" | Where-Object { $_ -match '^\[Lua\] error' })
        Check ($luaErrors.Count -eq 0) "$template has no Lua errors in the log ($($luaErrors.Count))"
        foreach ($line in $luaErrors) { Write-NoxStep "  $line" }
    }

    # Names and folders are checked before anything is written.
    $bad = Invoke-Nox -Arguments @('--new-project', '..\escape', '--template', 'empty_3d') -LogName 'bad_name.log'
    Check ($bad.ExitCode -ne 0 -and $bad.Text -match 'must start with a letter|may only use') 'a path-like project name is refused'
    $outside = Invoke-Nox -Arguments @('--new-project', "nptest_out_$stamp", '--template', 'empty_3d', '--path', "..\nptest_out_$stamp") -LogName 'outside.log'
    Check ($outside.ExitCode -ne 0 -and $outside.Text -match 'direct child of projects') 'a project folder outside projects/ is refused'
    Check (-not (Test-Path -LiteralPath (Join-Path $repo "..\nptest_out_$stamp"))) 'nothing is written outside the checkout'

    # Package the third-person project with its own profile, then run the package.
    $tp = "nptest_third_person_3d_$stamp"
    $package = Join-Path $repo "Package/$tp"
    $pack = Invoke-Nox -FilePath 'powershell.exe' -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'tools/Package.ps1'), '-Profile', $tp, '-SkipBuild') -LogName 'package.log'
    Check ($pack.ExitCode -eq 0) "package $tp exits 0 (got $($pack.ExitCode))"
    foreach ($leaf in @('Nox_Engine.exe', 'Play.cmd', 'Editor.cmd', 'nox_bootstrap.json', 'package-manifest.json', 'engine.pak', 'game.pak')) {
        Check (Test-Path -LiteralPath (Join-Path $package $leaf)) "package has $leaf"
    }
    if (Test-Path -LiteralPath $package) {
        $bytes = (Get-ChildItem -LiteralPath $package -Recurse -File | Measure-Object -Property Length -Sum).Sum
        Write-NoxStep ('package size {0:N1} MiB' -f ($bytes / 1MB))
    }
    $testRun = Invoke-Nox -FilePath 'powershell.exe' -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'tools/TestPackagedRun.ps1'), '-Package', "Package\$tp", '-Frames', [string]$PackageFrames) -LogName 'testpackagedrun.log'
    Check ($testRun.ExitCode -eq 0) "TestPackagedRun passes on the package (exit $($testRun.ExitCode))"
    Write-NoxStep (($testRun.Text -split "`n" | Where-Object { $_ -match '^\[NOX\]' } | Select-Object -Last 4) -join "`n")
}
finally {
    foreach ($path in $cleanup) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $full = [IO.Path]::GetFullPath($path)
        if (-not ($full.StartsWith((Join-Path $repo 'projects'), [StringComparison]::OrdinalIgnoreCase) -or
                  $full.StartsWith((Join-Path $repo 'packaging'), [StringComparison]::OrdinalIgnoreCase) -or
                  $full.StartsWith((Join-Path $repo 'Package'), [StringComparison]::OrdinalIgnoreCase))) {
            throw "Refusing to remove outside the test folders: $full"
        }
        if ($KeepArtifacts) { Write-NoxStep "kept $full"; continue }
        Get-ChildItem -LiteralPath $full -Recurse -Force -File | Where-Object { (Get-Item $_.FullName).Attributes -band [IO.FileAttributes]::ReparsePoint } |
            ForEach-Object { throw "Refusing to delete through a link: $($_.FullName)" }
        Remove-Item -LiteralPath $full -Recurse -Force
    }
    if (-not $KeepArtifacts) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($failures.Count) {
    Write-NoxStep ('FAILED: ' + ($failures -join '; '))
    exit 1
}
Write-NoxStep 'NewProjectTests: all checks passed'
