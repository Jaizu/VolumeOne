# Closes the running VolumeOne, builds bin\VolumeOne.exe and launches it.
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\bin\VolumeOne.exe"

Get-Process VolumeOne -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process VolumeOne -ErrorAction SilentlyContinue | Wait-Process -Timeout 10

& cmd /c "`"$root\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# Launch through WMI rather than Start-Process: when this script runs inside a
# packaged app (e.g. the Claude desktop app), a direct child inherits its
# AppData virtualization, so the tap dll lands in a folder explorer can't see.
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = "`"$exe`""; CurrentDirectory = "$root\bin" }
if ($r.ReturnValue -ne 0) { throw "Launch failed ($($r.ReturnValue))." }
