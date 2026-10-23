# Publishes a GitHub release for the version in src\version.h, with
# VolumeOne.exe and VolumeOneRemover.exe (from the same folder), using that
# version's section of CHANGELOG.md as the release notes. The tag goes on the
# current commit, which must already be pushed.
# The Release workflow runs this with -IfNew on every push to main, which does
# nothing when that version is already released.
# Usage: tools\release.ps1 [-Exe bin\VolumeOne.exe] [-IfNew]
param([string]$Exe = "$PSScriptRoot\..\bin\VolumeOne.exe", [switch]$IfNew)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."

$h = Get-Content "$root\src\version.h" -Raw
$v = "{0}.{1}.{2}" -f ([regex]::Match($h, 'VERSION_MAJOR (\d+)').Groups[1].Value),
                      ([regex]::Match($h, 'VERSION_MINOR (\d+)').Groups[1].Value),
                      ([regex]::Match($h, 'VERSION_PATCH (\d+)').Groups[1].Value)

if ($IfNew) {
    $released = gh release list --repo Jaizu/VolumeOne --limit 1000 --json tagName --jq ".[].tagName"
    if ($LASTEXITCODE -ne 0) { throw "Could not list the releases." }
    if ($released -contains "v$v") { "v$v is already released."; return }
}

$Remover = Join-Path (Split-Path $Exe) "VolumeOneRemover.exe"
foreach ($f in $Exe, $Remover) {
    if (-not (Test-Path $f)) { throw "$f not found. Rebuild first." }
    $exeVersion = (Get-Item $f).VersionInfo.ProductVersion
    if ($exeVersion -ne $v) { throw "$f is version $exeVersion, but src\version.h says $v. Rebuild first." }
}

$changelog = Get-Content "$root\CHANGELOG.md" -Raw
$m = [regex]::Match($changelog, "(?ms)^## $([regex]::Escape($v))\s*\r?\n(.*?)(?=^## |\z)")
if (-not $m.Success) { throw "CHANGELOG.md has no section for $v." }
$notes = $m.Groups[1].Value.Trim()
$notesFile = Join-Path $env:TEMP "volumeone-notes-$v.md"
[IO.File]::WriteAllText($notesFile, $notes, (New-Object System.Text.UTF8Encoding $false))  # no BOM

$commit = git -C $root rev-parse HEAD
gh release create "v$v" $Exe $Remover --repo Jaizu/VolumeOne --target $commit --title "VolumeOne $v" --notes-file $notesFile
if ($LASTEXITCODE -ne 0) { throw "Could not create the release." }
