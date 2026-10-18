#pragma once

#include "common.h"

// Tiny two-language string table: Spanish if the Windows UI language is
// Spanish, English otherwise.
struct Strings {
    const wchar_t* settings;
    const wchar_t* back;
    const wchar_t* useSystemIcon;
    const wchar_t* useSystemIconDesc;
    const wchar_t* hideQsVolume;
    const wchar_t* hideQsVolumeDesc;
    const wchar_t* hideSystemIcon;
    const wchar_t* hideSystemIconDesc;
    const wchar_t* startWithWindows;
    const wchar_t* startWithWindowsDesc;
    const wchar_t* outputDevices;
    const wchar_t* outputDevicesDesc;
    const wchar_t* inputDevices;
    const wchar_t* inputDevicesDesc;
    const wchar_t* noDevices;
    const wchar_t* systemSounds;
    const wchar_t* soundSettings;
    const wchar_t* openMixer;
    const wchar_t* windowsMixer;
    const wchar_t* exit;
    const wchar_t* muted;
    const wchar_t* audioOutput;
    const wchar_t* audioInput;
    const wchar_t* noInputs;
    const wchar_t* iconAuto;
    const wchar_t* iconSpeakers;
    const wchar_t* iconHeadphones;
    const wchar_t* iconCustom;
    const wchar_t* iconFiles;
    const wchar_t* updates;
    const wchar_t* checkUpdates;
    const wchar_t* checking;
    const wchar_t* upToDate;
    const wchar_t* checkFailed;
    const wchar_t* updateAvailable;  // followed by the version
    const wchar_t* updateNow;
    const wchar_t* updating;
    const wchar_t* updateFailed;
    const wchar_t* whatsNew;  // followed by the version
};

inline const Strings& S() {
    static const Strings es = {
        L"Configuración",
        L"Atrás",
        L"Usar el icono de volumen de Windows",
        L"Al pulsar el icono original de la barra de tareas se abre VolumeOne, sin icono propio en la bandeja",
        L"Quitar el volumen de los Ajustes rápidos",
        L"Oculta el control de volumen del panel de Ajustes rápidos de Windows mientras VolumeOne está abierto",
        L"Ocultar el icono de volumen de Windows",
        L"Quita el icono de volumen de la barra de tareas mientras VolumeOne está abierto",
        L"Iniciar con Windows",
        L"Se abre en segundo plano al iniciar sesión",
        L"Dispositivos de salida",
        L"Desactiva los que no quieras ver en el mezclador ni en el menú del botón derecho. Pulsa el icono para cambiarlo",
        L"Dispositivos de entrada",
        L"Desactiva los que no quieras ver en el menú del botón derecho",
        L"No hay dispositivos de salida visibles",
        L"Sonidos del sistema",
        L"Configuración de sonido",
        L"Abrir mezclador",
        L"Mezclador de volumen de Windows",
        L"Salir",
        L"Silenciado",
        L"Salida de audio",
        L"Entrada de audio",
        L"No hay entradas de audio",
        L"Automático",
        L"Altavoces",
        L"Auriculares",
        L"Icono personalizado…",
        L"Iconos e imágenes",
        L"Actualizaciones",
        L"Buscar actualizaciones",
        L"Buscando…",
        L"Estás al día",
        L"No se pudo comprobar",
        L"Disponible la versión ",
        L"Actualizar",
        L"Actualizando…",
        L"No se pudo actualizar",
        L"Novedades de la versión ",
    };
    static const Strings en = {
        L"Settings",
        L"Back",
        L"Use the Windows volume icon",
        L"Clicking the original taskbar icon opens VolumeOne, with no separate tray icon",
        L"Remove volume from Quick Settings",
        L"Hides the volume control in the Windows Quick Settings panel while VolumeOne is running",
        L"Hide the Windows volume icon",
        L"Removes the volume icon from the taskbar while VolumeOne is running",
        L"Start with Windows",
        L"Opens in the background when you sign in",
        L"Output devices",
        L"Turn off the ones you don't want to see in the mixer or the right-click menu. Click the icon to change it",
        L"Input devices",
        L"Turn off the ones you don't want to see in the right-click menu",
        L"No visible output devices",
        L"System sounds",
        L"Sound settings",
        L"Open mixer",
        L"Windows volume mixer",
        L"Exit",
        L"Muted",
        L"Audio output",
        L"Audio input",
        L"No audio inputs",
        L"Automatic",
        L"Speakers",
        L"Headphones",
        L"Custom icon…",
        L"Icons and images",
        L"Updates",
        L"Check for updates",
        L"Checking…",
        L"You're up to date",
        L"Couldn't check",
        L"Version available: ",
        L"Update",
        L"Updating…",
        L"Update failed",
        L"What's new in ",
    };
    static const bool spanish = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    return spanish ? es : en;
}
