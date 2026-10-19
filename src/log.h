#pragma once

#include "common.h"

// Diagnostic log, always on: %LOCALAPPDATA%\VolumeOne\VolumeOne.log (the
// previous one is kept as VolumeOne.old.log). The explorer dll writes its own
// VolumeOneTap.log next to it. A crash also leaves VolumeOne.dmp there.
namespace Log {

// Starts a new section in the log and installs the crash handler.
void Init(const wchar_t* cmdLine);
// printf-style, one line, thread-safe.
void Write(const wchar_t* fmt, ...);
// The folder the logs are in.
std::wstring Folder();
// Opens that folder in File Explorer.
void OpenFolder();

}  // namespace Log

#define LOG(...) Log::Write(__VA_ARGS__)
