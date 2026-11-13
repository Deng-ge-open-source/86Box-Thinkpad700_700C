param(
    [string]$PackageRoot = $PSScriptRoot,
    [switch]$AllFiles
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$manifest = Get-Content -LiteralPath (Join-Path $root 'PACKAGE-MANIFEST.json') -Raw | ConvertFrom-Json
$checked = 0
$skipped = 0
$problems = @()
foreach ($entry in $manifest.files) {
    if ($entry.mutable -and -not $AllFiles) { $skipped++; continue }
    $target = [IO.Path]::GetFullPath((Join-Path $root $entry.path))
    if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Invalid manifest path: $($entry.path)"
    }
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        $problems += "Missing: $($entry.path)"
        continue
    }
    $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $entry.sha256) { $problems += "Changed: $($entry.path)" }
    $checked++
}
if ($problems.Count) { throw ($problems -join [Environment]::NewLine) }
Write-Output "Verification passed: $checked files; $skipped mutable VM files skipped."
if ($skipped) { Write-Output 'Use -AllFiles before the first boot to check the complete initial snapshot.' }
