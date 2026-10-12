<#
.SYNOPSIS
Reports directory sizes, tracked bytes, and large files without modifying the checkout.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Common.ps1')
$repo = Get-NoxRepoRoot
$tracked = @{}
Push-Location -LiteralPath $repo
try { & git -c core.quotepath=false ls-files | ForEach-Object { $tracked[$_] = $true } }
finally { Pop-Location }
$rows = @()
$largest = @()
foreach ($entry in Get-ChildItem -LiteralPath $repo -Force) {
    Assert-NoxNoReparsePoint $entry.FullName
    $files = if ($entry.PSIsContainer) { @(Get-ChildItem -LiteralPath $entry.FullName -Recurse -File -Force) } else { @($entry) }
    $size = [long]0; $trackedSize = [long]0
    foreach ($file in $files) {
        $size += $file.Length
        $rel = $file.FullName.Substring($repo.Length + 1).Replace('\', '/')
        if ($tracked.ContainsKey($rel)) { $trackedSize += $file.Length }
        if ($file.Length -ge 32MB) { $largest += [pscustomobject]@{ MiB = [Math]::Round($file.Length / 1MB, 1); File = $rel } }
    }
    $rows += [pscustomobject]@{ Directory = $entry.Name; GiB = [Math]::Round($size / 1GB, 3); Files = $files.Count; TrackedMiB = [Math]::Round($trackedSize / 1MB, 1) }
}
$rows | Sort-Object GiB -Descending | Format-Table -AutoSize
Write-NoxStep 'Largest files (at least 32 MiB):'
$largest | Sort-Object MiB -Descending | Select-Object -First 20 | Format-Table -AutoSize
