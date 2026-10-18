#pragma once

// Shared between VolumeOne.exe and VolumeOneTap.dll (loaded into explorer and
// ShellHost).

// {6E1F3A57-2C4B-4E0D-9B8A-7F3C51D2A6E4}
static const CLSID CLSID_VolumeOneTap = {0x6e1f3a57, 0x2c4b, 0x4e0d, {0x9b, 0x8a, 0x7f, 0x3c, 0x51, 0xd2, 0xa6, 0xe4}};

// Single-instance mutex of VolumeOne.exe.
#define RUNNING_MUTEX_NAME L"Local\\VolumeOne.Running"
// VolumeOne's hidden window. The dll only hides the Windows volume icon while
// it exists, so quitting VolumeOne brings the icon back. (A window is visible
// across processes regardless of object ACLs, unlike the mutex.)
#define HOST_WINDOW_CLASS L"VolumeOne.Host"
// "<prefix><pid>" for each process the dll is in (explorer, ShellHost),
// signalled by VolumeOne.exe when a setting changes or the app exits.
#define TAP_REFRESH_PREFIX L"Local\\VolumeOne.TapRefresh."
// Created by the dll as "<prefix><pid>" so we don't inject twice.
#define TAP_LOADED_PREFIX L"Local\\VolumeOne.TapLoaded."
#define TAP_DLL_NAME L"VolumeOneTap.dll"

#define REG_KEY_PATH L"Software\\VolumeOne"
#define REG_HIDE_ICON L"HideSystemVolumeIcon"
// Use the Windows volume icon as VolumeOne's icon: the dll keeps it visible
// and forwards its clicks to VolumeOne instead of opening Quick Settings.
#define REG_USE_SYSTEM_ICON L"UseSystemVolumeIcon"
// Hide the volume slider in Windows' Quick Settings panel (ShellHost.exe).
#define REG_HIDE_QS_VOLUME L"HideQuickSettingsVolume"

// Posted by the dll to VolumeOne's host window; wParam is one of SYSICON_*.
#define WM_VOLUMEONE_SYSICON (WM_APP + 50)
#define SYSICON_CLICK 0
#define SYSICON_MENU 1
#define SYSICON_MIDDLE 2
#define SYSICON_WHEEL 3  // lParam = wheel delta (120 per notch)
