#pragma once

#include "common.h"

// Tiny two-language string table: Spanish if the Windows UI language is
// Spanish, English otherwise.
struct Strings {
    const wchar_t* settings;
    const wchar_t* back;
    const wchar_t* hideSystemIcon;
    const wchar_t* hideSystemIconDesc;
    const wchar_t* startWithWindows;
    const wchar_t* startWithWindowsDesc;
    const wchar_t* outputDevices;
    const wchar_t* outputDevicesDesc;
    const wchar_t* noDevices;
    const wchar_t* systemSounds;
    const wchar_t* soundSettings;
    const wchar_t* openMixer;
    const wchar_t* windowsMixer;
    const wchar_t* exit;
    const wchar_t* muted;
    const wchar_t* defaultDevice;
};

inline const Strings& S() {
    static const Strings es = {
        L"Configuración",
        L"Atrás",
        L"Ocultar el icono de volumen de Windows",
        L"Quita el icono de volumen de la barra de tareas mientras VolumeOne está abierto",
        L"Iniciar con Windows",
        L"Se abre en segundo plano al iniciar sesión",
        L"Dispositivos de salida",
        L"Desactiva los que no quieras ver en el mezclador",
        L"No hay dispositivos de salida visibles",
        L"Sonidos del sistema",
        L"Configuración de sonido",
        L"Abrir mezclador",
        L"Mezclador de volumen de Windows",
        L"Salir",
        L"Silenciado",
        L"Predeterminado",
    };
    static const Strings en = {
        L"Settings",
        L"Back",
        L"Hide the Windows volume icon",
        L"Removes the volume icon from the taskbar while VolumeOne is running",
        L"Start with Windows",
        L"Opens in the background when you sign in",
        L"Output devices",
        L"Turn off the ones you don't want to see in the mixer",
        L"No visible output devices",
        L"System sounds",
        L"Sound settings",
        L"Open mixer",
        L"Windows volume mixer",
        L"Exit",
        L"Muted",
        L"Default",
    };
    static const bool spanish = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    return spanish ? es : en;
}
