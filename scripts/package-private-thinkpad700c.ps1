param(
    [Parameter(Mandatory = $true)][string]$RuntimeRoot,
    [string]$BinaryPath,
    [string]$OutputDirectory,
    [switch]$NoArchive
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = (Resolve-Path -LiteralPath $RuntimeRoot).Path
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve source commit' }
$branch = (& git -C $repoRoot branch --show-current).Trim()
$dirty = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0 -or $dirty.Count) {
    throw 'Commit source changes before packaging so the source ZIP matches the handoff.'
}
if (-not $BinaryPath) { $BinaryPath = Join-Path $sourceRoot '86Box.exe' }
$BinaryPath = (Resolve-Path -LiteralPath $BinaryPath).Path
$created = [DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss UTC')
if (-not $OutputDirectory) {
    $name = 'ThinkPad700C-PRIVATE-Handoff-' + [DateTime]::UtcNow.ToString('yyyyMMdd') + '-' + $commit.Substring(0, 8)
    $OutputDirectory = Join-Path (Join-Path $repoRoot 'private-dist') $name
}
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'private-dist')) + [IO.Path]::DirectorySeparatorChar
if (-not $outputRoot.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Private output must be under this repository''s ignored private-dist directory.'
}
if ((Test-Path -LiteralPath $outputRoot) -or (Test-Path -LiteralPath ($outputRoot + '.zip'))) {
    throw "Refusing to overwrite an existing package: $outputRoot"
}

# Explicit input list: never copy an entire development directory or a .git tree.
$runtimeFiles = @(
    'runtime/roms/machines/ibmtp700c/39G9996.bin',
    'runtime/roms/video/mda/mda.rom',
    'runtime/media/ThinkPad700C-system.img',
    'runtime/media/ThinkPad700C-reference-1.20.ima',
    'runtime/media/ThinkPad700C-diagnostics.ima',
    'runtime/media/ThinkPad700C-video-1.20.ima',
    'runtime/ThinkPad 700C/86box.cfg',
    'runtime/ThinkPad 700C/nvr/ibmtp700c_39g9996.nvr',
    'runtime/ThinkPad 700C/nvr/ibmtp700c_39g9996_sec.nvr'
)
foreach ($relative in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot $relative) -PathType Leaf)) {
        throw "Missing private runtime file: $relative"
    }
}
$config = Get-Content -LiteralPath (Join-Path $sourceRoot 'runtime/ThinkPad 700C/86box.cfg') -Raw
if ($config -notmatch '(?m)^machine\s*=\s*ibmtp700c\s*$') { throw 'Expected a configured 700C runtime' }
if ($config -match '(?m)^\s*(hdd_01_fn|fdd_01_fn)\s*=\s*(?:[A-Za-z]:|[/\\])') {
    throw 'Use relative media paths in the runtime configuration before packaging.'
}

New-Item -ItemType Directory -Path $outputRoot | Out-Null
foreach ($relative in $runtimeFiles) {
    $destination = Join-Path $outputRoot $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot $relative) -Destination $destination
}
Copy-Item -LiteralPath $BinaryPath -Destination (Join-Path $outputRoot '86Box.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'COPYING') -Destination $outputRoot
Copy-Item -LiteralPath (Join-Path $repoRoot 'AUTHORS') -Destination $outputRoot
# Preserve the installed drive-audio library and its provenance when present.
# These are explicit asset locations, never the whole development workspace.
foreach ($relative in @('assets/sounds/fdd', 'assets/sounds/hdd', 'assets/LICENSE-86Box-assets.txt', 'DRIVE-AUDIO.json')) {
    $audioSource = Join-Path $sourceRoot $relative
    if (Test-Path -LiteralPath $audioSource) {
        $audioDestination = Join-Path $outputRoot $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $audioDestination) -Force | Out-Null
        Copy-Item -LiteralPath $audioSource -Destination $audioDestination -Recurse
    }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'verify-private-package.ps1') -Destination (Join-Path $outputRoot 'Verify-Package.ps1')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'create-private-shortcut.ps1') -Destination (Join-Path $outputRoot 'Create-Desktop-Shortcut.ps1')
$launcher = @'
@echo off
setlocal
cd /d "%~dp0"
start "" "%~dp086Box.exe" -N -W -F -R "%~dp0runtime\roms" -P "%~dp0runtime\ThinkPad 700C"
'@
[IO.File]::WriteAllText((Join-Path $outputRoot 'Start-ThinkPad-700C.bat'), $launcher.Replace("`r`n", "`n").Replace("`n", "`r`n") + "`r`n", [Text.Encoding]::ASCII)

$sourceFolder = Join-Path $outputRoot 'source'
New-Item -ItemType Directory -Path $sourceFolder | Out-Null
$sourceArchive = Join-Path $sourceFolder '86Box-ThinkPad700_700C.zip'
& git -C $repoRoot archive --format=zip "--output=$sourceArchive" HEAD
if ($LASTEXITCODE -ne 0) { throw 'Source archive failed' }

$verificationFolder = Join-Path $outputRoot 'verification'
New-Item -ItemType Directory -Path (Join-Path $verificationFolder 'images') -Force | Out-Null
foreach ($doc in @('VERIFICATION.md', 'RELEASE-NOTES.md')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "docs/$doc") -Destination $verificationFolder
}
foreach ($capture in @('reference-proportions', 'error-log-empty', 'windows-color', '256-color-test', 'usb-floppy-reference')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "docs/images/thinkpad700c-$capture.png") -Destination (Join-Path $verificationFolder 'images')
}

$exeHash = (Get-FileHash -LiteralPath (Join-Path $outputRoot '86Box.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
$readme = Get-Content -LiteralPath (Join-Path $repoRoot 'docs/PRIVATE-700C-README.template.md') -Raw -Encoding UTF8
$readme = $readme.Replace('@CREATED_UTC@', $created).Replace('@SOURCE_COMMIT@', $commit).Replace('@SOURCE_BRANCH@', $branch).Replace('@EXECUTABLE_SHA256@', $exeHash)
[IO.File]::WriteAllText((Join-Path $outputRoot 'README.md'), $readme, (New-Object Text.UTF8Encoding($false)))
$buildInfo = [ordered]@{
    created_utc = $created
    source_repository = 'https://github.com/Deng-ge-open-source/86Box-Thinkpad700_700C'
    source_commit = $commit
    source_branch = $branch
    upstream_baseline = '86Box 6.0 / 4fef696a4'
    binary_platform = 'Windows x64; Qt software renderer'
    executable_sha256 = $exeHash
    runtime_machine = 'ibmtp700c'
    verification = 'verification/VERIFICATION.md'
    private_runtime = $true
}
$buildInfo | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputRoot 'BUILD-INFO.json') -Encoding UTF8

$entries = @()
foreach ($item in (Get-ChildItem -LiteralPath $outputRoot -Recurse -File | Sort-Object FullName)) {
    $relative = $item.FullName.Substring($outputRoot.Length + 1).Replace('\', '/')
    $entries += [ordered]@{
        path = $relative
        size = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        mutable = ($relative.StartsWith('runtime/media/') -or $relative.StartsWith('runtime/ThinkPad 700C/'))
    }
}
@{ format_version = 1; files = $entries } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot 'PACKAGE-MANIFEST.json') -Encoding UTF8
$entries | ForEach-Object { $_.sha256 + '  ' + $_.path } | Set-Content -LiteralPath (Join-Path $outputRoot 'SHA256SUMS.txt') -Encoding ASCII
& (Join-Path $outputRoot 'Verify-Package.ps1') -AllFiles

if (-not $NoArchive) {
    Compress-Archive -LiteralPath $outputRoot -DestinationPath ($outputRoot + '.zip') -CompressionLevel Optimal
    $archiveHash = (Get-FileHash -LiteralPath ($outputRoot + '.zip') -Algorithm SHA256).Hash.ToLowerInvariant()
    $archiveHash + '  ' + [IO.Path]::GetFileName($outputRoot + '.zip') | Set-Content -LiteralPath ($outputRoot + '.zip.sha256') -Encoding ASCII
}
Write-Output "Private handoff: $outputRoot"
if (-not $NoArchive) { Write-Output "Private ZIP: $outputRoot.zip" }
