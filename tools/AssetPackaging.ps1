# Dependency discovery shared by Package.ps1 and its fixture tests.
#
# Graph roots: profile scenes, profile include paths, the project descriptor (C4) and the engine
# ownership list. Edges come from registered schema keys (packaging/dependency_rules.json), glTF
# buffers/images, OBJ/MTL sidecars, shader #include directives (resolved like ShaderLoader: relative
# to the including file), and project script roots / data roots / explicit assets. Other JSON uses a
# generic reference scan for documents without a registration. Every file records the reason it was
# selected.
. (Join-Path $PSScriptRoot 'Common.ps1')

$script:NoxRuntimeExtensions = @('.glb', '.gltf', '.bin', '.fbx', '.obj', '.mtl', '.hdr', '.exr',
    '.png', '.jpg', '.jpeg', '.tga', '.bmp', '.dds', '.ktx', '.ktx2', '.ttf', '.otf',
    '.wav', '.ogg', '.mp3', '.json', '.ini', '.glsl', '.vert', '.frag', '.geom', '.comp', '.lua')
$script:NoxAssetRoots = @('config', 'fonts', 'hdrs', 'images', 'scenes', 'shaders', 'sounds', 'textures', 'models', 'data', 'generated', 'projects')
$script:NoxShaderExtensions = @('.glsl', '.vert', '.frag', '.geom', '.comp')

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

# Follows only registered dotted keys ("scenes.path": every element of "scenes" that has "path").
# Arrays are transparent; a key that is absent or not a string contributes nothing.
function Get-NoxRegisteredReferences {
    param($Node, [string[]]$Segments = @())
    if ($null -eq $Node) { return }
    if ($Segments.Count -eq 0) {
        if ($Node -is [string]) { [pscustomobject]@{ Raw = $Node; Key = '' } }
        return
    }
    if ($Segments[0] -eq '**') {
        # Any depth: match the rest here, then keep descending with the same pattern.
        Get-NoxRegisteredReferences $Node @($Segments | Select-Object -Skip 1)
        if ($Node -is [System.Management.Automation.PSCustomObject]) {
            foreach ($prop in $Node.PSObject.Properties) { Get-NoxRegisteredReferences $prop.Value $Segments }
        }
        elseif ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string]) {
            foreach ($item in $Node) { Get-NoxRegisteredReferences $item $Segments }
        }
        return
    }
    if ($Node -is [System.Collections.IEnumerable] -and $Node -isnot [string]) {
        foreach ($item in $Node) { Get-NoxRegisteredReferences $item $Segments }
        return
    }
    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $prop = $Node.PSObject.Properties[$Segments[0]]
        if ($prop) { Get-NoxRegisteredReferences $prop.Value @($Segments | Select-Object -Skip 1) }
    }
}

# packaging/dependency_rules.json: { "documents": { "<logical path>": { "reference_keys": [...] } } }
function Get-NoxDependencyRules {
    param([string]$Root)
    $rules = @{}
    $path = Join-Path $Root 'packaging/dependency_rules.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $rules }
    $doc = Read-NoxAssetJson $path
    if ($doc.format -ne 1) { throw 'Unsupported dependency_rules.json format' }
    foreach ($prop in $doc.documents.PSObject.Properties) {
        $rules[$prop.Name] = [string[]]@($prop.Value.reference_keys)
    }
    if ($doc.PSObject.Properties['scene_documents']) {
        $scene = $doc.scene_documents
        $generators = @{}
        foreach ($gen in $scene.generators.PSObject.Properties) { $generators[$gen.Name] = [string[]]@($gen.Value) }
        $rules['#scene'] = @{
            reference_keys = [string[]]@($scene.reference_keys)
            default_generator = [string]$scene.default_generator
            generators = $generators
        }
    }
    return $rules
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

# Every path component must match the stored spelling exactly: packs are case-sensitive, loose
# Windows folders are not, so a mismatch would pass on the development machine and fail in a pack.
function Assert-NoxExactCase {
    param([string]$Full, [string]$RootPath, [hashtable]$Listings = @{})
    $rel = $Full.Substring($RootPath.Length + 1)
    $current = $RootPath
    foreach ($segment in $rel.Split([IO.Path]::DirectorySeparatorChar)) {
        if (-not $Listings.ContainsKey($current)) {
            $Listings[$current] = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
            foreach ($entry in [IO.Directory]::GetFileSystemEntries($current)) { [void]$Listings[$current].Add((Split-Path -Leaf $entry)) }
        }
        if (-not $Listings[$current].Contains($segment)) {
            throw "Case mismatch: '$($rel.Replace('\', '/'))' is not stored with this spelling"
        }
        $current = Join-Path $current $segment
    }
}

# packaging/pack_rules.json: extensions that are always stored (seek-heavy or already compressed).
function Get-NoxStoredExtensions {
    param([string]$Root)
    $path = Join-Path $Root 'packaging/pack_rules.json'
    $doc = Read-NoxAssetJson $path
    if ($doc.format -ne 1) { throw 'Unsupported pack_rules.json format' }
    return [string[]]@($doc.stored_extensions | ForEach-Object { $_.ToLowerInvariant() })
}

# Reads the ownership list shared by all profiles: engine.pak content.
function Get-NoxEngineOwnedPaths {
    param([string]$Root)
    $path = Join-Path $Root 'packaging/engine_owned.json'
    $doc = Read-NoxAssetJson $path
    if ($doc.format -ne 1) { throw 'Unsupported engine_owned.json format' }
    return [string[]]@($doc.paths)
}

# Procedural nodes anywhere in a scene document (their "procedural" objects).
function Find-NoxProceduralNodes {
    param($Node, [System.Collections.Generic.List[object]]$List)
    if ($null -eq $Node -or $Node -is [string]) { return }
    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $proc = $Node.PSObject.Properties['procedural']
        if ($proc -and $proc.Value -is [System.Management.Automation.PSCustomObject]) { $List.Add($proc.Value) }
        foreach ($prop in $Node.PSObject.Properties) { Find-NoxProceduralNodes $prop.Value $List }
    }
    elseif ($Node -is [System.Collections.IEnumerable]) {
        foreach ($item in $Node) { Find-NoxProceduralNodes $item $List }
    }
}

# Schema-aware scene references: registered keys (dotted, "**" = any depth) plus each procedural
# generator's declared params. Path-like values that no registration claims are returned as Unclaimed
# (reported, not packed): add them to packaging/dependency_rules.json when they are real inputs.
function Get-NoxSceneReferences {
    param($Doc, [hashtable]$SceneRules)
    $refs = New-Object System.Collections.Generic.List[string]
    $claimed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $warnings = New-Object System.Collections.Generic.List[string]
    foreach ($key in @($SceneRules.reference_keys)) {
        foreach ($reference in Get-NoxRegisteredReferences $Doc $key.Split('.')) {
            $refs.Add($reference.Raw); [void]$claimed.Add($reference.Raw)
        }
    }
    $procedurals = New-Object System.Collections.Generic.List[object]
    Find-NoxProceduralNodes $Doc $procedurals
    foreach ($proc in $procedurals) {
        $genProp = $proc.PSObject.Properties['generator']
        $generator = if ($genProp) { [string]$genProp.Value } else { [string]$SceneRules.default_generator }
        if (-not $SceneRules.generators.ContainsKey($generator)) {
            $warnings.Add("procedural generator '$generator' has no declaration in dependency_rules.json; its inputs are not packed")
            continue
        }
        $paramsProp = $proc.PSObject.Properties['params']
        if (-not $paramsProp -or $paramsProp.Value -isnot [System.Management.Automation.PSCustomObject]) { continue }
        foreach ($key in @($SceneRules.generators[$generator])) {
            $p = $paramsProp.Value.PSObject.Properties[$key]
            if ($p -and $p.Value -is [string]) { $refs.Add($p.Value); [void]$claimed.Add($p.Value) }
        }
    }
    foreach ($reference in Get-NoxJsonReferences $Doc) {
        if ($reference.Key -in @('output', 'name', 'id', 'scene_name')) { continue }
        if ($claimed.Contains($reference.Raw)) { continue }
        if ($reference.Raw -match '^(config|fonts|hdrs|images|scenes|shaders|sounds|textures|models|data|generated|projects)[/\\]' -or
            $reference.Raw -match '\.(json|glb|gltf|png|jpg|jpeg|hdr|exr|ttf|wav|ogg|mp3|glsl|bin|lua)$') {
            $warnings.Add("unclaimed path-like value '$($reference.Raw)' at key '$($reference.Key)' (not packed)")
        }
    }
    return [pscustomobject]@{ References = @($refs | Select-Object -Unique); Warnings = @($warnings | Select-Object -Unique) }
}

function Get-NoxPackageAssets {
    param(
        [string]$Root,
        [string[]]$Scenes,
        [string[]]$Include,
        [string]$Project = '',
        [hashtable]$JsonOverrides = @{},
        [hashtable]$Rules = $null
    )
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    if ($null -eq $Rules) { $Rules = Get-NoxDependencyRules $rootPath }
    $files = @{}
    $queue = New-Object 'System.Collections.Generic.Queue[string]'
    $reasons = @{}
    $listings = @{}
    $warnings = New-Object System.Collections.Generic.List[string]

    function Add-PackagePath {
        param([string]$FullPath, [string]$Reason)
        $full = [IO.Path]::GetFullPath($FullPath)
        if (-not (Test-NoxPathWithin $full $rootPath)) { throw "Asset escapes repository: $FullPath ($Reason)" }
        Assert-NoxNoReparsePoint $full
        $rel = $full.Substring($rootPath.Length + 1).Replace('\', '/')
        if ($rel.Split('/')[0] -notin $script:NoxAssetRoots) { throw "Unsupported asset root: $rel ($Reason)" }
        if ($rel -match '(^|/)(src|source|sources|raw|originals|__pycache__)(/|$)|generation_prompts\.json$' -or
            ((Test-Path -LiteralPath $full -PathType Leaf) -and [IO.Path]::GetExtension($rel) -notin $script:NoxRuntimeExtensions)) {
            throw "Authoring/non-runtime file cannot be packaged: $rel ($Reason)"
        }
        if (-not (Test-Path -LiteralPath $full)) { throw "Missing asset '$rel' ($Reason)" }
        Assert-NoxExactCase $full $rootPath $listings
        if (Test-Path -LiteralPath $full -PathType Container) {
            foreach ($child in Get-ChildItem -LiteralPath $full -Recurse -File | Sort-Object FullName) {
                $childRel = $child.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
                if ($childRel -match '(^|/)(src|source|sources|raw|originals|__pycache__)(/|$)|generation_prompts\.json$' -or
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

    # #include resolution matches src/ShaderLoader.h: relative to the including file only.
    function Add-ShaderInclude {
        param([string]$Raw, [string]$From)
        $candidate = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent (Join-Path $rootPath $From)) $Raw.Trim()))
        if (-not (Test-NoxPathWithin $candidate $rootPath) -or -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "Missing shader include '$Raw' (included by $From)"
        }
        Add-PackagePath $candidate "shader #include in $From"
    }

    # Project descriptor (contract C4). Project-relative paths resolve against the descriptor's
    # directory (AssetIO::ResolveRelative semantics); logical roots are reached with "../..".
    function Add-ProjectDescriptor {
        param([string]$DescriptorRel)
        $descriptorFull = Join-Path $rootPath $DescriptorRel
        if (-not (Test-Path -LiteralPath $descriptorFull -PathType Leaf)) { throw "Project descriptor missing: $DescriptorRel" }
        Add-PackagePath $descriptorFull 'project descriptor'
        $desc = Read-NoxAssetJson $descriptorFull
        if ($desc.format -ne 1) { throw "Unsupported project format in $DescriptorRel" }
        $projectDir = Split-Path -Parent (Join-Path $rootPath $DescriptorRel)
        function Resolve-ProjectPath([string]$Ref) {
            $full = [IO.Path]::GetFullPath((Join-Path $projectDir $Ref.Replace('\', '/')))
            if (-not (Test-NoxPathWithin $full $rootPath)) { throw "Project path escapes repository: $Ref ($DescriptorRel)" }
            return $full
        }
        $entryFull = Resolve-ProjectPath $desc.entry
        if (-not (Test-Path -LiteralPath $entryFull -PathType Leaf)) { throw "Project entry missing: $($desc.entry) ($DescriptorRel)" }
        $scriptDirs = @()
        foreach ($ref in @($desc.script_roots)) {
            $dir = Resolve-ProjectPath $ref
            if (-not (Test-Path -LiteralPath $dir -PathType Container)) { throw "Script root missing: $ref ($DescriptorRel)" }
            Assert-NoxExactCase $dir $rootPath $listings
            $scriptDirs += $dir
            # Every .lua under a declared root is a module candidate; module names are not parsed.
            foreach ($lua in Get-ChildItem -LiteralPath $dir -Recurse -File -Filter '*.lua' | Sort-Object FullName) {
                Add-PackagePath $lua.FullName "Lua script root $ref"
            }
        }
        if (-not (@($scriptDirs | Where-Object { Test-NoxPathWithin $entryFull $_ -or $entryFull -eq $_ }).Count)) {
            throw "Project entry '$($desc.entry)' is not inside a declared script root ($DescriptorRel)"
        }
        foreach ($ref in @($desc.data_roots)) { Add-PackagePath (Resolve-ProjectPath $ref) "project data root $ref" }
        # Contract C4 refinement: "scenes" and "assets" are root-logical paths.
        foreach ($ref in @($desc.assets)) { Add-PackagePath (Join-Path $rootPath $ref.Replace('\', '/')) "project asset $ref" }
        foreach ($ref in @($desc.scenes)) { Add-PackagePath (Join-Path $rootPath $ref.Replace('\', '/')) "project scene $ref" }
    }

    foreach ($rel in @($Scenes) + @($Include)) {
        if ($rel) { Add-PackagePath (Join-Path $rootPath $rel) 'package profile' }
    }
    if ($Project) { Add-ProjectDescriptor $Project.Replace('\', '/') }
    while ($queue.Count) {
        $rel = $queue.Dequeue()
        $ext = [IO.Path]::GetExtension($rel).ToLowerInvariant()
        if ($ext -in @('.json', '.gltf', '.glb')) {
            if ($rel -match '^projects/[^/]+/project\.json$') { continue } # handled by Add-ProjectDescriptor
            $doc = if ($JsonOverrides.ContainsKey($rel)) { $JsonOverrides[$rel] } else { Read-NoxAssetJson $files[$rel] }
            if ($ext -in @('.gltf', '.glb')) {
                foreach ($entry in @($doc.buffers) + @($doc.images)) {
                    if ($entry.uri) { Add-PackageReference $entry.uri $rel $true }
                }
            }
            elseif ($rel -match '^scenes/.+\.json$' -and $Rules.ContainsKey('#scene')) {
                $scene = Get-NoxSceneReferences $doc $Rules['#scene']
                foreach ($raw in $scene.References) { Add-PackageReference $raw $rel }
                foreach ($message in $scene.Warnings) { $warnings.Add("${rel}: $message") }
            }
            elseif ($Rules.ContainsKey($rel)) {
                foreach ($key in $Rules[$rel]) {
                    foreach ($reference in Get-NoxRegisteredReferences $doc $key.Split('.')) {
                        Add-PackageReference $reference.Raw $rel
                    }
                }
            }
            else {
                foreach ($reference in Get-NoxJsonReferences $doc) { Add-PackageReference $reference.Raw $rel }
            }
        }
        elseif ($ext -in $script:NoxShaderExtensions) {
            foreach ($line in [IO.File]::ReadAllLines($files[$rel])) {
                if ($line -match '^\s*#\s*include\s*[<"]([^">]+)[">]') { Add-ShaderInclude $Matches[1] $rel }
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
    return [pscustomobject]@{ Files = $files; Reasons = $reasons; Warnings = @($warnings | Select-Object -Unique) }
}

# True when a logical path is owned by engine.pak (exact file or directory prefix).
function Test-NoxEngineOwned {
    param([string]$Rel, [string[]]$Owned)
    foreach ($owner in $Owned) {
        if ($Rel -ceq $owner -or $Rel.StartsWith($owner + '/', [StringComparison]::Ordinal)) { return $true }
    }
    return $false
}

# Writes text only when it differs, so unchanged manifests/launchers keep their timestamps.
function Write-NoxUtf8IfChanged {
    param([string]$Path, [string]$Text)
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        if ([IO.File]::ReadAllText($Path) -ceq $Text) { return $false }
    }
    Write-NoxUtf8 $Path $Text
    return $true
}
