<#
.SYNOPSIS
Creates a self-contained, playable NOX Racing folder (Nox_Engine.exe, DLLs, shaders, fonts,
config, the game scenes and only the assets they reference, licenses, Play.cmd and Editor.cmd).

.DESCRIPTION
Builds first unless -SkipBuild is given. Asset references are read from the packaged scenes and from
config/*.json (every string that points at an existing file or folder under the repo). Missing
assets that a scene needs are errors; missing config references are warnings. Re-running refreshes
changed content: folders are mirrored with robocopy /MIR, single files are copied when their size
or timestamp differs. Nothing is deleted outside the output folder.

.PARAMETER Configuration
Release (default) or Debug. The exe is taken from build/vs2022-x64/<Configuration>/.

.PARAMETER Output
Output folder. Relative paths are relative to the repo root. Default: Package/NOX_Racing.

.PARAMETER SkipBuild
Do not run Build.ps1 first; package the exe that is already built.

.EXAMPLE
tools\package.cmd
tools\package.cmd -SkipBuild -Output "$env:TEMP\nox_pkg_test"
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Output = 'Package/NOX_Racing',
    [switch]$SkipBuild,
    [string]$GameScene = 'scenes/nfs_sprint.json'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')

$repo = Get-NoxRepoRoot
$outRaw = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path $repo $Output }
$outDir = [IO.Path]::GetFullPath($outRaw).TrimEnd('\', '/')
if ($outDir -eq [IO.Path]::GetFullPath($repo).TrimEnd('\', '/') -or $outDir.Length -lt 10) {
    Stop-Nox "Refusing to package into '$outDir'. Choose a dedicated output folder."
}

# Scenes shipped with the game. -GameScene is the main game scene (default nfs_sprint.json).
$gameScene = $GameScene.replace('\', '/')
$packagedScenes = @(
    $gameScene,
    'scenes/nfs_night_street.json',
    'scenes/nfs_night_rain.json',
    'scenes/nfs_dawn_street.json'
) | Select-Object -Unique
$assetExtensions = @('.glb', '.gltf', '.bin', '.fbx', '.obj', '.hdr', '.png', '.jpg', '.jpeg', '.ttf', '.otf',
    '.wav', '.ogg', '.mp3', '.json')

Write-NoxProgress 0.0

if (-not $SkipBuild) {
    Write-NoxStep "Building $Configuration first (use -SkipBuild to package the existing exe)"
    & (Join-Path $PSScriptRoot 'Build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) {
        Stop-Nox "Build failed, package not created."
    }
}

$buildOut = Join-Path $repo "build/vs2022-x64/$Configuration"
$exeSrc = Join-Path $buildOut 'Nox_Engine.exe'
if (-not (Test-Path -LiteralPath $exeSrc)) {
    Stop-Nox "Nox_Engine.exe not found at '$exeSrc'. Build first (tools\build.cmd -Configuration $Configuration) or drop -SkipBuild."
}

# ---------------------------------------------------------------------------
# Asset discovery
# ---------------------------------------------------------------------------
$script:FilesToCopy = @{}      # repo-relative path (forward slashes) -> $true
$script:DirsToSync = @{}       # repo-relative folder -> $true
$script:Problems = New-Object System.Collections.Generic.List[string]
$script:Warnings = New-Object System.Collections.Generic.List[string]

function Get-NoxJsonStrings {
    param($Root)
    $found = New-Object System.Collections.Generic.List[string]
    $stack = New-Object System.Collections.Stack
    $stack.Push($Root)
    while ($stack.Count -gt 0) {
        $node = $stack.Pop()
        if ($null -eq $node) { continue }
        if ($node -is [string]) { $found.Add($node); continue }
        if ($node -is [System.Management.Automation.PSCustomObject]) {
            foreach ($prop in $node.PSObject.Properties) { $stack.Push($prop.Value) }
            continue
        }
        if ($node -is [System.Collections.IEnumerable]) {
            foreach ($item in $node) { $stack.Push($item) }
        }
    }
    return , $found
}

function Add-NoxReference {
    param([string]$Raw, [string]$FromFile, [bool]$Required)

    $rel = [regex]::Replace($Raw.Replace('\', '/'), '/+', '/')
    if ($rel -eq '' -or $rel -match '^(/|[A-Za-z]:|https?:)' -or $rel -match '(^|/)\.\.(/|$)') { return }
    if ($rel.StartsWith('./')) { $rel = $rel.Substring(2) }

    $full = Join-Path $repo $rel
    if (Test-Path -LiteralPath $full -PathType Container) {
        # The scenes folder is not mirrored: only the packaged scenes ship.
        if ($rel -ne 'scenes') { $script:DirsToSync[$rel] = $true }
        return
    }

    # Scenes other than the packaged set are not shipped (config/config.json lists them all).
    if ($rel.StartsWith('scenes/') -and ($packagedScenes -notcontains $rel)) { return }

    $ext = [IO.Path]::GetExtension($rel).ToLowerInvariant()
    if (-not ($assetExtensions -contains $ext -or $rel.StartsWith('scenes/'))) { return }

    if (Test-Path -LiteralPath $full -PathType Leaf) {
        $script:FilesToCopy[$rel] = $true
        return
    }
    # Missing packaged scenes are reported by the scene loop, not once per reference.
    if ($rel.StartsWith('scenes/')) { return }
    $message = "Missing asset '$rel' (referenced by $FromFile)."
    if ($Required) { $script:Problems.Add($message) } else { $script:Warnings.Add($message) }
}

function Read-NoxJson {
    param([string]$RelPath)
    $full = Join-Path $repo $RelPath
    try {
        return Get-Content -LiteralPath $full -Raw | ConvertFrom-Json
    }
    catch {
        $script:Problems.Add("Could not parse JSON '$RelPath': $($_.Exception.Message)")
        return $null
    }
}

foreach ($scene in $packagedScenes) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $scene))) {
        if ($scene -eq $gameScene) {
            $script:Problems.Add("Game scene '$gameScene' does not exist yet. It is required for the package.")
        }
        else {
            $script:Warnings.Add("Scene '$scene' not found, skipped.")
        }
        continue
    }
    $script:FilesToCopy[$scene] = $true
    $json = Read-NoxJson $scene
    if ($null -eq $json) { continue }
    foreach ($text in (Get-NoxJsonStrings $json)) {
        Add-NoxReference -Raw $text -FromFile $scene -Required $true
    }
}

foreach ($configFile in Get-ChildItem -LiteralPath (Join-Path $repo 'config') -Filter '*.json' -File) {
    $rel = 'config/' + $configFile.Name
    $json = Read-NoxJson $rel
    if ($null -eq $json) { continue }
    foreach ($text in (Get-NoxJsonStrings $json)) {
        Add-NoxReference -Raw $text -FromFile $rel -Required $false
    }
}

# Always shipped content.
foreach ($dir in @('shaders', 'fonts', 'config')) {
    if (Test-Path -LiteralPath (Join-Path $repo $dir)) {
        $script:DirsToSync[$dir] = $true
    }
    else {
        $script:Problems.Add("Required folder '$dir/' is missing from the repo.")
    }
}
# CC0 kit (git-ignored): the procedural street kit and the HDRIs read from here.
if (Test-Path -LiteralPath (Join-Path $repo 'textures/cc0')) {
    $script:DirsToSync['textures/cc0'] = $true
}
else {
    $script:Problems.Add("textures/cc0/ is missing. It is git-ignored CC0 content; see textures/cc0/SOURCES.md for the sources.")
}
if (Test-Path -LiteralPath (Join-Path $repo 'images/logo.png')) {
    $script:FilesToCopy['images/logo.png'] = $true
}
else {
    $script:Problems.Add("images/logo.png is missing (window icon).")
}
foreach ($optional in @('imgui.ini', 'imgui_layout.json')) {
    if (Test-Path -LiteralPath (Join-Path $repo $optional)) { $script:FilesToCopy[$optional] = $true }
}

foreach ($warning in $script:Warnings) { Write-NoxWarning $warning }
if ($script:Problems.Count -gt 0) {
    foreach ($problem in $script:Problems) { Write-Host "[NOX] ERROR: missing asset: $problem" }
    Stop-Nox "Package aborted: $($script:Problems.Count) problem(s) listed above."
}

Write-NoxProgress 0.3
Write-NoxStep "Packaging $Configuration to $outDir"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Count-Copy {
    param([bool]$Changed)
    if ($Changed) { $script:copiedCount++ } else { $script:unchangedCount++ }
}
$script:copiedCount = 0
$script:unchangedCount = 0

# ---------------------------------------------------------------------------
# Executable and runtime DLLs
# ---------------------------------------------------------------------------
Count-Copy (Copy-NoxFileIfChanged $exeSrc (Join-Path $outDir 'Nox_Engine.exe'))

$dllByName = @{}
foreach ($dll in Get-ChildItem -LiteralPath $repo -Filter '*.dll' -File) { $dllByName[$dll.Name] = $dll.FullName }
foreach ($dll in Get-ChildItem -LiteralPath $buildOut -Filter '*.dll' -File -ErrorAction SilentlyContinue) {
    $dllByName[$dll.Name] = $dll.FullName   # build output wins over the repo root copy
}
foreach ($name in ($dllByName.Keys | Sort-Object)) {
    Count-Copy (Copy-NoxFileIfChanged $dllByName[$name] (Join-Path $outDir $name))
}

# MSVC runtime, only if the exe links against it (CRT redistributable from the VS install).
$exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exeSrc))
if ($exeText.Contains('VCRUNTIME140')) {
    $crtDir = Get-ChildItem -Path "$env:ProgramFiles\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
        Sort-Object -Property FullName -Descending | Select-Object -First 1
    if ($crtDir) {
        foreach ($crt in Get-ChildItem -LiteralPath $crtDir.FullName -Filter '*.dll' -File |
                Where-Object { $_.Name -match '^(vcruntime140|vcruntime140_1|msvcp140|msvcp140_1|msvcp140_2|concrt140)\.dll$' }) {
            Count-Copy (Copy-NoxFileIfChanged $crt.FullName (Join-Path $outDir $crt.Name))
        }
    }
    else {
        Write-NoxWarning 'The exe needs the VC runtime but the redistributable folder was not found; the package needs VC++ 2022 installed.'
    }
}
Write-NoxProgress 0.5

# ---------------------------------------------------------------------------
# Content
# ---------------------------------------------------------------------------
foreach ($dir in ($script:DirsToSync.Keys | Sort-Object)) {
    Sync-NoxDirectory (Join-Path $repo $dir) (Join-Path $outDir ($dir -replace '/', '\'))
    Write-NoxStep "Synced folder: $dir/"
}
Write-NoxProgress 0.7

foreach ($rel in ($script:FilesToCopy.Keys | Sort-Object)) {
    $destination = Join-Path $outDir ($rel -replace '/', '\')
    Count-Copy (Copy-NoxFileIfChanged (Join-Path $repo $rel) $destination)
}

# Licenses and attribution.
$licenseDir = Join-Path $outDir 'licenses'
$licenseSources = @(
    @{ Src = 'include/IMGUI/LICENSE.txt'; Dst = 'ImGui-LICENSE.txt' },
    @{ Src = 'src/editor/solace/THIRD_PARTY.md'; Dst = 'solace-THIRD_PARTY.md' },
    @{ Src = 'textures/cc0/SOURCES.md'; Dst = 'textures-cc0-SOURCES.md' }
)
foreach ($lic in $licenseSources) {
    $src = Join-Path $repo $lic.Src
    if (Test-Path -LiteralPath $src) {
        Count-Copy (Copy-NoxFileIfChanged $src (Join-Path $licenseDir $lic.Dst))
    }
    else {
        Write-NoxWarning "License file not found, not packaged: $($lic.Src)"
    }
}
Write-NoxProgress 0.85

# Launchers. ASCII with CRLF, no BOM, so cmd.exe reads them reliably.
$launchers = @{
    'Play.cmd'   = @('@echo off', 'rem NOX Racing: launches the game preset. Extra arguments are passed through.', 'start "" /D "%~dp0" "%~dp0Nox_Engine.exe" --preset game %*')
    'Editor.cmd' = @('@echo off', 'rem NOX Engine: launches the editor preset. Extra arguments are passed through.', 'start "" /D "%~dp0" "%~dp0Nox_Engine.exe" --preset editor %*')
}
foreach ($name in $launchers.Keys) {
    $text = ($launchers[$name] -join "`r`n") + "`r`n"
    $path = Join-Path $outDir $name
    $existing = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllText($path, [Text.Encoding]::ASCII) } else { $null }
    if ($existing -ne $text) {
        [IO.File]::WriteAllText($path, $text, [Text.Encoding]::ASCII)
        $script:copiedCount++
    }
    else {
        $script:unchangedCount++
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
$all = Get-ChildItem -LiteralPath $outDir -Recurse -File
$totalBytes = ($all | Measure-Object -Property Length -Sum).Sum
Write-NoxStep ('Package ready: {0}' -f $outDir)
Write-NoxStep ('  files: {0} ({1:N1} MB), refreshed or added this run: {2}, unchanged: {3}' -f `
        $all.Count, ($totalBytes / 1MB), $script:copiedCount, $script:unchangedCount)
Write-NoxStep ('  folders synced: ' + (($script:DirsToSync.Keys | Sort-Object) -join ', '))
Write-NoxStep ('  scenes: ' + (($packagedScenes | Where-Object { $script:FilesToCopy.ContainsKey($_) }) -join ', '))
Write-NoxStep '  start the game with Play.cmd, the editor with Editor.cmd'
Write-NoxProgress 1.0
exit 0
