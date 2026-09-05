# ReC-Sandbox sync
# Regenerates this repository's engine/, patches/ and docs/ trees from the local development
# engine (E:\coding3\ReC_sandbox\CRYENGINE_Source, branch dev) against its pristine baseline
# (branch main). Run after every reviewed development stage, then commit here.
#
#   powershell -ExecutionPolicy Bypass -File sync.ps1 [-DevRepo <path>] [-BaseRef main] [-DevRef dev]

param(
    [string]$DevRepo = "E:\coding3\ReC_sandbox\CRYENGINE_Source",
    [string]$BaseRef = "main",
    [string]$DevRef  = "dev"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function Reset-Dir([string]$path) {
    if (Test-Path $path) { Get-ChildItem $path -Force | Remove-Item -Recurse -Force }
    New-Item -ItemType Directory -Force $path | Out-Null
}

Write-Host "Dev repo : $DevRepo ($BaseRef..$DevRef)"
$devHead  = (git -C $DevRepo rev-parse --short $DevRef).Trim()
$baseHead = (git -C $DevRepo rev-parse --short $BaseRef).Trim()

# 1. Patch series: one file per commit, plus one cumulative patch.
$patchDir = Join-Path $root "patches"
Reset-Dir $patchDir
git -C $DevRepo format-patch -q --binary -o $patchDir "$BaseRef..$DevRef"
git -C $DevRepo diff --binary "$BaseRef" "$DevRef" | Out-File -Encoding utf8 (Join-Path $patchDir "full.patch")

# 2. Source snapshot: every file added or modified on dev, in engine tree layout, so it can
#    be dropped onto a pristine CRYENGINE 5.7.1 checkout. Deleted files are listed, not copied.
$srcDir = Join-Path $root "engine"
Reset-Dir $srcDir
$changes = git -C $DevRepo diff --name-status "$BaseRef" "$DevRef"
$deleted = @()
foreach ($line in $changes) {
    $parts = $line -split "`t"
    $status = $parts[0]
    $file = $parts[-1]
    if ($status -like "D*") { $deleted += $file; continue }
    $dst = Join-Path $srcDir $file
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    git -C $DevRepo show "${DevRef}:$file" | Set-Content -Path $dst -Encoding utf8 -NoNewline
}
$deleted | Set-Content (Join-Path $srcDir "DELETED_FILES.txt")

# 3. Docs: plugin READMEs and the console reference (user documentation only).
$docDir = Join-Path $root "docs"
Reset-Dir $docDir
$docSources = @(
    "Code/CryPlugins/CinematicCamera/README.md",
    "Code/CryPlugins/CinematicCamera/docs/ConsoleReference.md",
    "Code/CryPlugins/CryPhoneTracker/README.md",
    "Code/CryPlugins/CinematicCamera/docs/SceneReferredContent.md",
    "Code/CryPlugins/CinematicCamera/docs/SceneReferredCalibration.md",
    "Code/CryPlugins/CinematicCamera/docs/SceneReferredLook.md",
    "Code/CryPlugins/CinematicCamera/docs/SceneReferredExport.md"
)
# Design specs are internal work orders and stay out of the public snapshot.
foreach ($doc in ($docSources | Select-Object -Unique)) {
    git -C $DevRepo cat-file -e "${DevRef}:$doc" 2>$null; if ($LASTEXITCODE -ne 0) { continue }
    $name = Split-Path -Leaf $doc
    if ($doc -like "*CryPhoneTracker*" -and $name -eq "README.md") { $name = "CryPhoneTracker-README.md" }
    if ($doc -like "*CinematicCamera*" -and $name -eq "README.md") { $name = "CinematicCamera-README.md" }
    git -C $DevRepo show "${DevRef}:$doc" | Set-Content -Path (Join-Path $docDir $name) -Encoding utf8 -NoNewline
}

# 4. Manifest.
$log = git -C $DevRepo log --oneline "$BaseRef..$DevRef"
@(
    "# Sync manifest",
    "",
    "Baseline : $BaseRef @ $baseHead (pristine CRYENGINE 5.7.1)",
    "Dev      : $DevRef @ $devHead",
    "Synced   : $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
    "",
    "## Commits (newest first)",
    ""
) + ($log | ForEach-Object { "- $_" }) + @(
    "",
    "## Changed files",
    ""
) + ($changes | ForEach-Object { "- $_" }) | Set-Content (Join-Path $root "MANIFEST.md") -Encoding utf8

Write-Host "Synced ${DevRef}@${devHead}: $((Get-ChildItem $patchDir).Count - 1) patches, $(($changes | Measure-Object).Count) files, $((Get-ChildItem $docDir).Count) docs."
