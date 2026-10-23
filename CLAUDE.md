# VolumeOne

## After every code change

Always run the dev script at the end of each change set, without being asked:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev.ps1
```

It closes the running VolumeOne, builds `bin\VolumeOne.exe` and relaunches it, so the change can be checked live. Don't use a plain `build.cmd` instead, and don't add `2>&1` (any stderr line trips the script's `$ErrorActionPreference = "Stop"`).

## Releasing a version

Bump `src\version.h`, add its `## X.Y.Z` section to `CHANGELOG.md`, commit (`VolumeOne X.Y.Z`) and push to `main`. The Release workflow (`.github\workflows\release.yml`) builds every push to `main` and publishes the release when `src\version.h` names a version that isn't released yet, so pushing is the whole job. After pushing, check that the run passed (`gh run list --workflow release.yml --limit 1`). A failed run means the version isn't out. Only run `tools\release.ps1` by hand if CI can't publish.
