<#
.SYNOPSIS
Syntax/semantic check of individual source files with the project's compiler flags (cl /Zs).

.DESCRIPTION
Runs the MSVC front end on each file without generating objects or PDBs, so several checks can run
in parallel and while another build or the editor holds the normal build outputs. Flags mirror the
CMake RelWithDebInfo configuration (include paths, defines, C++20, /permissive-).

.EXAMPLE
tools\CheckCompile.ps1 src\passes\BloomPass.cpp src\Texture.cpp
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot

$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * -property installationPath
        $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    }
}
if (-not (Test-Path -LiteralPath $vcvars)) { Stop-Nox "vcvars64.bat not found." }

$includes = @('', 'src', 'include', 'include\GL', 'include\SDL', 'include\glm', 'include\json', 'include\imgui', 'external\lua-5.5.1\src', 'external\physfs-3.2.0\src') |
    ForEach-Object { '/I"' + (Join-Path $repo $_).TrimEnd('\') + '"' }
$flags = @('/Zs', '/nologo', '/W3', '/diagnostics:column', '/EHsc', '/MD', '/std:c++20', '/permissive-', '/Zc:inline',
    '/D_MBCS', '/DWIN32', '/D_WINDOWS', '/DNDEBUG', '/DIMGUI_DEFINE_MATH_OPERATORS', '/DIMGUIMIZMO_DISABLE_CAPTURE_MOUSE_FROM_APP', '/DPHYSFS_STATIC', '/TP') + $includes

$failed = 0
foreach ($file in $Files) {
    $full = if ([IO.Path]::IsPathRooted($file)) { $file } else { Join-Path $repo $file }
    if (-not (Test-Path -LiteralPath $full)) { Write-NoxWarning "Missing file $file"; $failed++; continue }
    $cmd = '"' + $vcvars + '" >nul && cl ' + ($flags -join ' ') + ' "' + $full + '"'
    # vcvars prints harmless stderr noise (vswhere lookups); keep going and judge by cl's output.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & cmd.exe /c $cmd 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = $previous
    $errors = $output | Where-Object { $_ -match ': (fatal )?error ' }
    if ($LASTEXITCODE -ne 0 -or $errors) {
        $failed++
        Write-Host "[NOX] FAIL $file"
        $output | Where-Object { $_ -match ': (fatal )?error |: warning C4(244|267)' -or $_ -match 'error' } | Select-Object -First 60 | ForEach-Object { Write-Host "  $_" }
    }
    else {
        Write-Host "[NOX] OK   $file"
    }
}
if ($failed -gt 0) { Stop-Nox "$failed file(s) failed to compile." }
Write-NoxStep "All $($Files.Count) file(s) compile."
