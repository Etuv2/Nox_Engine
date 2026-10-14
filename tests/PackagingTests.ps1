# Standard-library fixture tests. Run: powershell -NoProfile -File tests/PackagingTests.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/AssetPackaging.ps1')
Add-Type -Path (Join-Path $PSScriptRoot '../tools/NoxPackWriter.cs') -ReferencedAssemblies 'System.IO.Compression'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('nox-packaging-' + [Guid]::NewGuid().ToString('N'))
$testRoot = [IO.Path]::GetFullPath($testRoot)
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Assert-Test {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}
function Put-TestFile {
    param([string]$Relative, [string]$Text = 'fixture')
    $path = Join-Path $testRoot $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    Write-NoxUtf8 $path $Text
}
function Expect-AssetFailure {
    param([scriptblock]$Action, [string]$Pattern)
    $failed = $false
    try { & $Action | Out-Null }
    catch { $failed = $true; Assert-Test ($_.Exception.Message -match $Pattern) "Unexpected failure: $_" }
    Assert-Test $failed "Expected rejection matching '$Pattern'"
}
function Run-FixturePackage {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $testRoot 'tools/Package.ps1') -Profile fixture -SkipBuild -SkipCook -Layout loose
    Assert-Test ($LASTEXITCODE -eq 0) 'Fixture package failed'
}

try {
    Put-TestFile 'scenes/main.json' '{"model":"models/car.gltf","metadata":"data/nested.json","output":"generated/not-an-input","pack":"textures/pack"}'
    Put-TestFile 'models/car.gltf' '{"buffers":[{"uri":"car.bin"}],"images":[{"uri":"../textures/car%20paint.png"},{"uri":"data:image/png;base64,AA=="}]}'
    Put-TestFile 'models/car.bin'
    Put-TestFile 'textures/car paint.png'
    Put-TestFile 'data/nested.json' '{"texture":"textures/detail.png","loop":"data/nested.json"}'
    Put-TestFile 'textures/detail.png'
    Put-TestFile 'textures/pack/used.png'
    Put-TestFile 'textures/pack/src/raw.png'
    Put-TestFile 'textures/pack/model.blend'
    Put-TestFile 'textures/pack/README.md'
    $assets = Get-NoxPackageAssets -Root $testRoot -Scenes 'scenes/main.json'
    foreach ($rel in @('models/car.bin', 'textures/car paint.png', 'textures/detail.png', 'textures/pack/used.png')) {
        Assert-Test ($assets.Files.ContainsKey($rel)) "Transitive dependency missing: $rel"
    }
    Assert-Test (-not $assets.Files.ContainsKey('textures/pack/src/raw.png')) 'Raw source directory leaked'
    Assert-Test (-not $assets.Files.ContainsKey('textures/pack/model.blend')) 'Blend source leaked'
    Put-TestFile 'scenes/child.json' '{"texture":"textures/detail.png"}'
    Put-TestFile 'data/scene-link.json' '{"scene":"scenes/child.json"}'
    $linkedScene = Get-NoxPackageAssets -Root $testRoot -Include 'data/scene-link.json'
    Assert-Test ($linkedScene.Files.ContainsKey('scenes/child.json')) 'Referenced scene dependency was skipped'
    Put-TestFile 'scenes/bad.json' '{"model":"models/missing.glb"}'
    Expect-AssetFailure { Get-NoxPackageAssets -Root $testRoot -Scenes 'scenes/bad.json' } 'Missing'
    Put-TestFile 'scenes/bad.json' '{"model":"C:/outside.glb"}'
    Expect-AssetFailure { Get-NoxPackageAssets -Root $testRoot -Scenes 'scenes/bad.json' } 'Non-portable'
    # Prefab references: a scene's nodes.**.prefab, and a nested prefab inside a prefab, are packed with their content.
    Put-TestFile 'projects/fixture/prefabs/car.prefab.json' '{"type":"node","name":"Car","children":[{"type":"model","name":"Body","path":"models/car.gltf"},{"prefab":"projects/fixture/prefabs/wheel.prefab.json","name":"Wheel"}]}'
    Put-TestFile 'projects/fixture/prefabs/wheel.prefab.json' '{"type":"model","name":"Tyre","path":"textures/wheel.png"}'
    Put-TestFile 'textures/wheel.png'
    Put-TestFile 'scenes/prefab_main.json' '{"nodes":[{"prefab":"projects/fixture/prefabs/car.prefab.json","name":"PlayerCar","position":[0,0,0]}]}'
    $prefabAssets = Get-NoxPackageAssets -Root $testRoot -Scenes 'scenes/prefab_main.json'
    foreach ($rel in @('projects/fixture/prefabs/car.prefab.json', 'projects/fixture/prefabs/wheel.prefab.json', 'textures/wheel.png', 'models/car.gltf')) {
        Assert-Test ($prefabAssets.Files.ContainsKey($rel)) "Prefab dependency missing: $rel"
    }
    # A procedural node inside a prefab packs the input folders its generator declares, as a scene node does.
    Put-TestFile 'projects/fixture/prefabs/street.prefab.json' '{"type":"model","name":"StreetKit","procedural":{"generator":"street_kit","params":{"seed":1,"output":"generated/fixture_street","texture_root":"textures/fixture_root"}}}'
    Put-TestFile 'textures/fixture_root/tile.png'
    Put-TestFile 'scenes/prefab_street.json' '{"nodes":[{"prefab":"projects/fixture/prefabs/street.prefab.json","name":"StreetKit","position":[0,0,0]}]}'
    $streetAssets = Get-NoxPackageAssets -Root $testRoot -Scenes 'scenes/prefab_street.json'
    Assert-Test ($streetAssets.Files.ContainsKey('textures/fixture_root/tile.png')) 'Procedural texture_root inside a prefab was not packed'
    Put-TestFile 'models/bad.gltf' '{"buffers":[{"uri":"../../escape.bin"}]}'
    Expect-AssetFailure { Get-NoxPackageAssets -Root $testRoot -Include 'models/bad.gltf' } 'out-of-tree'
    Expect-AssetFailure { Get-NoxPackageAssets -Root $testRoot -Include '../escape.bin' } 'escapes repository'

    # GLB can also contain external images. Verify the JSON chunk reader follows them.
    $chunk = [Text.Encoding]::UTF8.GetBytes('{"images":[{"uri":"../textures/detail.png"}]} ')
    while ($chunk.Length % 4) { $chunk += [byte]32 }
    $writer = New-Object IO.BinaryWriter([IO.File]::Create((Join-Path $testRoot 'models/external.glb')))
    try {
        $writer.Write([uint32]0x46546C67); $writer.Write([uint32]2); $writer.Write([uint32](20 + $chunk.Length))
        $writer.Write([uint32]$chunk.Length); $writer.Write([uint32]0x4E4F534A); $writer.Write([byte[]]$chunk)
    }
    finally { $writer.Dispose() }
    $glbAssets = Get-NoxPackageAssets -Root $testRoot -Include 'models/external.glb'
    Assert-Test ($glbAssets.Files.ContainsKey('textures/detail.png')) 'GLB external image missing'

    foreach ($name in @('Common.ps1', 'AssetPackaging.ps1', 'Package.ps1', 'Clean.ps1', 'NoxPackWriter.cs')) {
        Put-TestFile "tools/$name" ([IO.File]::ReadAllText((Join-Path $PSScriptRoot "../tools/$name")))
    }
    Put-TestFile 'packaging/fixture.json' '{"name":"Fixture","game":false,"scenes":["scenes/main.json"],"include":["config/config.json"],"licenses":[]}'
    Put-TestFile 'packaging/engine_owned.json' '{"format":1,"paths":["shaders"]}'
    Put-TestFile 'config/config.json' '{"first_scene":"scenes/unrelated.json","scenes":[{"path":"scenes/unrelated.json"}]}'
    Put-TestFile 'config/launch_presets.json' '{"default":"editor","presets":{"game":{"scene":"scenes/unrelated.json","mode":"game"},"editor":{"scene":"scenes/unrelated.json","mode":"editor"}}}'
    Put-TestFile 'shaders/shader.glsl'
    Put-TestFile 'build/vs2022-x64/Release/Nox_Engine.exe'
    foreach ($dll in @('SDL2.dll', 'SDL2_image.dll', 'SDL2_mixer.dll', 'SDL2_ttf.dll', 'glew32.dll', 'assimp-vc143-mt.dll')) { Put-TestFile $dll }
    Put-TestFile 'textures/legacy.png'
    Put-TestFile 'Package/Fixture/textures/legacy.png'
    Put-TestFile 'Package/Fixture/save.json' '{"user":true}'
    Run-FixturePackage
    $out = Join-Path $testRoot 'Package/Fixture'
    $manifest = Read-NoxAssetJson (Join-Path $out 'package-manifest.json')
    Assert-Test ($manifest.files.path -contains 'models/car.bin') 'Manifest omitted glTF buffer'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $out 'textures/legacy.png'))) 'Source-identical legacy package asset survived'
    Assert-Test (Test-Path -LiteralPath (Join-Path $out 'save.json')) 'Legacy package save deleted'
    Assert-Test ((Read-NoxAssetJson (Join-Path $out 'config/config.json')).first_scene -eq 'scenes/main.json') 'Packaged config scene was not updated'
    Assert-Test ([IO.File]::ReadAllText((Join-Path $out 'Play.cmd')).Contains('--no-game')) 'Engine launcher registered racing module'
    Put-TestFile 'Package/Fixture/save.json' '{"user":true}'
    Put-TestFile 'scenes/main.json' '{"model":"models/car.gltf"}'
    Run-FixturePackage
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $out 'textures/detail.png'))) 'Stale managed dependency survived'
    Assert-Test (Test-Path -LiteralPath (Join-Path $out 'save.json')) 'Unmanaged runtime save was deleted'
    Put-TestFile 'scenes/main.json' '{"model":"models/car.gltf","detail":"textures/detail.png"}'
    Run-FixturePackage
    Put-TestFile 'Package/Fixture/textures/detail.png' 'user-edited'
    Put-TestFile 'scenes/main.json' '{"model":"models/car.gltf"}'
    Run-FixturePackage
    Assert-Test ([IO.File]::ReadAllText((Join-Path $out 'textures/detail.png')) -eq 'user-edited') 'Edited stale file was deleted'
    & powershell -NoProfile -File (Join-Path $testRoot 'tools/Package.ps1') -Profile fixture -SkipBuild -SkipCook -Output 'models/package'
    Assert-Test ($LASTEXITCODE -ne 0) 'Source-overlapping output accepted'

    # ---- Packed layout: dependency graph, deterministic packs, cache, manifest ownership ----
    $packRoot = Join-Path $testRoot 'pack-fixture'
    New-Item -ItemType Directory -Path $packRoot | Out-Null
    Copy-Item -Path (Join-Path $PSScriptRoot 'fixtures/packaging/tree/*') -Destination $packRoot -Recurse -Force
    New-Item -ItemType Directory -Path (Join-Path $packRoot 'tools') -Force | Out-Null
    foreach ($name in @('Common.ps1', 'AssetPackaging.ps1', 'Package.ps1', 'NoxPackWriter.cs')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../tools/$name") -Destination (Join-Path $packRoot "tools/$name") -Force
    }
    New-Item -ItemType Directory -Path (Join-Path $packRoot 'build/vs2022-x64/Release') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $packRoot 'build/vs2022-x64/Release/Nox_Engine.exe'), 'fake exe')
    foreach ($dll in @('SDL2.dll', 'SDL2_image.dll', 'SDL2_mixer.dll', 'SDL2_ttf.dll', 'glew32.dll', 'assimp-vc143-mt.dll')) {
        [IO.File]::WriteAllText((Join-Path $packRoot "build/vs2022-x64/Release/$dll"), 'fake dll')
    }
    function Invoke-FixturePack {
        param([string]$Output, [string]$Profile = 'fixture', [string[]]$Extra = @())
        $log = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $packRoot 'tools/Package.ps1') -Profile $Profile -SkipBuild -SkipCook -Output $Output @Extra 2>&1 | Out-String
        return [pscustomobject]@{ Code = $LASTEXITCODE; Log = $log; Dir = (Join-Path $packRoot $Output) }
    }
    function Get-ZipNames {
        param([string]$Path)
        $zip = [IO.Compression.ZipFile]::OpenRead($Path)
        try { return [string[]]@($zip.Entries | ForEach-Object { $_.FullName }) } finally { $zip.Dispose() }
    }
    function Get-Sha256Hex {
        param([string]$Path)
        $hash = $null; $crc = [uint32]0; $length = [long]0
        [NoxPack]::HashFile($Path, [ref]$hash, [ref]$crc, [ref]$length)
        return $hash
    }

    # Closure: registered config keys, shader includes, Lua roots, project assets and scene references.
    $rules = Get-NoxDependencyRules $packRoot
    $packagedConfig = Read-NoxAssetJson (Join-Path $packRoot 'config/config.json')
    $packagedConfig.first_scene = 'scenes/main.json'
    $packagedConfig.scenes = @([pscustomobject]@{ path = 'scenes/main.json' })
    $closure = Get-NoxPackageAssets -Root $packRoot -Scenes @('scenes/main.json') -Include @('config/config.json') -Project 'projects/fixture/project.json' -JsonOverrides @{ 'config/config.json' = $packagedConfig } -Rules $rules
    foreach ($rel in @('scenes/main.json', 'models/car.gltf', 'models/car.bin', 'textures/car paint.png', 'textures/ui/icon.png',
        'projects/fixture/project.json', 'projects/fixture/scripts/main.lua',
        'projects/fixture/scripts/vehicle/nitrous.lua', 'projects/fixture/scripts/unused.lua', 'projects/fixture/data/tuning.json', 'images/logo.png')) {
        Assert-Test ($closure.Files.ContainsKey($rel)) "Closure missing $rel"
    }
    foreach ($rel in @('hdrs/unused.hdr', 'projects/other/project.json', 'projects/other/scripts/main.lua', 'captures/run.png', 'tests/unrelated.txt', 'models/src/raw.blend', 'shaders/main.glsl', 'scenes/unrelated.json')) {
        Assert-Test (-not $closure.Files.ContainsKey($rel)) "Closure wrongly includes $rel"
    }
    $shaderClosure = Get-NoxPackageAssets -Root $packRoot -Include @('shaders/main.glsl') -Rules $rules
    Assert-Test ($shaderClosure.Files.ContainsKey('shaders/includes/common.glsl') -and $shaderClosure.Files.ContainsKey('shaders/includes/deep.glsl')) 'Shader include closure is incomplete'
    Assert-Test ($shaderClosure.Reasons['shaders/includes/deep.glsl'] -match 'shader #include') 'Shader include reason missing'
    Assert-Test ($closure.Reasons['models/car.bin'] -match 'models/car.gltf') 'glTF buffer reason missing'
    Assert-Test ($closure.Reasons['projects/fixture/scripts/unused.lua'] -match 'Lua script root') 'Lua root reason missing'
    Assert-Test ($closure.Files.ContainsKey('textures/fixture_cc0/a.png')) 'Generator input directory missing from the closure'
    Assert-Test (@($closure.Warnings | Where-Object { $_ -match 'unclaimed path-like value .textures/loose/unknown.png.' }).Count -eq 1) 'Unclaimed scene value was not reported'
    Assert-Test (-not $closure.Files.ContainsKey('textures/loose/unknown.png')) 'Unclaimed scene value was packed'

    # Unsafe, missing and case-mismatched references fail the graph before anything is written.
    $badDir = Join-Path $packRoot 'projects/bad'
    New-Item -ItemType Directory -Path (Join-Path $badDir 'scripts') -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $badDir 'scripts/main.lua'), 'return 1')
    foreach ($case in @(@('../outside.png', 'escapes repository'), @('textures/UI/icon.png', 'Case mismatch'))) {
        $descriptor = '{"format":1,"name":"bad","runtime":"lua","api_version":1,"entry":"scripts/main.lua","script_roots":["scripts"],"data_roots":[],"assets":["ASSET"],"scenes":[],"native_extensions":[]}'
        [IO.File]::WriteAllText((Join-Path $badDir 'project.json'), $descriptor.Replace('ASSET', $case[0]))
        Expect-AssetFailure { Get-NoxPackageAssets -Root $packRoot -Project 'projects/bad/project.json' } $case[1]
    }
    [IO.File]::WriteAllText((Join-Path $packRoot 'shaders/broken.glsl'), "#include ""missing_part.glsl""`n")
    Expect-AssetFailure { Get-NoxPackageAssets -Root $packRoot -Include 'shaders/broken.glsl' } 'Missing shader include'
    Remove-Item -LiteralPath (Join-Path $packRoot 'shaders/broken.glsl') -Force
    Expect-AssetFailure { [NoxPack]::ValidateName('a/../b') } 'relative segment'
    Expect-AssetFailure { [NoxPack]::ValidateName('/abs.png') } 'Invalid pack entry name'
    Expect-AssetFailure { [NoxPack]::ValidateName('win\path.png') } 'not portable'
    $collide = New-Object 'System.Collections.Generic.List[NoxPackInput]'
    foreach ($name in @('a/B.txt', 'a/b.txt')) {
        $in = New-Object NoxPackInput; $in.Name = $name; $in.Bytes = [Text.Encoding]::ASCII.GetBytes('x'); $collide.Add($in)
    }
    Expect-AssetFailure { [NoxPack]::Build((Join-Path $packRoot 'collide.pak'), (Join-Path $packRoot 'build/cook-cache'), 'win-x64', $collide) } 'case-colliding'

    # Ownership: a profile may not name an engine-owned path; references from game content resolve into engine.pak.
    [IO.File]::WriteAllText((Join-Path $packRoot 'packaging/owner-clash.json'), '{"name":"Clash","game":false,"scenes":["scenes/main.json"],"include":["shaders"],"licenses":[]}')
    $clash = Invoke-FixturePack -Output 'Package/Clash' -Profile (Join-Path $packRoot 'packaging/owner-clash.json')
    Assert-Test ($clash.Code -ne 0 -and $clash.Log -match 'engine-owned') 'Profile naming an engine-owned path was accepted'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $packRoot 'Package/Clash'))) 'Ownership error modified the output'

    # Packed build: structure, bootstrap, pack contents, ordering, timestamps and hashes.
    $a = Invoke-FixturePack -Output 'Package/FixtureA'
    Assert-Test ($a.Code -eq 0) "Packed fixture build failed: $($a.Log)"
    foreach ($leaf in @('engine.pak', 'game.pak', 'nox_bootstrap.json', 'package-manifest.json', 'Play.cmd', 'Editor.cmd', 'Nox_Engine.exe')) {
        Assert-Test (Test-Path -LiteralPath (Join-Path $a.Dir $leaf)) "Packed output missing $leaf"
    }
    foreach ($loose in @('shaders', 'scenes', 'models', 'textures', 'projects', 'hdrs', 'config')) {
        Assert-Test (-not (Test-Path -LiteralPath (Join-Path $a.Dir $loose))) "Packed output contains loose asset root $loose"
    }
    $boot = Read-NoxAssetJson (Join-Path $a.Dir 'nox_bootstrap.json')
    Assert-Test ($boot.mode -eq 'packed' -and $boot.format -eq 1 -and $boot.descriptor -eq 'projects/fixture/project.json') 'Bootstrap fields wrong'
    $manifest = Read-NoxAssetJson (Join-Path $a.Dir 'package-manifest.json')
    $indexText = (@($manifest.packs | ForEach-Object { $_.index_sha256 })) -join "`n"
    Assert-Test ($boot.content_version -eq [NoxPack]::Sha256Text($indexText) -and $manifest.content_version -eq $boot.content_version) 'content_version is not the pack index hash'
    $engineNames = Get-ZipNames (Join-Path $a.Dir 'engine.pak')
    $gameNames = Get-ZipNames (Join-Path $a.Dir 'game.pak')
    Assert-Test (@($engineNames | Where-Object { $gameNames -contains $_ }).Count -eq 0) 'A file is in both packs'
    Assert-Test (($engineNames -contains 'shaders/includes/deep.glsl') -and ($engineNames -contains 'images/logo.png')) 'engine.pak missing engine-owned content'
    Assert-Test (($gameNames -contains 'models/car.bin') -and ($gameNames -contains 'projects/fixture/scripts/unused.lua') -and ($gameNames -contains 'projects/fixture/data/tuning.json')) 'game.pak missing project content'
    foreach ($bad in @('hdrs/unused.hdr', 'projects/other/scripts/main.lua', 'captures/run.png', 'tests/unrelated.txt', 'models/src/raw.blend')) {
        Assert-Test (-not (($engineNames -contains $bad) -or ($gameNames -contains $bad))) "Pack contains excluded $bad"
    }
    foreach ($names in @($engineNames, $gameNames)) {
        $sorted = [string[]]$names.Clone(); [Array]::Sort($sorted, [StringComparer]::Ordinal)
        Assert-Test (($names -join '|') -eq ($sorted -join '|')) 'Pack entries are not ordinal-sorted'
    }
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $a.Dir 'game.pak'))
    try {
        $epoch = New-Object DateTime 1980, 1, 1, 0, 0, 0
        foreach ($entry in $zip.Entries) {
            Assert-Test ($entry.LastWriteTime.DateTime -eq $epoch) "Entry timestamp is not 1980-01-01: $($entry.FullName)"
        }
        $reader = New-Object IO.StreamReader($zip.GetEntry('config/config.json').Open())
        try { $cfgText = $reader.ReadToEnd() } finally { $reader.Dispose() }
        Assert-Test (($cfgText | ConvertFrom-Json).first_scene -eq 'scenes/main.json') 'Packed config override was not applied'
    }
    finally { $zip.Dispose() }
    $itemHash = @{}
    foreach ($pack in $manifest.packs) {
        foreach ($item in $pack.entries) { $itemHash[$item.path] = $item.sha256 }
    }
    Assert-Test ($itemHash['projects/fixture/data/tuning.json'] -eq (Get-Sha256Hex (Join-Path $packRoot 'projects/fixture/data/tuning.json'))) 'Manifest hash differs from source'
    Assert-Test (@($manifest.packs | ForEach-Object { $_.entries } | Where-Object { $_.method -eq 8 }).Count -gt 0) 'No entry was deflated'
    Assert-Test (@($manifest.packs | ForEach-Object { $_.entries } | Where-Object { $_.method -eq 0 }).Count -gt 0) 'No entry was stored'

    # Reproducible: a second output folder reuses the cache and writes identical packs.
    $b = Invoke-FixturePack -Output 'Package/FixtureB'
    Assert-Test ($b.Code -eq 0 -and $b.Log -match 'engine\.pak: \d+ entries, [^\r\n]*0 cooked' -and $b.Log -match 'game\.pak: \d+ entries, [^\r\n]*0 cooked') 'Second output folder cooked entries that the cache holds'
    Assert-Test ((Get-Sha256Hex (Join-Path $a.Dir 'engine.pak')) -eq (Get-Sha256Hex (Join-Path $b.Dir 'engine.pak'))) 'engine.pak is not byte-identical across runs'
    Assert-Test ((Get-Sha256Hex (Join-Path $a.Dir 'game.pak')) -eq (Get-Sha256Hex (Join-Path $b.Dir 'game.pak'))) 'game.pak is not byte-identical across runs'

    # No-op rerun: nothing cooked, packs and manifest keep their timestamps.
    $stampFiles = @('engine.pak', 'game.pak', 'package-manifest.json', 'nox_bootstrap.json')
    $stampBefore = @($stampFiles | ForEach-Object { (Get-Item -LiteralPath (Join-Path $a.Dir $_)).LastWriteTimeUtc })
    Start-Sleep -Milliseconds 1200
    $a2 = Invoke-FixturePack -Output 'Package/FixtureA'
    Assert-Test ($a2.Code -eq 0 -and $a2.Log -match 'engine\.pak: [^\r\n]*0 cooked, unchanged' -and $a2.Log -match 'game\.pak: [^\r\n]*0 cooked, unchanged') 'No-op rerun cooked or rewrote a pack'
    $stampAfter = @($stampFiles | ForEach-Object { (Get-Item -LiteralPath (Join-Path $a.Dir $_)).LastWriteTimeUtc })
    for ($i = 0; $i -lt $stampBefore.Count; $i++) {
        Assert-Test ($stampBefore[$i] -eq $stampAfter[$i]) "No-op rerun touched $($stampFiles[$i])"
    }

    # Corrupt cache entries (payload bytes, unreadable metadata) are discarded and rebuilt.
    $cacheFiles = @(Get-ChildItem -LiteralPath (Join-Path $packRoot 'build/cook-cache') -File)
    $payload = $cacheFiles | Where-Object { $_.Extension -eq '.bin' } | Select-Object -First 1
    $meta = $cacheFiles | Where-Object { $_.Extension -eq '.meta' } | Select-Object -First 1
    $bytes = [IO.File]::ReadAllBytes($payload.FullName)
    $bytes[0] = [byte]($bytes[0] -bxor 0xFF)
    [IO.File]::WriteAllBytes($payload.FullName, $bytes)
    [IO.File]::WriteAllText($meta.FullName, 'not metadata')
    $c = Invoke-FixturePack -Output 'Package/FixtureC'
    Assert-Test ($c.Code -eq 0 -and $c.Log -match 'discarded') 'Corrupt cache entries were not reported'
    Assert-Test ((Get-Sha256Hex (Join-Path $a.Dir 'game.pak')) -eq (Get-Sha256Hex (Join-Path $c.Dir 'game.pak'))) 'Recovered pack differs from the clean build'

    # Changed inputs rewrite the pack from scratch (no appended archive); a fresh build matches it.
    $before = (Get-Item -LiteralPath (Join-Path $a.Dir 'game.pak')).Length
    [IO.File]::WriteAllText((Join-Path $packRoot 'models/car.gltf'), '{"buffers":[{"uri":"car.bin"}],"images":[]}')
    $d = Invoke-FixturePack -Output 'Package/FixtureA'
    Assert-Test ($d.Code -eq 0) 'Dependency-removal rebuild failed'
    $after = (Get-Item -LiteralPath (Join-Path $a.Dir 'game.pak')).Length
    Assert-Test ($after -lt $before) 'Removing a texture dependency did not shrink game.pak'
    Assert-Test (-not ((Get-ZipNames (Join-Path $a.Dir 'game.pak')) -contains 'textures/car paint.png')) 'Removed dependency is still packed'
    $fresh = Invoke-FixturePack -Output 'Package/FixtureE'
    Assert-Test ((Get-Sha256Hex (Join-Path $a.Dir 'game.pak')) -eq (Get-Sha256Hex (Join-Path $fresh.Dir 'game.pak'))) 'Changed-input pack differs from a fresh build'

    # Unmanaged files survive; previously managed loose files are removed only when unchanged.
    [IO.File]::WriteAllText((Join-Path $a.Dir 'save.json'), '{"user":true}')
    $loose = Invoke-FixturePack -Output 'Package/FixtureL' -Extra @('-Layout', 'loose')
    Assert-Test ($loose.Code -eq 0 -and (Test-Path -LiteralPath (Join-Path $loose.Dir 'models/car.gltf'))) 'Loose layout did not write assets'
    [IO.File]::WriteAllText((Join-Path $loose.Dir 'shaders/main.glsl'), '// user edit')
    $back = Invoke-FixturePack -Output 'Package/FixtureL'
    Assert-Test ($back.Code -eq 0) 'Switching a loose package to packed failed'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $loose.Dir 'models/car.gltf'))) 'Unchanged managed loose asset survived the switch to packed'
    Assert-Test ((Get-Content -Raw -LiteralPath (Join-Path $loose.Dir 'shaders/main.glsl')) -match 'user edit') 'Edited managed loose file was deleted'
    Assert-Test ($back.Log -match 'Preserved edited stale file') 'Preserved file was not reported'
    Assert-Test (Test-Path -LiteralPath (Join-Path $a.Dir 'save.json')) 'Unmanaged file was deleted by repackaging'
    Assert-Test (Test-Path -LiteralPath (Join-Path $loose.Dir 'engine.pak')) 'Packed switch did not write engine.pak'
    Write-Host '[NOX] Packed-layout fixture tests passed.'

    Put-TestFile 'build/vs2022-x64/Release/textures/detail.png'
    Put-TestFile 'build/vs2022-x64/Release/textures/divergent.png' 'build-only'
    Put-TestFile 'textures/divergent.png' 'authored'
    Put-TestFile 'captures/preserved.txt'
    & powershell -NoProfile -File (Join-Path $testRoot 'tools/Clean.ps1') -Apply -WhatIf
    Assert-Test ($LASTEXITCODE -eq 0) 'Cleanup dry run failed'
    Assert-Test (Test-Path -LiteralPath (Join-Path $testRoot 'build/vs2022-x64/Release/textures/detail.png')) 'WhatIf deleted a duplicate'
    & powershell -NoProfile -File (Join-Path $testRoot 'tools/Clean.ps1') -Apply
    Assert-Test ($LASTEXITCODE -eq 0) 'Fixture cleanup failed'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $testRoot 'build/vs2022-x64/Release/textures/detail.png'))) 'Identical build copy survived'
    Assert-Test (Test-Path -LiteralPath (Join-Path $testRoot 'build/vs2022-x64/Release/textures/divergent.png')) 'Divergent build asset deleted'
    Assert-Test (Test-Path -LiteralPath (Join-Path $testRoot 'captures/preserved.txt')) 'Capture deleted'
    New-Item -ItemType Directory -Force -Path (Join-Path $testRoot 'build/visual_parity_runtime') | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $testRoot 'build/visual_parity_runtime/textures') -Target (Join-Path $testRoot 'textures') | Out-Null
    & powershell -NoProfile -File (Join-Path $testRoot 'tools/Clean.ps1') -Apply
    Assert-Test ($LASTEXITCODE -eq 0) 'Junction-aware cleanup failed'
    Assert-Test (Test-Path -LiteralPath (Join-Path $testRoot 'textures/detail.png')) 'Cleanup followed a junction into source assets'
    # Remove only the fixture junction itself before recursive TEMP cleanup.
    [IO.Directory]::Delete((Join-Path $testRoot 'build/visual_parity_runtime/textures'))
    Put-TestFile '.vs/cache.bin'
    Put-TestFile 'x64/Debug/old.pdb'
    Put-TestFile 'x64/Release/snapshots/save.json' '{"user":true}'
    & powershell -NoProfile -File (Join-Path $testRoot 'tools/Clean.ps1') -Apply -IdeCache
    Assert-Test ($LASTEXITCODE -eq 0) 'IDE cache cleanup failed'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $testRoot '.vs'))) 'IDE cache survived'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $testRoot 'x64/Debug/old.pdb'))) 'Obsolete debug symbols survived'
    Assert-Test (Test-Path -LiteralPath (Join-Path $testRoot 'x64/Release/snapshots/save.json')) 'Legacy build snapshot deleted'
    Write-Host '[NOX] Packaging and cleanup fixture tests passed.'
}
finally {
    # Only the uniquely named fixture directory under TEMP may be removed.
    if (-not (Test-NoxPathWithin $testRoot ([IO.Path]::GetTempPath())) -or (Split-Path -Leaf $testRoot) -notmatch '^nox-packaging-[0-9a-f]{32}$') {
        throw "Unsafe fixture cleanup target: $testRoot"
    }
    Assert-NoxNoReparsePoint $testRoot
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
