#include "strings.h"

#include <map>

namespace {

// Parses "key = value" lines (UTF-8). \n in a value becomes a line break.
std::map<std::string, std::wstring> Parse(const char* data, DWORD size) {
    std::map<std::string, std::wstring> out;
    std::string text(data, size);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // BOM
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        for (size_t i; (i = value.find("\\n")) != std::string::npos;) value.replace(i, 2, "\n");
        int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), w.data(), n);
        out[key] = std::move(w);
    }
    return out;
}

std::map<std::string, std::wstring> LoadLanguage(const wchar_t* code) {
    HRSRC res = FindResourceW(nullptr, code, L"LANG");
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    const char* data = mem ? (const char*)LockResource(mem) : nullptr;
    if (!data) return {};
    return Parse(data, SizeofResource(nullptr, res));
}

// "es-ES" -> "ES": resource names are the language part, upper case.
std::wstring UiLanguageCode() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = L"en";
    LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0);
    std::wstring code = name;
    code = code.substr(0, code.find(L'-'));
    CharUpperBuffW(code.data(), (DWORD)code.size());
    return code;
}

}  // namespace

const Strings& S() {
    static std::map<std::string, std::wstring> table;
    static Strings strings = [] {
        table = LoadLanguage(L"EN");
        std::wstring code = UiLanguageCode();
        if (code != L"EN")
            for (auto& [key, value] : LoadLanguage(code.c_str())) table[key] = value;  // missing keys stay English
        Strings s{};
        // A key missing everywhere shows its own name instead of nothing.
        auto get = [](const char* key) -> const wchar_t* {
            auto& v = table[key];
            if (v.empty()) v.assign(key, key + strlen(key));
            return v.c_str();
        };
#define X(key) s.key = get(#key);
        STRING_KEYS(X)
#undef X
        return s;
    }();
    return strings;
}

std::wstring Fmt(const wchar_t* text, const std::wstring& value) {
    std::wstring s = text;
    size_t i = s.find(L"{0}");
    if (i != std::wstring::npos) s.replace(i, 3, value);
    return s;
}
