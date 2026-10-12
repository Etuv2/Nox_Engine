<#
.SYNOPSIS
Stages a standalone package from a profile and its transitive runtime dependencies.
.DESCRIPTION
Default layout "packed": runtime content is written as two read-only ZIP packs, engine.pak (engine-owned
content listed in packaging/engine_owned.json) and game.pak (selected project content and per-distribution
config overrides), plus nox_bootstrap.json (contract C2). Packs are built directly from source files and
the content-addressed cache under build/cook-cache; unchanged inputs are not cooked and unchanged packs are
not rewritten. Layout "loose" keeps the earlier copy-tree output for development comparisons.
Reruns remove unchanged files owned by the previous manifest; unmanaged or edited files are preserved.
.EXAMPLE
tools\package.cmd -SkipBuild
tools\package.cmd -Profile engine -SkipBuild
tools\package.cmd -Layout loose -SkipBuild -Output Package/NOX_Racing_loose
tools\package.cmd -GameScene scenes/my_game.json -Plan
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Output = '',
    [string]$Profile = 'nox-racing',
    [ValidateSet('packed', 'loose')]
    [string]$Layout = 'packed',
    [switch]$SkipBuild,
    # Do not run nox_cook: procedural nodes then rely on cooked results already present (fixtures, tooling).
    [switch]$SkipCook,
    [string]$GameScene = '',
    [switch]$Plan
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AssetPackaging.ps1')
Add-Type -Path (Join-Path $PSScriptRoot 'NoxPackWriter.cs') -ReferencedAssemblies 'System.IO.Compression'
$repo = Get-NoxRepoRoot

try {
    $profilePath = if (Test-Path -LiteralPath $Profile -PathType Leaf) { [IO.Path]::GetFullPath($Profile) } else {
        Join-Path $repo "packaging/$Profile.json"
    }
    $profileData = Read-NoxAssetJson $profilePath
    $scenes = @($profileData.scenes)
    if ($GameScene) { $scenes = @($GameScene.Replace('\', '/')) }
    if (-not $scenes.Count -or $scenes[0] -notmatch '^scenes/.+\.json$') { throw 'Profile needs a main scene under scenes/.' }
    $project = if ($profileData.PSObject.Properties['project']) { [string]$profileData.project } else { '' }
    $include = @($profileData.include | Where-Object { $_ })
    if ($project -and $project -notmatch '^projects/[^/]+/project\.json$') { throw "Profile project must be projects/<name>/project.json: $project" }
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

    # Ownership: engine.pak owns the shared list; profiles may not name owned paths (one owner per file).
    $engineOwned = Get-NoxEngineOwnedPaths $repo
    foreach ($named in @($scenes) + @($include) + @($project)) {
        if ($named -and (@($engineOwned | Where-Object { Test-NoxEngineOwned $named @($_) }).Count)) {
            throw "Profile names engine-owned path '$named'; engine.pak owns it (packaging/engine_owned.json)."
        }
    }
    $rules = Get-NoxDependencyRules $repo
    $engineSeed = Get-NoxPackageAssets -Root $repo -Include $engineOwned -Rules $rules
    $gameAssets = Get-NoxPackageAssets -Root $repo -Scenes $scenes -Include $include -Project $project -JsonOverrides $overrides -Rules $rules

    foreach ($warning in @($engineSeed.Warnings) + @($gameAssets.Warnings)) { Write-NoxWarning $warning }
    $assetFiles = @{}
    $assetReasons = @{}
    $assetPack = @{}
    foreach ($closure in @($engineSeed, $gameAssets)) {
        foreach ($rel in $closure.Files.Keys) {
            $assetFiles[$rel] = $closure.Files[$rel]
            $assetReasons[$rel] = $closure.Reasons[$rel]
            $assetPack[$rel] = if (Test-NoxEngineOwned $rel $engineOwned) { 'engine.pak' } else { 'game.pak' }
        }
    }
    $assetBytes = ($assetFiles.Values | ForEach-Object { (Get-Item -LiteralPath $_).Length } | Measure-Object -Sum).Sum
    $enginePackCount = @($assetPack.Values | Where-Object { $_ -eq 'engine.pak' }).Count
    Write-NoxStep ('Selected {0} runtime files ({1:N1} MiB) for {2}: engine.pak {3}, game.pak {4}' -f $assetFiles.Count, ($assetBytes / 1MB), $profileData.name, $enginePackCount, ($assetFiles.Count - $enginePackCount))
    if ($Plan) {
        foreach ($rel in ($assetFiles.Keys | Sort-Object -CaseSensitive)) {
            $why = $assetReasons[$rel]
            if ($overrides.ContainsKey($rel)) { $why = "$why; package override" }
            Write-NoxStep "$rel [$($assetPack[$rel])] ($why)"
        }
        exit 0
    }

    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot 'Build.ps1') -Configuration $Configuration
        if ($LASTEXITCODE -ne 0) { throw 'Build failed; package was not created.' }
    }
    $buildOut = Join-Path $repo "build/vs2022-x64/$Configuration"
    $exeSrc = Join-Path $buildOut 'Nox_Engine.exe'
    if (-not (Test-Path -LiteralPath $exeSrc)) { throw "Build executable missing: $exeSrc" }

    # Cooked procedural results (contract C10): nox_cook keeps every valid result (same key) and writes the
    # others under generated/cooked/<generator>-<key>/. Those directories are game content and join game.pak.
    # Only scenes with procedural nodes need the cook tool (fixtures and plain scenes do not).
    $needsCook = @($scenes | Where-Object { [IO.File]::ReadAllText((Join-Path $repo $_)).Contains('"procedural"') }).Count -gt 0
    if ($profileData.game -and $needsCook -and -not $SkipCook) {
        $cookExe = Join-Path $buildOut 'nox_cook.exe'
        if (-not (Test-Path -LiteralPath $cookExe)) { throw "Build executable missing: $cookExe" }
        $cookManifest = Join-Path $repo 'build/cook-cache/cooked-manifest.json'
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cookManifest) | Out-Null
        if (Test-Path -LiteralPath $cookManifest) { Remove-Item -LiteralPath $cookManifest -Force }
        $sceneArgs = @()
        foreach ($scene in $scenes) { $sceneArgs += @('--scene', $scene) }
        & $cookExe --repo $repo --manifest $cookManifest @sceneArgs
        if ($LASTEXITCODE -ne 0) { throw 'nox_cook failed; package was not created.' }
        $cooked = Read-NoxAssetJson $cookManifest
        $cookedFileCount = 0
        foreach ($result in @($cooked.results)) {
            $dirRel = [string]$result.dir
            if ($dirRel -notmatch '^generated/cooked/[^/]+$') { throw "Unexpected cooked directory: $dirRel" }
            $dirFull = Join-Path $repo ($dirRel -replace '/', [IO.Path]::DirectorySeparatorChar)
            foreach ($file in Get-ChildItem -LiteralPath $dirFull -Recurse -File | Sort-Object FullName) {
                Assert-NoxNoReparsePoint $file.FullName
                $rel = $file.FullName.Substring($repo.TrimEnd([IO.Path]::DirectorySeparatorChar, '/').Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/')
                if ($assetFiles.ContainsKey($rel)) { throw "Cooked file is also selected as source content: $rel" }
                $assetFiles[$rel] = $file.FullName
                $assetReasons[$rel] = 'cooked ' + [string]$result.generator + ' result ' + ([string]$result.key).Substring(0, 12)
                $assetPack[$rel] = 'game.pak'
                $cookedFileCount++
            }
        }
        Write-NoxStep ('Cooked results: {0} ({1} files) from {2}' -f @($cooked.results).Count, $cookedFileCount, ($scenes -join ', '))
    }

    # Loose files: executable, runtime DLLs, VC runtime, licenses. Assets join them only in loose layout.
    $sources = @{}
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
    if ($Layout -eq 'loose') {
        foreach ($rel in $assetFiles.Keys) { $sources[$rel] = $assetFiles[$rel] }
    }
    $packed = $Layout -eq 'packed'
    $packNames = @()
    if ($packed) { $packNames = @('engine.pak', 'game.pak', 'nox_bootstrap.json') }
    $launcherNames = @('Play.cmd', 'Editor.cmd')
    # Everything this run writes; the cleanup below never deletes these.
    $keep = @{}
    foreach ($key in @($sources.Keys) + $packNames + $launcherNames) { $keep[$key] = $true }

    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $manifestPath = Join-Path $outDir 'package-manifest.json'
    $oldManifest = if (Test-Path -LiteralPath $manifestPath) { Read-NoxAssetJson $manifestPath } else { $null }
    if (-not $oldManifest -and @(Get-ChildItem -LiteralPath $outDir -Force).Count) {
        # Migrate old packages conservatively: only source-identical asset copies can be owned.
        $legacy = @()
        foreach ($file in Get-ChildItem -LiteralPath $outDir -Recurse -File) {
            Assert-NoxNoReparsePoint $file.FullName
            $rel = $file.FullName.Substring($outDir.TrimEnd('\', '/').Length + 1).Replace('\', '/')
            if ($rel.Split('/')[0] -notin $script:NoxAssetRoots -or $keep.ContainsKey($rel)) { continue }
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
    $packInfo = @()
    $contentVersion = ''
    if ($packed) {
        $cacheDir = Join-Path $repo 'build/cook-cache'
        $storedExtensions = Get-NoxStoredExtensions $repo
        $inputs = @{ 'engine.pak' = New-Object 'System.Collections.Generic.List[NoxPackInput]'; 'game.pak' = New-Object 'System.Collections.Generic.List[NoxPackInput]' }
        foreach ($rel in ($assetFiles.Keys | Sort-Object -CaseSensitive)) {
            $input = New-Object NoxPackInput
            $input.Name = $rel
            if ($overrides.ContainsKey($rel)) {
                $input.Bytes = (New-Object Text.UTF8Encoding($false)).GetBytes((($overrides[$rel] | ConvertTo-Json -Depth 100) + "`n"))
            }
            else { $input.Path = $assetFiles[$rel] }
            $input.ForceStored = [IO.Path]::GetExtension($rel).ToLowerInvariant() -in $storedExtensions
            $inputs[$assetPack[$rel]].Add($input)
        }
        # Packs are written from selected sources. Ownership is disjoint; the bootstrap lists game.pak first.
        $indexHashes = @()
        foreach ($packName in @('engine.pak', 'game.pak')) {
            $pakPath = Join-Path $outDir $packName
            if ($inputs[$packName].Count -eq 0) {
                if (Test-Path -LiteralPath $pakPath) { Remove-Item -LiteralPath $pakPath -Force }
                continue
            }
            $result = [NoxPack]::Build($pakPath, $cacheDir, 'win-x64', $inputs[$packName])
            $indexHashes += $result.IndexSha256
            Write-NoxStep ('{0}: {1} entries, {2:N1} MiB, cache {3} hit / {4} cooked, {5}' -f $packName, $result.Entries.Count, ($result.FileBytes / 1MB), $result.CacheHits, $result.Cooked, $(if ($result.Rewritten) { 'written' } else { 'unchanged' }))
            foreach ($entry in @($result.Entries | Where-Object { -not $_.CacheHit })) { Write-NoxStep "  cooked: $($entry.Name)" }
            if ($result.Recovered) { Write-NoxWarning "${packName}: discarded $($result.Recovered) corrupt cache entries and rebuilt them." }
            $entryInfo = @($result.Entries | ForEach-Object {
                [ordered]@{ path = $_.Name; bytes = $_.Size; stored_bytes = $_.CompressedSize; method = $_.Method; sha256 = $_.Sha256; reason = $assetReasons[$_.Name] }
            })
            $packInfo += [ordered]@{ name = $packName; index_sha256 = $result.IndexSha256; entries = $entryInfo }
        }
        $contentVersion = [NoxPack]::Sha256Text(($indexHashes -join "`n"))
        $bootstrap = [ordered]@{
            format = 1
            project = $profileData.name
            descriptor = $project
            mode = 'packed'
            packs = @('game.pak', 'engine.pak')
            content_version = $contentVersion
            user_dir = 'NOX/' + $profileData.name
            reload = $false
        }
        $bootstrapText = (($bootstrap | ConvertTo-Json -Depth 10) + "`n")
        Write-NoxUtf8IfChanged (Join-Path $outDir 'nox_bootstrap.json') $bootstrapText | Out-Null
    }

    foreach ($rel in ($sources.Keys | Sort-Object -CaseSensitive)) {
        $dest = [IO.Path]::GetFullPath((Join-Path $outDir $rel))
        if (-not (Test-NoxPathWithin $dest $outDir)) { throw "Destination escapes package: $rel" }
        Assert-NoxNoReparsePoint $dest
        if ($null -eq $sources[$rel]) { continue }
        Copy-NoxFileIfChanged $sources[$rel] $dest | Out-Null
        if ($Layout -eq 'loose' -and $overrides.ContainsKey($rel)) {
            Write-NoxUtf8IfChanged $dest (($overrides[$rel] | ConvertTo-Json -Depth 100) + "`n") | Out-Null
        }
    }
    $noGame = if ($profileData.game) { '' } else { ' --no-game' }
    foreach ($entry in @(@{ Name = 'Play.cmd'; Mode = 'game' }, @{ Name = 'Editor.cmd'; Mode = 'editor' })) {
        $text = '@echo off' + "`r`n" + 'start "" /D "%~dp0" "%~dp0Nox_Engine.exe" --preset ' + $entry.Mode + $noGame + " %*`r`n"
        Write-NoxUtf8IfChanged (Join-Path $outDir $entry.Name) $text | Out-Null
    }
    if ($Layout -eq 'loose') {
        Write-NoxUtf8IfChanged (Join-Path $outDir 'config/launch_presets.json') (($launch | ConvertTo-Json -Depth 100) + "`n") | Out-Null
    }

    # Delete only unchanged files owned by the previous package; keep edited configs/saves.
    foreach ($entry in $oldManifest.files) {
        if ($keep.ContainsKey($entry.path)) { continue }
        $stale = [IO.Path]::GetFullPath((Join-Path $outDir $entry.path))
        if (-not (Test-NoxPathWithin $stale $outDir)) { throw "Invalid previous manifest path: $($entry.path)" }
        Assert-NoxNoReparsePoint $stale
        if (Test-Path -LiteralPath $stale -PathType Leaf) {
            if ((Get-FileHash -LiteralPath $stale -Algorithm SHA256).Hash -eq $entry.sha256) { Remove-Item -LiteralPath $stale -Force }
            else { Write-NoxWarning "Preserved edited stale file: $($entry.path)" }
        }
    }
    $allRel = @($keep.Keys | Sort-Object -CaseSensitive | Where-Object { Test-Path -LiteralPath (Join-Path $outDir $_) -PathType Leaf })
    $entries = @($allRel | ForEach-Object {
        $path = Join-Path $outDir $_
        $fileSha = ''; $fileCrc = [uint32]0; $fileLength = [long]0
        [NoxPack]::HashFile($path, [ref]$fileSha, [ref]$fileCrc, [ref]$fileLength)
        [pscustomobject]@{ path = $_; bytes = $fileLength; sha256 = $fileSha }
    })
    $totalBytes = ($entries | Measure-Object bytes -Sum).Sum
    $manifest = [ordered]@{ version = 2; layout = $Layout; profile = $profileData.name; configuration = $Configuration; scenes = $scenes; project = $project; content_version = $contentVersion; bytes = $totalBytes; files = $entries; packs = $packInfo }
    Write-NoxUtf8IfChanged $manifestPath (($manifest | ConvertTo-Json -Depth 12) + "`n") | Out-Null
    Write-NoxStep ('Package ready: {0} ({1} managed files, {2:N1} MiB, layout {3})' -f $outDir, $entries.Count, ($totalBytes / 1MB), $Layout)
    Write-NoxProgress 1.0
}
catch { Stop-Nox $_.Exception.Message }
