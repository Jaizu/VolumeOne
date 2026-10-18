# Changelog

All notable changes to VolumeOne. Each version's section is used as the notes of its GitHub release, and VolumeOne shows them in Settings when an update is available.

## Unreleased

### New
- **Use the Windows volume icon** (Settings): instead of a separate tray icon, the original volume icon in the taskbar opens VolumeOne. Left-click opens the mixer, right-click the menu, middle-click mutes and the mouse wheel changes the volume. Hovering it highlights the whole Quick Settings button, like the network icon.
- **Remove volume from Quick Settings** (Settings): hides the volume slider in the Windows Quick Settings panel while VolumeOne is running. On laptops the brightness slider stays.

- **Close and restore Windows** (Settings): for when you stop using VolumeOne. It closes the app, brings back the Windows volume icon and the Quick Settings slider, removes the startup entry, deletes its settings and data, and restarts the taskbar so nothing of it is left loaded.
- **Level meter colour** (Settings): accent, white, green (like Windows 7) or a custom hex colour.
- **Translations**: the texts live in `lang/*.lang` files, one per language, so they're easy to translate. English and Spanish for now, picked from the Windows display language. See `lang/README.md` to add one.
- **Location warning**: if VolumeOne runs from Downloads, the Desktop or a temp folder, a yellow banner offers to move it to `Documents\VolumeOne` (and updates "Start with Windows"). Works with any Windows language and with redirected folders.

### Changed
- When explorer (or Quick Settings) still has an older VolumeOne helper loaded, for example after an update, VolumeOne restarts it automatically so the new one is used. explorer is restarted through the Windows Restart Manager, which reopens File Explorer windows.
- The Output devices and Input devices sections in Settings start collapsed.
- The gear only turns when clicked; the view then slides in, and the back arrow turns from left to down when going back.
- The level meter on the sliders is thicker than the track and a solid lighter shade of the accent colour, so it's easier to see.
- The footer matches the Quick Settings one: same height and colour, no divider line, and the gear in the same place.
- In Settings, the footer has a back arrow (in the gear's place) instead of the Exit button.
- VolumeOne can no longer be closed from its own UI: Exit is gone from the right-click menu too.
- The settings gear is drawn like the Quick Settings one (six pointed teeth, small hole).
- Changing the volume with the mouse wheel over the taskbar icon (or muting with a middle-click) shows a small pill above the taskbar with the device, a bar and the percentage.

### Fixed
- The header of an expanded device is now the same height as a collapsed one, so its hover highlight is centred on the name and icon.
- The flyout closes when you click anywhere else, also when it was opened from the Windows volume icon (it didn't, because Windows doesn't give it the focus in that case).
- The right-click menu opened from the Windows volume icon is placed above the taskbar instead of partly behind it.

## 1.2.0

### New
- **Switch the default output** from the tray icon's right-click menu, next to the audio inputs. Hidden devices are left out.
- **Level meters**: every slider (device and app) shows the current audio level as a lighter band, like EarTrumpet.
- **What's new**: when an update is available, Settings shows its release notes. Clicking the update banner in the mixer opens them.

### Changed
- The current output device is always the last card, right above the taskbar.
- The inactive bars of the volume icon are drawn dimmed, like the Windows icon, both in the tray and in the mixer.
- Hiding a device in Settings now also hides it from the right-click menu.

### Fixed
- Muting (for example with a middle-click on the tray icon) made the hidden Windows volume icon appear again, showing the mute glyph. Windows draws the icon with several stacked glyphs, and the background one goes empty when muted; the icon is now treated as a whole.

## 1.1.0

### New
- **Collapsible devices**: click a device's header to collapse or expand it. Devices that aren't the default and aren't playing anything start collapsed.
- **Device icons**: choose speakers, headphones or a custom image (`.ico`, `.png`, `.jpg`, `.bmp`) for each output device. Custom icons are copied to `%LOCALAPPDATA%\VolumeOne\icons`, so they keep working if the original file is deleted.
- **Default audio input** can be switched from the tray icon's right-click menu. Input devices can be hidden from it in Settings.
- **Built-in updater**: checks GitHub releases at startup and once a day, verifies the download's SHA-256 and replaces the exe in place.

### Changed
- VolumeOne is now a single `VolumeOne.exe`. The explorer helper dll is embedded and extracted to `%LOCALAPPDATA%\VolumeOne` with a per-build name, so updating never conflicts with the copy explorer has loaded.
- The helper re-hides the Windows volume icon if something else (like a Windhawk mod being unloaded) shows it again.

## 1.0.0

First version.

- Per-app volume for every output device in a flyout styled like Windows 11 Quick Settings (acrylic, rounded corners, Fluent sliders, accent colour, light and dark mode).
- Hide output devices from the mixer.
- Hide the Windows volume tray icon (replaces the Windhawk mod *taskbar-tray-system-icon-tweaks*).
- Start with Windows.
- Tray icon with the same Fluent glyphs as the Windows one. Middle-click mutes the default device.
- Starts in under 100 ms and uses about 2 MB of memory in the background.
