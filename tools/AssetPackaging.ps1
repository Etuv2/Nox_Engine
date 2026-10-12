# Dependency discovery shared by Package.ps1 and its fixture tests.
. (Join-Path $PSScriptRoot 'Common.ps1')

$script:NoxRuntimeExtensions = @('.glb', '.gltf', '.bin', '.fbx', '.obj', '.mtl', '.hdr', '.exr',
    '.png', '.jpg', '.jpeg', '.tga', '.bmp', '.dds', '.ktx', '.ktx2', '.ttf', '.otf',
    '.wav', '.ogg', '.mp3', '.json', '.ini', '.glsl', '.vert', '.frag', '.geom', '.comp')
$script:NoxAssetRoots = @('config', 'fonts', 'hdrs', 'images', 'scenes', 'shaders', 'sounds', 'textures', 'models', 'data', 'generated')

function Get-NoxJsonReferences {
    param($Node, [string]$Key = '')
    if ($null -eq $Node) { return }
    if ($Node -is [string]) {
        [pscustomobject]@{ Raw = $Node; Key = $Key }
    }
    elseif ($Node -is [System.Management.Automation.PSCustomObject]) {
        foreach ($prop in $Node.PSObject.Properties) {
            # Authoring metadata and generated output names are not input dependencies.
            if ($prop.Name -in @('prompt', 'exports', 'source_sha256', 'output')) { continue }
            Get-NoxJsonReferences $prop.Value $prop.Name
        }
    }
    elseif ($Node -is [System.Collections.IEnumerable]) {
        foreach ($item in $Node) { Get-NoxJsonReferences $item $Key }
    }
}

function Read-NoxAssetJson {
    param([string]$Path)
    if ([IO.Path]::GetExtension($Path) -ne '.glb') {
        return [IO.File]::ReadAllText($Path) | ConvertFrom-Json
    }
    # Read only the GLB JSON chunk, not the often very large embedded binary buffer.
    $reader = New-Object IO.BinaryReader([IO.File]::OpenRead($Path))
    try {
        if ($reader.ReadUInt32() -ne 0x46546C67 -or $reader.ReadUInt32() -ne 2) { throw "Invalid GLB header: $Path" }
        $length = $reader.ReadUInt32()
        $jsonLength = $reader.ReadUInt32()
        if ($length -ne $reader.BaseStream.Length -or $jsonLength -gt ($length - 20) -or
            $reader.ReadUInt32() -ne 0x4E4F534A) { throw "Invalid GLB JSON chunk: $Path" }
        return [Text.Encoding]::UTF8.GetString($reader.ReadBytes($jsonLength)).TrimEnd([char]0) | ConvertFrom-Json
    }
    finally { $reader.Dispose() }
}

function Get-NoxPackageAssets {
    param(
        [string]$Root,
        [string[]]$Scenes,
        [string[]]$Include,
        [hashtable]$JsonOverrides = @{}
    )
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $files = @{}
    $queue = New-Object 'System.Collections.Generic.Queue[string]'
    $reasons = @{}

    function Add-PackagePath {
        param([string]$FullPath, [string]$Reason)
        $full = [IO.Path]::GetFullPath($FullPath)
        if (-not (Test-NoxPathWithin $full $rootPath)) { throw "Asset escapes repository: $FullPath ($Reason)" }
        Assert-NoxNoReparsePoint $full
        $rel = $full.Substring($rootPath.Length + 1).Replace('\', '/')
        if ($rel.Split('/')[0] -notin $script:NoxAssetRoots) { throw "Unsupported asset root: $rel ($Reason)" }
        if ($rel -match '(^|/)(src|source|sources|raw|originals|__pycache__)(/|$)' -or
            ((Test-Path -LiteralPath $full -PathType Leaf) -and [IO.Path]::GetExtension($rel) -notin $script:NoxRuntimeExtensions)) {
            throw "Authoring/non-runtime file cannot be packaged: $rel ($Reason)"
        }
        if (-not (Test-Path -LiteralPath $full)) { throw "Missing asset '$rel' ($Reason)" }
        if (Test-Path -LiteralPath $full -PathType Container) {
            foreach ($child in Get-ChildItem -LiteralPath $full -Recurse -File) {
                $childRel = $child.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
                if ($childRel -match '(^|/)(src|source|sources|raw|originals|__pycache__)(/|$)' -or
                    $child.Extension -notin $script:NoxRuntimeExtensions) { continue }
                Add-PackagePath $child.FullName $Reason
            }
            return
        }
        if (-not $files.ContainsKey($rel)) {
            $files[$rel] = $full
            $reasons[$rel] = $Reason
            $queue.Enqueue($rel)
        }
    }

    function Add-PackageReference {
        param([string]$Raw, [string]$From, [bool]$ModelUri = $false)
        $rawPath = $Raw.Trim().Replace('\', '/')
        if ($rawPath -eq '' -or $rawPath.StartsWith('data:')) { return }
        if ($rawPath.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0) {
            if ($ModelUri) { throw "Invalid model URI in $From" }
            return
        }
        if ($rawPath -match '^[a-zA-Z][a-zA-Z0-9+.-]*:' -or $rawPath.StartsWith('/')) {
            if ($ModelUri -or [IO.Path]::GetExtension($rawPath) -in $script:NoxRuntimeExtensions) {
                throw "Non-portable asset reference '$Raw' in $From"
            }
            return
        }
        if ($ModelUri) { $rawPath = [Uri]::UnescapeDataString($rawPath) }
        $top = $rawPath.TrimStart('.', '/').Split('/')[0]
        if (-not $ModelUri -and $top -notin $script:NoxAssetRoots -and
            [IO.Path]::GetExtension($rawPath) -notin $script:NoxRuntimeExtensions) { return }
        if ($rawPath -eq 'scenes') { return }
        $fromDir = Split-Path -Parent (Join-Path $rootPath $From)
        $candidates = if ($ModelUri) { @(Join-Path $fromDir $rawPath) } else {
            @((Join-Path $rootPath $rawPath), (Join-Path $fromDir $rawPath), (Join-Path (Split-Path -Parent $fromDir) $rawPath))
        }
        foreach ($candidate in $candidates) {
            $candidate = [IO.Path]::GetFullPath($candidate)
            if (-not (Test-NoxPathWithin $candidate $rootPath)) { continue }
            if (Test-Path -LiteralPath $candidate) { Add-PackagePath $candidate "referenced by $From"; return }
        }
        throw "Missing or out-of-tree asset '$Raw' (referenced by $From)"
    }

    foreach ($rel in @($Scenes) + @($Include)) {
        if ($rel) { Add-PackagePath (Join-Path $rootPath $rel) 'package profile' }
    }
    while ($queue.Count) {
        $rel = $queue.Dequeue()
        $ext = [IO.Path]::GetExtension($rel).ToLowerInvariant()
        if ($ext -in @('.json', '.gltf', '.glb')) {
            $doc = if ($JsonOverrides.ContainsKey($rel)) { $JsonOverrides[$rel] } else { Read-NoxAssetJson $files[$rel] }
            if ($ext -in @('.gltf', '.glb')) {
                foreach ($entry in @($doc.buffers) + @($doc.images)) {
                    if ($entry.uri) { Add-PackageReference $entry.uri $rel $true }
                }
            }
            else {
                foreach ($reference in Get-NoxJsonReferences $doc) { Add-PackageReference $reference.Raw $rel }
            }
        }
        elseif ($ext -in @('.obj', '.mtl')) {
            foreach ($line in [IO.File]::ReadAllLines($files[$rel])) {
                if ($line -match '^\s*(?:mtllib|map_[A-Za-z]+|bump|disp|decal)\s+(.+?)\s*$') {
                    # shortcut: MTL map options are rejected; add a parser if an exported model needs them.
                    if ($Matches[1].StartsWith('-')) { throw "Unsupported MTL map options in $rel" }
                    Add-PackageReference $Matches[1] $rel $true
                }
            }
        }
        # shortcut: FBX sidecars are opaque; list their texture directory in the profile's include list.
    }
    return [pscustomobject]@{ Files = $files; Reasons = $reasons }
}
