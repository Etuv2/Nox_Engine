<#
.SYNOPSIS
Fails when engine code outside the OpenGL RHI backend uses the OpenGL API directly.

.DESCRIPTION
The renderer talks to the GPU only through src/rhi (rhi::Device). OpenGL calls (gl*), GL types
(GLuint, GLenum, ...), GL_ constants and GL headers are allowed only in src/rhi/gl/. Third-party
code (tiny_gltf, stb, imgui under include/) is not scanned.

.EXAMPLE
tools\CheckNoRawGL.ps1
#>
[CmdletBinding()]
param([switch]$Summary)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot
$src = Join-Path $repo 'src'
$backend = (Join-Path $src 'rhi\gl').ToLowerInvariant()
$skip = @('tiny_gltf.h', 'stb_image.h', 'stb_image_write.h', 'json.hpp', 'bend_sss_cpu.h')

# gl/GL calls, GL types, GL_ constants, GL/GLEW headers. glm:: is not OpenGL.
$patterns = @(
    '\bgl[A-Z][A-Za-z0-9_]*\s*\(',
    '\bGL(uint|int|enum|float|double|sizei|boolean|bitfield|sizeiptr|intptr|uint64|int64|sync|char|ubyte|short|ushort|void)\b',
    '\bGL_[A-Z0-9_]+\b',
    '#\s*include\s*[<"](GL/|gl/)?(glew|glext|gl|wglew)\.h[>"]',
    '\bGLEW_[A-Za-z0-9_]+\b',
    'ImGui_ImplOpenGL3_'
)
$regex = [regex]::new(($patterns -join '|'))

$hits = New-Object System.Collections.Generic.List[string]
$perFile = @{}
Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { @('.cpp', '.h', '.hpp', '.inl') -contains $_.Extension.ToLowerInvariant() } | ForEach-Object {
    $path = $_.FullName
    if ($path.ToLowerInvariant().StartsWith($backend)) { return }
    if ($skip -contains $_.Name) { return }
    $rel = $path.Substring($repo.Length + 1)
    $lineNo = 0
    foreach ($line in [IO.File]::ReadLines($path)) {
        $lineNo++
        $code = ($line -split '//', 2)[0]
        if ($regex.IsMatch($code)) {
            $hits.Add("${rel}:${lineNo}: $($line.Trim())")
            $perFile[$rel] = 1 + ($perFile[$rel] -as [int])
        }
    }
}

if ($hits.Count -eq 0) {
    Write-NoxStep 'No raw OpenGL outside src/rhi/gl.'
    exit 0
}
if ($Summary) {
    $perFile.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Host ('{0,6}  {1}' -f $_.Value, $_.Key) }
}
else {
    $hits | Select-Object -First 200 | ForEach-Object { Write-Host $_ }
}
Stop-Nox "$($hits.Count) raw OpenGL use(s) in $($perFile.Count) file(s) outside src/rhi/gl."
