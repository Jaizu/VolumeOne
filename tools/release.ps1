# Publishes a GitHub release for the version in src\version.h, using that
# version's section of CHANGELOG.md as the release notes.
# Usage: tools\release.ps1 [-Exe bin\VolumeOne.exe]
param([string]$Exe = "$PSScriptRoot\..\bin\VolumeOne.exe")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."

$h = Get-Content "$root\src\version.h" -Raw
$v = "{0}.{1}.{2}" -f ([regex]::Match($h, 'VERSION_MAJOR (\d+)').Groups[1].Value),
                      ([regex]::Match($h, 'VERSION_MINOR (\d+)').Groups[1].Value),
                      ([regex]::Match($h, 'VERSION_PATCH (\d+)').Groups[1].Value)

$exeVersion = (Get-Item $Exe).VersionInfo.ProductVersion
if ($exeVersion -ne $v) { throw "$Exe is version $exeVersion, but src\version.h says $v. Rebuild first." }

$changelog = Get-Content "$root\CHANGELOG.md" -Raw
$m = [regex]::Match($changelog, "(?ms)^## $([regex]::Escape($v))\s*\r?\n(.*?)(?=^## |\z)")
if (-not $m.Success) { throw "CHANGELOG.md has no section for $v." }
$notes = $m.Groups[1].Value.Trim()
$notesFile = Join-Path $env:TEMP "volumeone-notes-$v.md"
Set-Content $notesFile $notes -Encoding utf8

gh release create "v$v" $Exe --repo Jaizu/VolumeOne --title "VolumeOne $v" --notes-file $notesFile
