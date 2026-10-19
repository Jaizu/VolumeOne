# VolumeOne

## After every code change

Always run the dev script at the end of each change set, without being asked:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev.ps1
```

It closes the running VolumeOne, builds `bin\VolumeOne.exe` and relaunches it, so the change can be checked live. Don't use a plain `build.cmd` instead, and don't add `2>&1` (any stderr line trips the script's `$ErrorActionPreference = "Stop"`).
