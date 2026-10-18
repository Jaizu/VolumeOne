#pragma once

#include "common.h"

// UI strings, loaded from lang\<code>.lang (embedded as LANG resources) for
// the Windows display language, with English as the fallback. To add a
// language, see lang\README.md.
#define STRING_KEYS(X)                                                                                         \
    X(settings) X(back)                                                                                        \
    X(useSystemIcon) X(useSystemIconDesc) X(hideQsVolume) X(hideQsVolumeDesc) X(hideSystemIcon)                \
    X(hideSystemIconDesc) X(startWithWindows) X(startWithWindowsDesc)                                          \
    X(meterColor) X(meterColorDesc) X(meterAccent) X(meterWhite) X(meterGreen) X(meterCustom)                  \
    X(outputDevices) X(outputDevicesDesc) X(inputDevices) X(inputDevicesDesc) X(noDevices)                     \
    X(systemSounds) X(soundSettings) X(openMixer) X(windowsMixer) X(muted)                                     \
    X(audioOutput) X(audioInput) X(noInputs)                                                                   \
    X(iconAuto) X(iconSpeakers) X(iconHeadphones) X(iconCustom) X(iconFiles)                                   \
    X(updates) X(checkUpdates) X(checking) X(upToDate) X(checkFailed) X(updateAvailable) X(updateNow)          \
    X(updating) X(updateFailed) X(whatsNew)                                                                    \
    X(closeAndRestore) X(closeAndRestoreDesc) X(closeAndRestoreButton) X(closeConfirmTitle) X(closeConfirm)    \
    X(locationWarning) X(moveToDocuments) X(moveFailed)

struct Strings {
#define X(key) const wchar_t* key;
    STRING_KEYS(X)
#undef X
};

const Strings& S();

// Replaces {0} in a string with a value.
std::wstring Fmt(const wchar_t* text, const std::wstring& value);
