<#
.SYNOPSIS
Reports or removes redundant build assets and reproducible Visual Studio caches.
.DESCRIPTION
The default is a dry run. -Apply deletes only byte-identical source asset copies under
known build output folders. Changed or missing originals are preserved. -IdeCache also
clears .vs and obsolete legacy object caches when Visual Studio/the compiler are closed.
Captures, logs, snapshots, source assets and the current CMake build are preserved.
.EXAMPLE
tools\Clean.ps1
tools\Clean.ps1 -Apply -IdeCache
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param([switch]$Apply, [switch]$IdeCache)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot
$assetDirs = @('config', 'fonts', 'hdrs', 'images', 'models', 'scenes', 'shaders', 'sounds', 'textures', 'data', 'generated')
$outputs = @('build/visual_parity_runtime', 'x64/Release', 'x64/Debug')
foreach ($build in @('build/vs2022-x64', 'build/msbuild/x64', 'build/msbuild/Win32')) {
    foreach ($config in @('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')) { $outputs += "$build/$config" }
}
$hashes = @{}
$bytes = [long]0
$count = 0
$preserved = 0
foreach ($output in $outputs) {
    $outputPath = [IO.Path]::GetFullPath((Join-Path $repo $output))
    if (-not (Test-NoxPathWithin $outputPath $repo)) { throw "Cleanup target escapes repository: $output" }
    Assert-NoxNoReparsePoint $outputPath
    foreach ($dir in $assetDirs) {
        $target = Join-Path $outputPath $dir
        if (-not (Test-Path -LiteralPath $target -PathType Container)) { continue }
        if ((Get-Item -LiteralPath $target -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            # Existing parity-test junctions share source assets and consume no duplicate space.
            Write-NoxStep "Preserved asset junction: $output/$dir"
            continue
        }
        Assert-NoxNoReparsePoint $target
        foreach ($file in Get-ChildItem -LiteralPath $target -Recurse -File -Force) {
            Assert-NoxNoReparsePoint $file.FullName
            $source = Join-Path $repo ($file.FullName.Substring($outputPath.Length + 1))
            Assert-NoxNoReparsePoint $source
            if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
                (Get-Item -LiteralPath $source).Length -ne $file.Length) { $preserved++; continue }
            if (-not $hashes.ContainsKey($source)) { $hashes[$source] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash }
            if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -ne $hashes[$source]) { $preserved++; continue }
            $bytes += $file.Length
            $count++
            if ($Apply -and $PSCmdlet.ShouldProcess($file.FullName, 'Remove verified duplicate build asset')) {
                Remove-Item -LiteralPath $file.FullName -Force
            }
        }
        if ($Apply) {
            foreach ($empty in Get-ChildItem -LiteralPath $target -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending) {
                if (-not @(Get-ChildItem -LiteralPath $empty.FullName -Force).Count -and $PSCmdlet.ShouldProcess($empty.FullName, 'Remove empty directory')) {
                    Remove-Item -LiteralPath $empty.FullName
                }
            }
            if (-not @(Get-ChildItem -LiteralPath $target -Force).Count -and $PSCmdlet.ShouldProcess($target, 'Remove empty asset directory')) {
                Remove-Item -LiteralPath $target
            }
        }
    }
}
if ($IdeCache) {
    if (Get-Process devenv,MSBuild,cl -ErrorAction SilentlyContinue) { throw 'Close Visual Studio and stop builds before clearing IDE/legacy caches.' }
    foreach ($cache in @('.vs', 'Nox_Engine/Debug', 'Nox_Engine/x64/Debug', 'Nox_Engine/x64/Release', 'x64/Debug', 'x64/Release', 'Debug', 'Release')) {
        $path = [IO.Path]::GetFullPath((Join-Path $repo $cache))
        if (-not (Test-NoxPathWithin $path $repo)) { throw "Cache target escapes repository: $cache" }
        if (-not (Test-Path -LiteralPath $path -PathType Container)) { continue }
        Assert-NoxNoReparsePoint $path
        $items = @(Get-ChildItem -LiteralPath $path -Recurse -Force)
        foreach ($item in $items) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked cache entry: $($item.FullName)" }
        }
        # Legacy object trees can contain diagnostic output; only remove compiler products.
        if ($cache -ne '.vs') {
            foreach ($file in $items | Where-Object { -not $_.PSIsContainer }) {
                if ($file.Extension -notin @('.obj', '.pdb', '.idb', '.ilk', '.pch', '.ipch', '.ipdb', '.iobj', '.ifc', '.dt', '.exp', '.lib', '.tlog', '.lastbuildstate', '.exe', '.dll', '.recipe')) { $preserved++; continue }
                $bytes += $file.Length; $count++
                if ($Apply -and $PSCmdlet.ShouldProcess($file.FullName, 'Remove legacy compiler product')) { Remove-Item -LiteralPath $file.FullName -Force }
            }
        }
        else {
            $bytes += ($items | Where-Object { -not $_.PSIsContainer } | Measure-Object Length -Sum).Sum
            $count += @($items | Where-Object { -not $_.PSIsContainer }).Count
            if ($Apply -and $PSCmdlet.ShouldProcess($path, 'Remove reproducible Visual Studio cache')) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
    }
}
$verb = if ($Apply -and -not $WhatIfPreference) { 'Removed' } else { 'Can remove' }
Write-NoxStep ('{0}: {1} files, {2:N2} GiB. Preserved divergent/unmatched files: {3}.' -f $verb, $count, ($bytes / 1GB), $preserved)
