#include "log.h"
#include "settings.h"

#include <dbghelp.h>

#include <cstdarg>
#include <mutex>

namespace Log {

namespace {

const DWORD kMaxSize = 1024 * 1024;  // past this the log starts over (the old one is kept)

std::recursive_mutex g_lock;  // the crash handler may write while this thread holds it
HANDLE g_file = INVALID_HANDLE_VALUE;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;

void WriteUtf8(const std::wstring& line) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), s.data(), n, nullptr, nullptr);
    DWORD written;
    WriteFile(g_file, s.data(), (DWORD)s.size(), &written, nullptr);
}

std::wstring WindowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {sizeof(v)};
    if (auto get = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) get(&v);
    DWORD ubr = 0, size = sizeof(ubr);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                 nullptr, &ubr, &size);
    wchar_t buf[64];
    swprintf_s(buf, L"%lu.%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, ubr);
    return buf;
}

// "VolumeOne.exe+0x1234" for a code address.
std::wstring AddressName(void* addr) {
    HMODULE mod = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &mod) && mod) {
        GetModuleFileNameW(mod, name, MAX_PATH);
    }
    wchar_t buf[MAX_PATH + 32];
    swprintf_s(buf, L"%s+0x%llX", PathFindFileNameW(name), (unsigned long long)((BYTE*)addr - (BYTE*)mod));
    return buf;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    Write(L"CRASH: exception %08X at %s", er->ExceptionCode, AddressName(er->ExceptionAddress).c_str());

    // A minidump, so the crash can be opened in a debugger.
    std::wstring path = Folder() + L"\\VolumeOne.dmp";
    HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    auto writeDump = dbghelp ? (WriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump") : nullptr;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writeDump && f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
        BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
        Write(L"minidump %s: %s", ok ? L"written" : L"failed", path.c_str());
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

std::wstring Folder() { return AppDataDir(); }

void Init(const wchar_t* cmdLine) {
    std::wstring path = Folder() + L"\\VolumeOne.log";
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > kMaxSize))
        MoveFileExW(path.c_str(), (Folder() + L"\\VolumeOne.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    // Shared, so it can be opened in an editor while VolumeOne runs.
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WriteUtf8(L"\r\n");
    Write(L"===== VolumeOne %s, pid %lu, Windows %s", APP_VERSION, GetCurrentProcessId(), WindowsVersion().c_str());
    Write(L"exe: %s", exe);
    Write(L"command line: \"%s\"", cmdLine ? cmdLine : L"");
    BOOL elevated = FALSE;
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te;
        DWORD size;
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &size)) elevated = te.TokenIsElevated;
        CloseHandle(token);
    }
    wchar_t lang[LOCALE_NAME_MAX_LENGTH] = L"";
    GetUserDefaultLocaleName(lang, LOCALE_NAME_MAX_LENGTH);
    Write(L"elevated=%d, locale=%s, system dpi=%u, monitors=%d", elevated, lang, GetDpiForSystem(),
          GetSystemMetrics(SM_CMONITORS));
}

void Write(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t head[64];
    swprintf_s(head, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%5lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    std::lock_guard lock(g_lock);
    WriteUtf8(std::wstring(head) + msg + L"\r\n");
}

void OpenFolder() {
    std::wstring dir = Folder();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace Log
