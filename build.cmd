@echo off
rem Builds bin\VolumeOne.exe (with the explorer dll embedded) using MSVC.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
goto build

:novs
echo MSVC not found. Install "Desktop development with C++" or the VS Build Tools.
exit /b 1

rem Optional first argument: output folder (default: bin).
:build
set "OUT=%~1"
if "%OUT%"=="" set "OUT=bin"
if not exist "%OUT%" mkdir "%OUT%"
if not exist obj mkdir obj

set CFLAGS=/nologo /std:c++20 /O2 /GL /Gy /MT /EHsc /W3 /permissive- /utf-8 /DUNICODE /D_UNICODE /DNDEBUG /Fo:obj\
set LFLAGS=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO

rem The tap dll goes first: it is embedded in the exe as a resource.
cl %CFLAGS% /LD src\tap\tap.cpp /Fe:obj\VolumeOneTap.dll /link %LFLAGS% /DEF:src\tap\tap.def ^
   user32.lib ole32.lib oleaut32.lib advapi32.lib windowsapp.lib || exit /b 1

rc /nologo /fo obj\app.res res\app.rc || exit /b 1

cl %CFLAGS% src\main.cpp src\audio.cpp src\flyout.cpp src\settings.cpp src\tapclient.cpp src\updater.cpp obj\app.res ^
   /Fe:"%OUT%\VolumeOne.exe" /link %LFLAGS% /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib advapi32.lib dwmapi.lib d2d1.lib dwrite.lib ^
   windowscodecs.lib version.lib shcore.lib propsys.lib winhttp.lib bcrypt.lib || exit /b 1

echo.
echo Built %OUT%\VolumeOne.exe
