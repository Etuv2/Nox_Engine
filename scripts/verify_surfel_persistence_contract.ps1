$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $repoRoot

python scripts\validate_surfel_gi_static.py
python scripts\validate_surfel_gpu_resources_scaffold.py

Write-Host 'Clean Surfel GI persistence contract verified.'
