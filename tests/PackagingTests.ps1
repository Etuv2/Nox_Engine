# Standard-library fixture tests. Run: powershell -NoProfile -File tests/PackagingTests.ps1
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/AssetPackaging.ps1')
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
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $testRoot 'tools/Package.ps1') -Profile fixture -SkipBuild
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

    foreach ($name in @('Common.ps1', 'AssetPackaging.ps1', 'Package.ps1', 'Clean.ps1')) {
        Put-TestFile "tools/$name" ([IO.File]::ReadAllText((Join-Path $PSScriptRoot "../tools/$name")))
    }
    Put-TestFile 'packaging/fixture.json' '{"name":"Fixture","game":false,"scenes":["scenes/main.json"],"include":["config/config.json","shaders"],"licenses":[]}'
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
    & powershell -NoProfile -File (Join-Path $testRoot 'tools/Package.ps1') -Profile fixture -SkipBuild -Output 'models/package'
    Assert-Test ($LASTEXITCODE -ne 0) 'Source-overlapping output accepted'

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
