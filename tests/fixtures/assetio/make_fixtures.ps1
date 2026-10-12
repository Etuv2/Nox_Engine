# Regenerates the AssetIO test fixtures (tests/fixtures/assetio):
#   tree/                 loose source tree (the reference content)
#   assetio_fixture.pak   the same files as a ZIP: stored entries under stored/, Deflate entries under deflate/
# Output is deterministic (fixed LCG seed, fixed content). Run from any directory:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\fixtures\assetio\make_fixtures.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Join-Path $PSScriptRoot 'tree'
$pak = Join-Path $PSScriptRoot 'assetio_fixture.pak'
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
New-Item -ItemType Directory -Path $root | Out-Null

function Write-Bytes([string]$rel, [byte[]]$bytes) {
    $path = Join-Path $root ($rel -replace '/', '\')
    New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force | Out-Null
    [System.IO.File]::WriteAllBytes($path, $bytes)
}
function Write-Text([string]$rel, [string]$text) {
    Write-Bytes $rel ([System.Text.Encoding]::ASCII.GetBytes($text))
}

# Deterministic pseudo-random bytes (incompressible), 20000 bytes.
$blob = New-Object byte[] 20000
[int64]$state = 12345
for ($i = 0; $i -lt $blob.Length; $i++) {
    $state = ($state * 1103515245 + 12345) % 2147483648
    $blob[$i] = [byte](($state -shr 16) -band 0xff)
}

$deflateText = ''
for ($i = 0; $i -lt 200; $i++) { $deflateText += "deflate line $i of a repetitive asset`n" }

Write-Text 'stored/hello.txt' "hello stored`n"
Write-Bytes 'stored/blob.bin' $blob
Write-Text 'deflate/hello.txt' $deflateText
Write-Bytes 'deflate/blob.bin' $blob
Write-Text 'Case/Mixed.txt' "mixed case file`n"
Write-Text 'nested/a/b/c.txt' "deep`n"
Write-Text 'models/tex/x.txt' "texture stand-in`n"
Write-Text 'models/scene.gltf' "{`"asset`":{`"version`":`"2.0`"}}`n"
Write-Text 'empty.txt' ''

if (Test-Path -LiteralPath $pak) { Remove-Item -LiteralPath $pak -Force }
$zip = [System.IO.Compression.ZipFile]::Open($pak, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $files = Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName
    foreach ($file in $files) {
        $rel = $file.FullName.Substring($root.Length + 1) -replace '\\', '/'
        $level = if ($rel.StartsWith('deflate/') -or $rel.StartsWith('nested/') -or $rel.StartsWith('models/') -or $rel.StartsWith('Case/')) {
            [System.IO.Compression.CompressionLevel]::Optimal
        } else {
            [System.IO.Compression.CompressionLevel]::NoCompression
        }
        $entry = $zip.CreateEntry($rel, $level)
        $entry.LastWriteTime = New-Object DateTimeOffset(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $in = [System.IO.File]::OpenRead($file.FullName)
        try {
            $out = $entry.Open()
            try { $in.CopyTo($out) } finally { $out.Dispose() }
        } finally { $in.Dispose() }
    }
} finally {
    $zip.Dispose()
}
Write-Host "Wrote $root and $pak"
