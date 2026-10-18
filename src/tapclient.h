#pragma once

#include "common.h"

// Loads VolumeOneTap.dll into explorer through the XAML diagnostics API (the
// same mechanism TranslucentTB uses) and tells it when to re-read settings.
namespace TapClient {

// Makes sure the dll is loaded when the icon should be hidden, then signals it.
// Blocking: call from a worker thread. Returns false if explorer isn't ready.
bool Sync(bool hide);

// Wakes the dll so it re-reads the setting and whether VolumeOne is running.
void Notify();

}  // namespace TapClient
