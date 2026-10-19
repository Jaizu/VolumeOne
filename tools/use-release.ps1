# Removes local dev builds and replaces them with the latest GitHub release, then launches it.
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$bin = "$root\bin"

Get-Process VolumeOne -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process VolumeOne -ErrorAction SilentlyContinue | Wait-Process -Timeout 10

Remove-Item "$bin\VolumeOne.exe*" -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $bin | Out-Null

gh release download --repo Jaizu/VolumeOne --pattern VolumeOne.exe --dir $bin --clobber
if ($LASTEXITCODE -ne 0) { throw "Could not download the latest release." }

Start-Process "$bin\VolumeOne.exe"
