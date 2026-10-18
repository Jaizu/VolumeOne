#pragma once

#include "common.h"

// Checks the latest GitHub release of Jaizu/VolumeOne and installs it.
// Each release is tagged vX.Y.Z and has a VolumeOne.exe asset.
namespace Updater {

#define REPOSITORY_URL L"https://github.com/Jaizu/VolumeOne"
#define UPDATED_ARG L"/updated"

struct ReleaseInfo {
    std::wstring version;  // "1.2.3"
    std::wstring exeUrl;
    std::wstring sha256;   // hex, from the asset's "digest"; may be empty
    std::wstring pageUrl;
};

// Blocking; call from a worker thread. False on network/parse errors.
bool FetchLatest(ReleaseInfo& out);

// True if version a > b ("1.10.0" > "1.9.3").
bool IsNewer(const std::wstring& a, const std::wstring& b);

// Downloads the new exe, swaps it in (the running exe is renamed to .old) and
// starts it with UPDATED_ARG <pid>. Blocking. The caller exits on success.
bool Install(const ReleaseInfo& release);

// Called by the new exe: waits for the old process and deletes the .old file.
void FinishUpdate(DWORD oldPid);

}  // namespace Updater
