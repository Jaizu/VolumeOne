# VolumeOne

A small, fast volume mixer for the Windows 11 notification area. Something like EarTrumpet, but native Win32 + Direct2D: it starts in under 100 ms and uses about 2 MB of memory in the background.

## Features

- **Per-app volume** for every output device, in a flyout styled like Windows 11 Quick Settings: acrylic backdrop, rounded corners, Fluent sliders, your accent colour, light/dark mode
- **Hide output devices** you don't want in the mixer (for example bass shakers for sim racing). Settings → Output devices
- **Hide the Windows volume icon** from the taskbar (toggle). This replaces the Windhawk mod *taskbar-tray-system-icon-tweaks → hideVolumeIcon*. The icon comes back when you turn the toggle off or quit VolumeOne
- **Start with Windows** (toggle)
- Tray icon uses the same Segoe Fluent glyphs as the Windows one and shows the default device's level
- Click a speaker or app icon to mute it. Scroll over a row to change its volume. Middle-click the tray icon to mute the default device

## Usage

- **Left-click** the tray icon: open the mixer
- **Right-click**: menu (settings, Windows sound settings, exit)
- Windows 11 puts new tray icons in the overflow menu. To keep it visible: Settings → Personalization → Taskbar → Other system tray icons → VolumeOne

## Building

Requires the MSVC toolchain (Visual Studio or Build Tools with "Desktop development with C++") and the Windows SDK.

```
build.cmd
```

Output: `bin\VolumeOne.exe` and `bin\VolumeOneTap.dll`. Keep both in the same folder.

## How hiding the Windows volume icon works

The taskbar's tray is XAML inside `explorer.exe`. VolumeOne loads `VolumeOneTap.dll` into explorer with `InitializeXamlDiagnosticsEx` (the documented XAML diagnostics API that TranslucentTB also uses). The dll watches the visual tree and collapses the volume glyph inside the Quick Settings button, the same elements the Windhawk mod hides. Nothing is patched on disk.

The dll stays loaded until explorer restarts, so if you rebuild while it is loaded, `build.cmd` can't replace `bin\VolumeOneTap.dll` (it warns). Restart explorer to pick up the new one.

Settings live in `HKCU\Software\VolumeOne`. Start with Windows uses `HKCU\...\Run`.
