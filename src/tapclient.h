#pragma once

#include "common.h"

// Loads VolumeOneTap.dll into explorer (taskbar) and ShellHost (Quick
// Settings) through the XAML diagnostics API (the same mechanism TranslucentTB
// uses) and tells it when to re-read settings.
namespace TapClient {

// Makes sure this build's dll is loaded where it is needed (explorer: to hide
// the Windows icon or take its clicks; ShellHost: to hide the Quick Settings
// volume slider), restarting a process that has an older build's dll, then
// signals it. Blocking: call from a worker thread. Returns false if a process
// that needs it isn't ready yet.
bool Sync(bool explorer, bool shellHost);

// Wakes the dll wherever it is loaded so it re-reads the settings and whether
// VolumeOne is running.
void Notify();

// Restarts explorer and ShellHost if they have the dll loaded, so nothing of
// VolumeOne is left in them. Blocking.
void Unload();

}  // namespace TapClient
