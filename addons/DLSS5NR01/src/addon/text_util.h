// Small text helpers shared by the recorder code (the manager's recorder service and feature, and addon/recorder.cpp): UTF-8 and wide strings, and a JSON string escaper.
#pragma once
#include <windows.h>
#include <cstdio>
#include <string>

namespace nr {
namespace util {

inline std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return w;
}

inline std::string Utf8(const std::wstring& w) {
    std::string s(w.size() * 3 + 1, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return s;
}

// The text as a JSON string with its quotes: quotes, backslashes and control characters escaped.
inline std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

} // namespace util
} // namespace nr
