<#
.SYNOPSIS
Stages a standalone package from a profile and its transitive runtime dependencies.
.DESCRIPTION
Development builds share source assets. This explicit step copies only selected content,
filters authoring files, writes a size/hash manifest and removes files managed by the previous
manifest that are no longer needed. Unmanaged files and runtime saves are preserved.
.EXAMPLE
tools\package.cmd -SkipBuild
tools\package.cmd -Profile engine -SkipBuild
tools\package.cmd -GameScene scenes/my_game.json -Plan
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Output = '',
    [string]$Profile = 'nox-racing',
    [switch]$SkipBuild,
    [string]$GameScene = '',
    [switch]$Plan
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AssetPackaging.ps1')
$repo = Get-NoxRepoRoot

try {
    $profilePath = if (Test-Path -LiteralPath $Profile -PathType Leaf) { [IO.Path]::GetFullPath($Profile) } else {
        Join-Path $repo "packaging/$Profile.json"
    }
    $profileData = Read-NoxAssetJson $profilePath
    $scenes = @($profileData.scenes)
    if ($GameScene) { $scenes = @($GameScene.Replace('\', '/')) }
    if (-not $scenes.Count -or $scenes[0] -notmatch '^scenes/.+\.json$') { throw 'Profile needs a main scene under scenes/.' }
    if (-not $Output) { $Output = 'Package/' + $profileData.name }
    $outDir = if ([IO.Path]::IsPathRooted($Output)) { [IO.Path]::GetFullPath($Output) } else {
        [IO.Path]::GetFullPath((Join-Path $repo $Output))
    }
    Assert-NoxNoReparsePoint $outDir
    if ($outDir.TrimEnd('\', '/') -eq $repo.TrimEnd('\', '/') -or (Test-NoxPathWithin $repo $outDir)) {
        throw "Refusing to package into repository/ancestor '$outDir'."
    }
    if ((Test-NoxPathWithin $outDir $repo) -and -not (Test-NoxPathWithin $outDir (Join-Path $repo 'Package'))) {
        throw 'In-repository packages must use a dedicated folder under Package/.'
    }
    foreach ($protected in @($script:NoxAssetRoots) + @('src', 'include', 'lib', 'tools', 'scripts', 'tests', 'docs', 'packaging', '.git', '.agents', 'build')) {
        $path = Join-Path $repo $protected
        if ($outDir.TrimEnd('\', '/') -eq $path -or (Test-NoxPathWithin $outDir $path)) {
            throw "Package output overlaps protected directory '$protected'."
        }
    }

    # Keep source configs intact; standalone menus/presets name only packaged scenes.
    $overrides = @{}
    $config = Read-NoxAssetJson (Join-Path $repo 'config/config.json')
    $config.first_scene = $scenes[0]
    $config.scenes = @($scenes | ForEach-Object { [pscustomobject]@{ path = $_ } })
    $overrides['config/config.json'] = $config
    $launch = Read-NoxAssetJson (Join-Path $repo 'config/launch_presets.json')
    foreach ($preset in $launch.presets.PSObject.Properties) { $preset.Value.scene = $scenes[0] }
    $launch.default = if ($profileData.game) { 'game' } else { 'editor' }
    $overrides['config/launch_presets.json'] = $launch

    # Runtime icon loading needs atlases, rects and aliases, not source PNGs/exports.
    if ($profileData.game) {
        $icons = Read-NoxAssetJson (Join-Path $repo 'textures/ui_icons/manifest.json')
        $overrides['textures/ui_icons/manifest.json'] = [pscustomobject]@{
            atlases = $icons.atlases
            assets = @($icons.assets | ForEach-Object {
                $asset = [ordered]@{ id = $_.id; atlas_rects = $_.atlas_rects }
                if ($_.alias_of) { $asset.alias_of = $_.alias_of }
                [pscustomobject]$asset
            })
        }
    }
    $assets = Get-NoxPackageAssets -Root $repo -Scenes $scenes -Include $profileData.include -JsonOverrides $overrides
    $assetBytes = ($assets.Files.Values | ForEach-Object { (Get-Item -LiteralPath $_).Length } | Measure-Object -Sum).Sum
    Write-NoxStep ('Selected {0} runtime files ({1:N1} MiB) for {2}' -f $assets.Files.Count, ($assetBytes / 1MB), $profileData.name)
    if ($Plan) {
        foreach ($rel in ($assets.Files.Keys | Sort-Object)) { Write-NoxStep "$rel ($($assets.Reasons[$rel]))" }
        exit 0
    }

    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'Build.ps1') -Configuration $Configuration
        if ($LASTEXITCODE -ne 0) { throw 'Build failed; package was not created.' }
    }
    $buildOut = Join-Path $repo "build/vs2022-x64/$Configuration"
    $exeSrc = Join-Path $buildOut 'Nox_Engine.exe'
    if (-not (Test-Path -LiteralPath $exeSrc)) { throw "Build executable missing: $exeSrc" }

    $sources = @{}
    foreach ($rel in $assets.Files.Keys) { $sources[$rel] = $assets.Files[$rel] }
    $sources['Nox_Engine.exe'] = $exeSrc
    foreach ($name in @('SDL2.dll', 'SDL2_image.dll', 'SDL2_mixer.dll', 'SDL2_ttf.dll', 'glew32.dll', 'assimp-vc143-mt.dll')) {
        $dll = Join-Path $buildOut $name
        if (-not (Test-Path -LiteralPath $dll)) { $dll = Join-Path $repo $name }
        if (-not (Test-Path -LiteralPath $dll)) { throw "Runtime DLL missing: $name" }
        $sources[$name] = $dll
    }
    $exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exeSrc))
    if ($exeText.Contains('VCRUNTIME140')) {
        $crtDir = Get-ChildItem -Path "$env:ProgramFiles\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($crtDir) {
            foreach ($crt in Get-ChildItem -LiteralPath $crtDir.FullName -Filter '*.dll' -File) { $sources[$crt.Name] = $crt.FullName }
        }
        else { Write-NoxWarning 'VC++ 2022 redistributable not found; it must be installed on the target machine.' }
    }
    foreach ($license in $profileData.licenses) {
        $src = [IO.Path]::GetFullPath((Join-Path $repo $license))
        if (-not (Test-NoxPathWithin $src $repo)) { throw "License escapes repository: $license" }
        if (-not (Test-Path -LiteralPath $src)) { throw "License/attribution file missing: $license" }
        $sources['licenses/' + $license.Replace('/', '_')] = $src
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $manifestPath = Join-Path $outDir 'package-manifest.json'
    $oldManifest = if (Test-Path -LiteralPath $manifestPath) { Read-NoxAssetJson $manifestPath } else { $null }
    if (-not $oldManifest -and @(Get-ChildItem -LiteralPath $outDir -Force).Count) {
        # Migrate old packages conservatively: only source-identical asset copies can be owned.
        $legacy = @()
        foreach ($file in Get-ChildItem -LiteralPath $outDir -Recurse -File) {
            Assert-NoxNoReparsePoint $file.FullName
            $rel = $file.FullName.Substring($outDir.TrimEnd('\', '/').Length + 1).Replace('\', '/')
            if ($rel.Split('/')[0] -notin $script:NoxAssetRoots -or $sources.ContainsKey($rel)) { continue }
            $original = Join-Path $repo $rel
            if (-not (Test-Path -LiteralPath $original -PathType Leaf)) { continue }
            Assert-NoxNoReparsePoint $original
            if ((Get-Item -LiteralPath $original).Length -ne $file.Length) { continue }
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            if ($hash -eq (Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash) {
                $legacy += [pscustomobject]@{ path = $rel; sha256 = $hash }
            }
        }
        $oldManifest = [pscustomobject]@{ files = $legacy }
        Write-NoxWarning 'Migrating an older package: source-identical obsolete assets can be removed; unmatched files are preserved.'
    }
    Write-NoxProgress 0.3
    foreach ($rel in ($sources.Keys | Sort-Object)) {
        $dest = [IO.Path]::GetFullPath((Join-Path $outDir $rel))
        if (-not (Test-NoxPathWithin $dest $outDir)) { throw "Destination escapes package: $rel" }
        Assert-NoxNoReparsePoint $dest
        Copy-NoxFileIfChanged $sources[$rel] $dest | Out-Null
        if ($overrides.ContainsKey($rel)) { Write-NoxUtf8 $dest (($overrides[$rel] | ConvertTo-Json -Depth 100) + "`n") }
    }
    Write-NoxUtf8 (Join-Path $outDir 'config/launch_presets.json') (($launch | ConvertTo-Json -Depth 100) + "`n")
    $sources['config/launch_presets.json'] = $null
    $noGame = if ($profileData.game) { '' } else { ' --no-game' }
    foreach ($entry in @(@{ Name = 'Play.cmd'; Mode = 'game' }, @{ Name = 'Editor.cmd'; Mode = 'editor' })) {
        $text = '@echo off' + "`r`n" + 'start "" /D "%~dp0" "%~dp0Nox_Engine.exe" --preset ' + $entry.Mode + $noGame + " %*`r`n"
        Write-NoxUtf8 (Join-Path $outDir $entry.Name) $text
        $sources[$entry.Name] = $null
    }

    # Delete only unchanged files owned by the previous package; keep edited configs/saves.
    foreach ($entry in $oldManifest.files) {
        if ($sources.ContainsKey($entry.path)) { continue }
        $stale = [IO.Path]::GetFullPath((Join-Path $outDir $entry.path))
        if (-not (Test-NoxPathWithin $stale $outDir)) { throw "Invalid previous manifest path: $($entry.path)" }
        Assert-NoxNoReparsePoint $stale
        if (Test-Path -LiteralPath $stale -PathType Leaf) {
            if ((Get-FileHash -LiteralPath $stale -Algorithm SHA256).Hash -eq $entry.sha256) { Remove-Item -LiteralPath $stale -Force }
            else { Write-NoxWarning "Preserved edited stale file: $($entry.path)" }
        }
    }
    $entries = @($sources.Keys | Sort-Object | ForEach-Object {
        $path = Join-Path $outDir $_
        [pscustomobject]@{ path = $_; bytes = (Get-Item -LiteralPath $path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $totalBytes = ($entries | Measure-Object bytes -Sum).Sum
    $manifest = [ordered]@{ version = 1; profile = $profileData.name; configuration = $Configuration; scenes = $scenes; bytes = $totalBytes; files = $entries }
    Write-NoxUtf8 $manifestPath (($manifest | ConvertTo-Json -Depth 10) + "`n")
    Write-NoxStep ('Package ready: {0} ({1} managed files, {2:N1} MiB)' -f $outDir, $entries.Count, ($totalBytes / 1MB))
    Write-NoxProgress 1.0
}
catch { Stop-Nox $_.Exception.Message }
