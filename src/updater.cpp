#include "updater.h"

#include <winhttp.h>
#include <bcrypt.h>

namespace Updater {

static const wchar_t kLatestApi[] = L"https://api.github.com/repos/Jaizu/VolumeOne/releases/latest";

static bool HttpGet(const std::wstring& url, std::string& body) {
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring object = std::wstring(path) + extra;

    bool ok = false;
    HINTERNET session = WinHttpOpen(L"VolumeOne/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        WinHttpSetTimeouts(request, 10000, 10000, 30000, 60000);
        const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
        if (WinHttpSendRequest(request, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                body.clear();
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) break;
                    if (avail == 0) {
                        ok = true;
                        break;
                    }
                    size_t old = body.size();
                    body.resize(old + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, body.data() + old, avail, &read)) break;
                    body.resize(old + read);
                }
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Minimal JSON string lookup: finds "key" at or after `from` and returns its
// string value (handles \" and \\ escapes; others are copied as-is).
static bool JsonString(const std::string& json, const char* key, size_t from, std::string& value, size_t* at = nullptr) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle, from);
    if (p == std::string::npos) return false;
    if (at) *at = p;
    p += needle.size();
    while (p < json.size() && isspace((unsigned char)json[p])) p++;
    if (p >= json.size() || json[p] != ':') return false;
    p++;
    while (p < json.size() && isspace((unsigned char)json[p])) p++;
    if (p >= json.size() || json[p] != '"') return false;
    p++;
    value.clear();
    for (; p < json.size() && json[p] != '"'; p++) {
        if (json[p] != '\\' || p + 1 >= json.size()) {
            value.push_back(json[p]);
            continue;
        }
        char e = json[++p];
        switch (e) {
            case 'n': value.push_back('\n'); break;
            case 't': value.push_back('\t'); break;
            case 'r': case 'b': case 'f': break;
            case 'u':
                if (p + 4 < json.size()) {
                    unsigned cp = strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < json.size() && json[p + 1] == '\\' && json[p + 2] == 'u') {
                        unsigned lo = strtoul(json.substr(p + 3, 4).c_str(), nullptr, 16);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    AppendUtf8(value, cp);
                }
                break;
            default: value.push_back(e); break;  // \" \\ \/
        }
    }
    return true;
}

// Release notes are Markdown; the flyout shows plain text.
static std::wstring StripMarkdown(const std::wstring& md) {
    std::wstring out;
    bool lastBlank = true;  // also drops leading blank lines
    size_t start = 0;
    while (start <= md.size()) {
        size_t end = md.find(L'\n', start);
        if (end == std::wstring::npos) end = md.size();
        std::wstring t = md.substr(start, end - start);
        start = end + 1;

        size_t i = t.find_first_not_of(L" \t#");
        t = i == std::wstring::npos ? L"" : t.substr(i);
        if (t.rfind(L"- ", 0) == 0 || t.rfind(L"* ", 0) == 0) t = L"• " + t.substr(2);
        std::wstring clean;
        for (size_t k = 0; k < t.size(); k++) {
            if (t[k] == L'*' || t[k] == L'`') continue;
            if (t[k] == L'[') {  // [text](url) -> text
                size_t close = t.find(L"](", k);
                size_t stop = close == std::wstring::npos ? close : t.find(L')', close);
                if (stop != std::wstring::npos) {
                    clean += t.substr(k + 1, close - k - 1);
                    k = stop;
                    continue;
                }
            }
            clean.push_back(t[k]);
        }
        // Keep at most one blank line in a row.
        bool blank = clean.empty();
        if (!blank || !lastBlank) out += clean + L"\n";
        lastBlank = blank;
    }
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    while (!out.empty() && out.front() == L'\n') out.erase(0, 1);
    return out;
}

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool FetchLatest(ReleaseInfo& out) {
    std::string json;
    if (!HttpGet(kLatestApi, json)) return false;

    std::string tag, page;
    if (!JsonString(json, "tag_name", 0, tag)) return false;
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    JsonString(json, "html_url", 0, page);
    std::string body;
    if (JsonString(json, "body", 0, body)) out.notes = StripMarkdown(Widen(body));

    // Asset objects list "name" before "digest" and "browser_download_url".
    std::string name;
    size_t pos = 0, at = 0;
    while (JsonString(json, "name", pos, name, &at)) {
        pos = at + 1;
        if (_stricmp(name.c_str(), "VolumeOne.exe") != 0) continue;
        std::string url, digest;
        size_t urlAt = 0, digestAt = 0;
        if (!JsonString(json, "browser_download_url", at, url, &urlAt)) return false;
        if (JsonString(json, "digest", at, digest, &digestAt) && digestAt < urlAt && digest.rfind("sha256:", 0) == 0)
            out.sha256 = Widen(digest.substr(7));
        out.exeUrl = Widen(url);
        break;
    }
    if (out.exeUrl.empty()) return false;
    out.version = Widen(tag);
    out.pageUrl = page.empty() ? REPOSITORY_URL L"/releases" : Widen(page);
    return true;
}

static void ParseVersion(const std::wstring& s, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    swscanf_s(s.c_str(), L"%d.%d.%d", &v[0], &v[1], &v[2]);
}

bool IsNewer(const std::wstring& a, const std::wstring& b) {
    int x[3], y[3];
    ParseVersion(a, x);
    ParseVersion(b, y);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

static std::wstring Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    BYTE hash[32];
    bool ok = BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), hash, sizeof(hash)) == 0;
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    wchar_t hex[65];
    for (int i = 0; i < 32; i++) swprintf_s(hex + i * 2, 3, L"%02x", hash[i]);
    return hex;
}

bool Install(const ReleaseInfo& release) {
    std::string data;
    if (!HttpGet(release.exeUrl, data) || data.size() < 1024 || data[0] != 'M' || data[1] != 'Z') return false;
    if (!release.sha256.empty() && _wcsicmp(Sha256Hex(data).c_str(), release.sha256.c_str()) != 0) return false;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring newExe = std::wstring(exe) + L".new";
    std::wstring oldExe = std::wstring(exe) + L".old";

    HANDLE f = CreateFileW(newExe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(newExe.c_str());
        return false;
    }

    // A running exe can't be overwritten, but it can be renamed.
    DeleteFileW(oldExe.c_str());
    if (!MoveFileW(exe, oldExe.c_str())) {
        DeleteFileW(newExe.c_str());
        return false;
    }
    if (!MoveFileW(newExe.c_str(), exe)) {
        MoveFileW(oldExe.c_str(), exe);
        return false;
    }

    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " UPDATED_ARG L" " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MoveFileExW(oldExe.c_str(), exe, MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void FinishUpdate(DWORD oldPid) {
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, oldPid)) {
        WaitForSingleObject(old, 15000);
        CloseHandle(old);
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring oldExe = std::wstring(exe) + L".old";
    for (int i = 0; i < 10 && GetFileAttributesW(oldExe.c_str()) != INVALID_FILE_ATTRIBUTES; i++) {
        if (DeleteFileW(oldExe.c_str())) break;
        Sleep(500);
    }
}

}  // namespace Updater
