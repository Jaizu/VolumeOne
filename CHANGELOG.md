# Changelog

All notable changes to VolumeOne. Each version's section is used as the notes of its GitHub release, and VolumeOne shows them in Settings when an update is available.

## 1.4.0

### New
- **Output selector** in the mixer: the flyout shows the current output, and clicking its name opens a list of your other outputs (the list slides open above it) so you can switch the default device.
- The device volume slider is now at the bottom of the mixer. The app list opens and closes from the button next to it, with an animation.
- Settings is split into collapsible sections that animate open and closed: **Settings**, **Personalization**, **Inputs** and **Outputs**.
- The flyout grows and shrinks smoothly when content opens or closes. It also has a drop shadow and a thin outline, like Quick Settings.
- **Device names** (Settings → Personalization): show your audio devices by the name you gave them in Windows Sound settings ("SHAKERS"), by the hardware's name ("AB13X USB Audio"), or both, as Windows does. Applies to the mixer, Settings, the volume pop-up and the right-click menu. A button opens Sound settings to rename them.
- The custom level meter colour also sets the colour of the slider bar behind the meter, and an animated preview under the colour options shows how it looks.
- Pasting a colour like `#47D1A8` into a hex field drops the `#` instead of cutting off the last digit.

### Changed
- **Close and restore Windows** keeps your settings and custom device icons, so closing VolumeOne this way and opening it again no longer resets them. It still brings back the Windows volume controls and removes the startup entry.
- Updates are checked as soon as VolumeOne starts instead of 15 seconds later.
- The Updates card is at the bottom of Settings and is always open.

### Fixed
- Reopening the flyout after closing it in Settings no longer flashes the Settings view for a moment.
- Switching between the mixer and Settings is smooth: animations now run at the display's refresh rate instead of stuttering at about 30 fps.
- The first time Settings opens is no longer slow; it is prepared in the background when the flyout appears.
- Launching VolumeOne with `/settings` while the flyout is open now animates to Settings.
- With **Remove volume from Quick Settings** on, the Quick Settings panel no longer keeps an empty gap where the volume slider was.

## 1.3.0

### New
- **Replace the Windows volume icon** (Settings): VolumeOne takes over the original volume icon next to the network icon instead of adding its own tray icon. Left-click opens the mixer, right-click the menu, middle-click mutes and the mouse wheel changes the volume. Hovering it highlights the whole Quick Settings button, like the network icon.
- **Volume pill**: changing the volume with the mouse wheel over the taskbar icon, or muting with a middle-click, shows a small pill above the taskbar with the device, a bar and the percentage.
- **Remove volume from Quick Settings** (Settings): hides the volume slider in the Windows Quick Settings panel while VolumeOne is running. On laptops the brightness slider stays.
- **Level meter colour** (Settings): accent, white, green (like Windows 7) or a custom hex colour. With green, the filled part of the sliders turns white (dark theme) or dark grey (light theme) so the green stands out.
- **Close and restore Windows** (Settings): for when you stop using VolumeOne. It closes the app, brings back the Windows volume icon and the Quick Settings slider, removes the startup entry, deletes its settings and data, and restarts the taskbar so nothing of it is left loaded.
- **Location warning**: if VolumeOne runs from Downloads, the Desktop or a temp folder, a yellow banner offers to move it to `Documents\VolumeOne` and updates "Start with Windows". Works with any Windows language and with redirected folders.
- **Translations**: the texts live in `lang/*.lang` files, one per language, so they're easy to translate. English and Spanish for now, picked from the Windows display language. See `lang/README.md` to add one.

### Changed
- When explorer or Quick Settings still has an older VolumeOne helper loaded (for example after an update), VolumeOne restarts it automatically so the new one is used. explorer goes through the Windows Restart Manager, which reopens File Explorer windows.
- The footer matches the Quick Settings one: same height and colour, no divider line, and the gear in the same place, drawn like the Quick Settings gear.
- The gear turns when clicked and the settings slide in; in Settings, a back arrow takes its place and turns from left to down when going back. The back button at the top of Settings is gone.
- The Output devices and Input devices sections in Settings start collapsed, sit in a card like the rest, and have a clearly visible collapse button.
- The level meter is thicker than the slider track and a solid colour, so it's easier to see.
- Clearer wording for the volume icon options.
- VolumeOne can no longer be closed from its own UI (Exit is gone from Settings and the right-click menu). To stop using it, use **Close and restore Windows**.

### Fixed
- The header of an expanded device is the same height as a collapsed one, so its hover highlight is centred.
- The mixer closes when you click anywhere else, also when it was opened from the Windows volume icon.
- The right-click menu opened from the Windows volume icon appears above the taskbar instead of partly behind it.
- Release notes in Settings no longer show a stray Markdown header when the notes file starts with a byte order mark.

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
