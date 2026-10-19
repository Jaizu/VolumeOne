#pragma once

#include "common.h"

// Battery level of wireless headsets, shown next to their output device.
//
// A worker thread reads the level about once a minute: Bluetooth devices
// through the level Windows itself keeps (the one in Settings > Bluetooth),
// and some USB dongles through their own HID protocol (see battery.cpp). A
// reading belongs to a physical device (its PnP container id), which is also
// what links it to the device's audio endpoints.
struct BatteryLevel {
    int percent = -1;  // 0..100, -1 = unknown
    bool charging = false;
};

namespace Battery {

// Starts the worker; it posts WM_APP_BATTERY_CHANGED to `notify` when a level changes.
void Start(HWND notify);
void Stop();
// Asks for a new reading soon (devices changed, the flyout opened).
void Refresh();
// The level of the device an audio endpoint belongs to, if one is known.
bool ForEndpoint(const std::wstring& endpointId, BatteryLevel* out);

}  // namespace Battery
