# Closes the running VolumeOne, builds bin\VolumeOne.exe and launches it.
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\bin\VolumeOne.exe"

Get-Process VolumeOne -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process VolumeOne -ErrorAction SilentlyContinue | Wait-Process -Timeout 10

& cmd /c "`"$root\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Start-Process $exe
