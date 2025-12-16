
# PowerShell script to remove comment prefixes from all files in src directory
# Removes: "CRITICAL FIX:", "FIX:", "CRITICAL:" from comments

param(
    [string]$SrcDirectory = "C:\Users\desti\Documents\GithubStuff\Nox_Engine\src"
)

# Patterns to remove
$patterns = @(
    "CRITICAL FIX:\s*",
    "FIX:\s*",
    "CRITICAL:\s*"
)

# File extensions to process
$extensions = @("*.cpp", "*.h", "*.glsl", "*.json")

# Counter for statistics
$filesModified = 0
$totalReplacements = 0

Write-Host "Starting comment cleanup in: $SrcDirectory" -ForegroundColor Green
Write-Host "Patterns to remove:" -ForegroundColor Yellow
$patterns | ForEach-Object { Write-Host "  - $_" }
Write-Host ""

# Get all relevant files recursively
$files = @()
foreach ($ext in $extensions) {
    $files += Get-ChildItem -Path $SrcDirectory -Filter $ext -Recurse
}

Write-Host "Found $($files.Count) files to process" -ForegroundColor Cyan
Write-Host ""

# Process each file
foreach ($file in $files) {
    $content = Get-Content -Path $file.FullName -Raw
    $originalContent = $content
    
    # Apply each pattern
    foreach ($pattern in $patterns) {
        $content = $content -replace $pattern, ""
    }
    
    # If content changed, write it back
    if ($content -ne $originalContent) {
        # Count replacements
        $original_count = ([regex]::Matches($originalContent, "CRITICAL FIX:|FIX:|CRITICAL:") | Measure-Object).Count
        $totalReplacements += $original_count
        
        Set-Content -Path $file.FullName -Value $content -NoNewline
        $filesModified++
        
        Write-Host "? Modified: $($file.FullName)" -ForegroundColor Green
        Write-Host "  Removed $original_count occurrences" -ForegroundColor Gray
    }
}

Write-Host ""
Write-Host "=" * 60 -ForegroundColor Yellow
Write-Host "Cleanup Complete!" -ForegroundColor Green
Write-Host "Files modified: $filesModified" -ForegroundColor Cyan
Write-Host "Total replacements: $totalReplacements" -ForegroundColor Cyan
Write-Host "=" * 60 -ForegroundColor Yellow
