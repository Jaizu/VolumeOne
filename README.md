# VolumeOne

> [!NOTE]
> This project was built with the help of AI ([Claude](https://claude.ai) by Anthropic).

A small, fast volume mixer for the Windows 11 notification area. Something like EarTrumpet, but native Win32 + Direct2D: it starts in under 100 ms and uses about 2 MB of memory in the background.

## Features

- **Per-app volume** for every output device, in a flyout styled like Windows 11 Quick Settings: acrylic backdrop, rounded corners, Fluent sliders, your accent colour, light/dark mode
- **Collapsible devices**: click a device's header to collapse or expand it. Devices that aren't the default and aren't playing anything start collapsed
- **Hide devices** you don't want to see (for example bass shakers for sim racing), for outputs and inputs. Settings → Output devices / Input devices
- **Device icons**: pick speakers, headphones or your own `.ico`/`.png` per output device (click the icon in Settings). Custom icons are copied to `%LOCALAPPDATA%\VolumeOne\icons`, so they keep working if you delete the original
- **Switch the default microphone** from the tray icon's right-click menu
- **Use the Windows volume icon** instead of a separate tray icon (toggle): the original taskbar icon opens VolumeOne (left-click mixer, right-click menu, middle-click mute, wheel for volume)
- **Remove the volume slider from Quick Settings** (toggle)
- **Hide the Windows volume icon** from the taskbar (toggle). This replaces the Windhawk mod *taskbar-tray-system-icon-tweaks → hideVolumeIcon*. The icon comes back when you turn the toggle off or quit VolumeOne
- **Start with Windows** (toggle)
- **Built-in updater**: checks GitHub releases at startup and once a day, and updates itself in place
- Click a speaker or app icon to mute it. Scroll over a row to change its volume. Middle-click the tray icon to mute the default device
- A single `.exe`, no installer

## Installation

1. Download `VolumeOne.exe` from the [latest release](https://github.com/Jaizu/VolumeOne/releases/latest), or [build it yourself](#building).
2. Put it in any folder and run it. Turn on **Start with Windows** in Settings.
3. Windows 11 puts new tray icons in the overflow menu. To keep it visible: Settings → Personalization → Taskbar → Other system tray icons → VolumeOne

SmartScreen may warn you the first time because the exe is not signed: **More info** → **Run anyway**.

## Usage

- **Left-click** the tray icon: open the mixer
- **Right-click**: pick the default audio input, open settings, Windows sound settings, exit

## Building

Requires the MSVC toolchain (Visual Studio or Build Tools with "Desktop development with C++") and the Windows SDK.

```
build.cmd
```

Output: `bin\VolumeOne.exe`. The version lives in `src\version.h`; a release is tagged `v<version>` with `VolumeOne.exe` attached.

## How hiding the Windows volume icon works

The taskbar's tray is XAML inside `explorer.exe`. VolumeOne embeds a small dll, writes it to `%LOCALAPPDATA%\VolumeOne` and loads it into explorer with `InitializeXamlDiagnosticsEx` (the documented XAML diagnostics API that TranslucentTB also uses). The dll watches the visual tree and collapses the volume glyph inside the Quick Settings button, the same elements the Windhawk mod hides. Nothing is patched on disk. In "use the Windows icon" mode it instead lays a transparent popup over the volume icon, so its clicks reach VolumeOne and not the Quick Settings button. The same dll is loaded into `ShellHost.exe` (the Quick Settings panel) to hide its volume slider. The dll stays loaded until explorer restarts; each build uses its own file name, so updates never collide with the loaded one.

Settings live in `HKCU\Software\VolumeOne`. Start with Windows uses `HKCU\...\Run`.
