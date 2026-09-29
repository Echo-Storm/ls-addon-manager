// The folders NVIDIA's NGX core searches for its runtimes and models, the same for every addon of ours in the process.
//
// NGX is one core per process (the driver's _nvngx.dll) and keeps the search paths of the FIRST Init. With Neural Rendering and the DLSS Upscaler
// both on, whichever started first fixed them: Neural Rendering's were Lossless Scaling's folder and its own, so the DLSS Upscaler's
// "FeatureNotFound (nvngx_dlss.dll missing?)" although the file was there (issue #7; reproduced with nr_sreval ngxfirst=1); the other way round
// Neural Rendering would not have found nvngx_dlssnr.dll. So each Init passes the union: its own folders first, then Lossless Scaling's folder
// (the model), Neural Rendering's, and the DLSS runtime's (the one the DLSS Upscaler is using, told through an environment variable of the
// process; else its default place).
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace nr::ngxpaths {

inline constexpr const wchar_t* kDlssRuntimeEnv = L"LSAM_DLSS_RUNTIME";

inline bool FolderExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The folder of the process's own executable: inside Lossless Scaling, its folder.
inline std::wstring ProcessDir() {
    wchar_t b[2 * MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, b, 2 * MAX_PATH);
    std::wstring s(b, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}

// The DLSS Upscaler says where its runtime is (before it starts NGX), for whichever addon starts NGX first.
inline void PublishDlssRuntime(const std::wstring& dir) { SetEnvironmentVariableW(kDlssRuntimeEnv, dir.c_str()); }

inline std::wstring PublishedDlssRuntime() {
    wchar_t b[2 * MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(kDlssRuntimeEnv, b, 2 * MAX_PATH);
    return n > 0 && n < 2 * MAX_PATH ? std::wstring(b, n) : std::wstring();
}

// `first`: the caller's own folders (kept as they are, in front); then the rest, each once and only where it exists.
inline std::vector<std::wstring> SearchList(const std::vector<std::wstring>& first) {
    std::vector<std::wstring> list;
    auto has = [&](const std::wstring& p) { for (const std::wstring& q : list) if (_wcsicmp(q.c_str(), p.c_str()) == 0) return true; return false; };
    for (const std::wstring& p : first) if (!p.empty() && !has(p)) list.push_back(p);
    const std::wstring ls = ProcessDir();
    std::wstring dlss = PublishedDlssRuntime();
    if (dlss.empty() && !ls.empty()) dlss = ls + L"\\addons\\DLSS4DLAA\\dlss";
    const std::wstring more[] = { ls, ls.empty() ? std::wstring() : ls + L"\\addons\\DLSS5NR01", dlss };
    for (const std::wstring& p : more) if (!p.empty() && FolderExists(p) && !has(p)) list.push_back(p);
    return list;
}

// The list as the C array NGX takes (valid as long as `list` is).
inline std::vector<const wchar_t*> AsArray(const std::vector<std::wstring>& list) {
    std::vector<const wchar_t*> a;
    for (const std::wstring& p : list) a.push_back(p.c_str());
    return a;
}

} // namespace nr::ngxpaths
