#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/shader_cache.h"
#include <shlobj.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nr {

namespace {

const uint32_t kMagic = 0x4353524Eu;   // "NRSC"
const uint32_t kVersion = 1;           // raise it when the way files are made or keyed changes

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t FnvStr(uint64_t h, const char* s) { const char zero = 0; h = Fnv(h, s ? s : "", s ? strlen(s) : 0); return Fnv(h, &zero, 1); }

bool Enabled() {
    static const bool on = [] { char b[16]; return !(GetEnvironmentVariableA("NR_SHADER_CACHE", b, 16) && atoi(b) == 0); }();
    return on;
}

std::wstring CacheDir() {
    static const std::wstring dir = [] {
        PWSTR base = nullptr; std::wstring d;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) d = std::wstring(base) + L"\\LSAddonManager\\shader-cache";
        if (base) CoTaskMemFree(base);
        if (!d.empty()) { SHCreateDirectoryExW(nullptr, d.c_str(), nullptr); }
        return d;
    }();
    return dir;
}

void Prune(const std::wstring& dir) {   // once a run: files whose last write is over 90 days old
    static bool done = false; if (done) return; done = true;
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((dir + L"\\*.cso").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const uint64_t n = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime, limit = 90ull * 24 * 3600 * 10000000ull;
    do {
        const uint64_t w = (static_cast<uint64_t>(f.ftLastWriteTime.dwHighDateTime) << 32) | f.ftLastWriteTime.dwLowDateTime;
        if (n > w && n - w > limit) DeleteFileW((dir + L"\\" + f.cFileName).c_str());
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

// A file used after 30 days gets its write time set to now, so that Prune (90 days since the last write) removes what is not used, not what is used once a quarter.
void TouchIfOld(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    const uint64_t n = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime, w = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    if (n <= w || n - w < 30ull * 24 * 3600 * 10000000ull) return;
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
}

bool Load(const std::wstring& path, std::vector<uint8_t>& payload) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}; std::vector<uint8_t> all; bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 24 && size.QuadPart < (64ll << 20);
    if (ok) { all.resize(static_cast<size_t>(size.QuadPart)); DWORD got = 0; ok = ReadFile(h, all.data(), static_cast<DWORD>(all.size()), &got, nullptr) && got == all.size(); }
    CloseHandle(h);
    if (!ok) return false;
    uint32_t magic, version; uint64_t length, sum;
    memcpy(&magic, all.data(), 4); memcpy(&version, all.data() + 4, 4); memcpy(&length, all.data() + 8, 8); memcpy(&sum, all.data() + 16, 8);
    if (magic != kMagic || version != kVersion || length != all.size() - 24 || Fnv(14695981039346656037ull, all.data() + 24, all.size() - 24) != sum) return false;
    payload.assign(all.begin() + 24, all.end());
    return true;
}

void Store(const std::wstring& path, const void* data, size_t n) {
    std::vector<uint8_t> all(24 + n);
    const uint64_t length = n, sum = Fnv(14695981039346656037ull, data, n);
    memcpy(all.data(), &kMagic, 4); memcpy(all.data() + 4, &kVersion, 4); memcpy(all.data() + 8, &length, 8); memcpy(all.data() + 16, &sum, 8); memcpy(all.data() + 24, data, n);
    const std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0; const bool ok = WriteFile(h, all.data(), static_cast<DWORD>(all.size()), &put, nullptr) && put == all.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temp.c_str());   // (written whole, then renamed: another process never sees half a file)
}

} // namespace

HRESULT CompileCached(const char* source, size_t length, const char* name, const D3D_SHADER_MACRO* defines, const char* entry, const char* target, UINT flags,
                      ID3DBlob** code, ID3DBlob** errors) {
    const std::wstring dir = Enabled() ? CacheDir() : std::wstring();
    if (dir.empty()) return D3DCompile(source, length, name, defines, nullptr, entry, target, flags, 0, code, errors);
    Prune(dir);
    uint64_t key = 14695981039346656037ull;
    key = Fnv(key, &kVersion, 4); key = Fnv(key, source, length); key = FnvStr(key, entry); key = FnvStr(key, target); key = Fnv(key, &flags, sizeof flags);
    for (const D3D_SHADER_MACRO* d = defines; d && d->Name; ++d) { key = FnvStr(key, d->Name); key = FnvStr(key, d->Definition); }
    wchar_t file[40]; swprintf(file, 40, L"\\%016llx.cso", static_cast<unsigned long long>(key));
    const std::wstring path = dir + file;
    std::vector<uint8_t> payload;
    if (Load(path, payload)) {
        TouchIfOld(path);
        ID3DBlob* blob = nullptr;
        if (SUCCEEDED(D3DCreateBlob(payload.size(), &blob))) {
            memcpy(blob->GetBufferPointer(), payload.data(), payload.size());
            *code = blob; if (errors) *errors = nullptr;
            return S_OK;
        }
    }
    const HRESULT hr = D3DCompile(source, length, name, defines, nullptr, entry, target, flags, 0, code, errors);
    if (SUCCEEDED(hr) && code && *code) Store(path, (*code)->GetBufferPointer(), (*code)->GetBufferSize());
    return hr;
}

} // namespace nr
