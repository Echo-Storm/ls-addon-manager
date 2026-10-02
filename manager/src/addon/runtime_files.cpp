#include "runtime_files.h"
#include "addon_security.h"
#include "../config/config_manager.h"
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <psapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

namespace eam {

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// The version resource: the fixed numbers (as "a.b.c", or "a.b.c.d" when the last is not 0) and two of the strings.
void ReadVersion(const std::wstring& path, RuntimeFile& f) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return;
    VS_FIXEDFILEINFO* fixed = nullptr; UINT len = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) && fixed && len >= sizeof(VS_FIXEDFILEINFO)) {
        const unsigned a = HIWORD(fixed->dwFileVersionMS), b = LOWORD(fixed->dwFileVersionMS), c = HIWORD(fixed->dwFileVersionLS), d = LOWORD(fixed->dwFileVersionLS);
        char text[48];
        if (d) snprintf(text, sizeof text, "%u.%u.%u.%u", a, b, c, d); else snprintf(text, sizeof text, "%u.%u.%u", a, b, c);
        f.version = text;
    }
    struct Lang { WORD language, codePage; }* langs = nullptr;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || !langs || len < sizeof(Lang)) return;
    auto text = [&](const wchar_t* key) {
        wchar_t query[128]; swprintf(query, 128, L"\\StringFileInfo\\%04x%04x\\%s", langs[0].language, langs[0].codePage, key);
        wchar_t* value = nullptr; UINT n = 0;
        return VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value), &n) && value && n ? Utf8(value) : std::string();
    };
    f.description = text(L"FileDescription");
    f.company = text(L"CompanyName");
}

// Authenticode, as Windows judges it, from what is on this computer only (no revocation check, no downloads): signed and intact, signed
// but changed since, or not signed. The signer's name when signed.
void ReadSignature(const std::wstring& path, RuntimeFile& f) {
    WINTRUST_FILE_INFO file{}; file.cbStruct = sizeof file; file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{}; data.cbStruct = sizeof data; data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE; data.pFile = &file; data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (result == ERROR_SUCCESS) f.signature = RuntimeFile::Signature::Signed;
    else if (result == TRUST_E_NOSIGNATURE || result == TRUST_E_SUBJECT_FORM_UNKNOWN || result == TRUST_E_PROVIDER_UNKNOWN) f.signature = RuntimeFile::Signature::Unsigned;
    else if (result == TRUST_E_BAD_DIGEST || result == CRYPT_E_HASH_VALUE) f.signature = RuntimeFile::Signature::Broken;
    else f.signature = RuntimeFile::Signature::Unsigned;   // expired, untrusted root, ...: not a signature to rely on
    if (f.signature != RuntimeFile::Signature::Unsigned && data.hWVTStateData) {
        if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
                if (signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
                    wchar_t name[256] = {};
                    CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                    f.signer = Utf8(name);
                }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
}

// What is known of each file, by path: read again when its size or time changes. The reading runs on a thread of its own (a 34 MB file's
// SHA-256 and signature take a moment); the list holds what was read last. Never freed: nothing may be joined as the process ends.
struct Cached { uint64_t size = 0; FILETIME written{}; bool exists = false; RuntimeFile file; bool reading = false; ULONGLONG checkedAt = 0;
                bool loaded = false; ULONGLONG loadedAt = 0; };   // loaded: judged at most every two seconds (it opens files)
struct Cache { std::mutex mutex; std::map<std::wstring, Cached> byPath; std::vector<std::wstring> modules; ULONGLONG modulesAt = 0; };

// A file as Windows knows it (its volume and its number there): the same for every spelling of its path (slashes, 8.3 names, \\?\, case),
// which is how a loader may have written the path it loaded a DLL from (NVIDIA's writes "...\dlss/nvngx_dlss.dll").
struct FileId { DWORD volume = 0, high = 0, low = 0; bool valid = false; bool operator==(const FileId& o) const { return valid && o.valid && volume == o.volume && high == o.high && low == o.low; } };
FileId IdOf(const std::wstring& path) {
    FileId id;
    const HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return id;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info)) { id.volume = info.dwVolumeSerialNumber; id.high = info.nFileIndexHigh; id.low = info.nFileIndexLow; id.valid = true; }
    CloseHandle(h);
    return id;
}
std::wstring BaseName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}
Cache& TheCache() { static Cache* c = new Cache; return *c; }

std::wstring Lower(std::wstring s) { for (wchar_t& ch : s) ch = (wchar_t)towlower(ch); return s; }

// The DLLs loaded in this process (Lossless Scaling's, the manager living in it), by full path in lower case: read at most every two seconds.
const std::vector<std::wstring>& LoadedModules(Cache& cache, ULONGLONG now) {
    if (cache.modulesAt && now - cache.modulesAt < 2000) return cache.modules;
    cache.modulesAt = now;
    cache.modules.clear();
    HMODULE mods[1024]; DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes))
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
            wchar_t name[MAX_PATH];
            if (GetModuleFileNameW(mods[i], name, MAX_PATH)) cache.modules.push_back(Lower(name));
        }
    return cache.modules;
}

} // namespace

std::string RuntimeFile::ShownVersion() const {
    if (shipped && !shippedLabel.empty()) return shippedLabel;
    std::string v = version;
    if (std::count(v.begin(), v.end(), '.') == 3) v.resize(v.rfind('.'));   // 4.1.1.2740 -> 4.1.1: the build number is in the tooltip
    return v;
}

std::string RuntimeFile::FileName() const { return Utf8(std::filesystem::path(defaultPath).filename().wstring()); }

void InspectRuntimeFile(const std::wstring& path, RuntimeFile& f) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    f.exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    f.read = true;
    if (!f.exists) return;
    f.size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    ReadVersion(path, f);
    ReadSignature(path, f);
    f.sha256 = AddonSecurity::ComputeSHA256(path);
}

bool ReadDllExports(const std::wstring& path, std::vector<std::string>& names) {
    names.clear();
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto at = [&](size_t off, size_t n) { return off <= data.size() && n <= data.size() - off ? data.data() + off : nullptr; };   // (offsetOf's (size_t)-1 for an address outside every section must not wrap around)
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(at(0, sizeof(IMAGE_DOS_HEADER)));
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(at((size_t)dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64)));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) return false;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        at((size_t)dos->e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader, sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections));
    if (!sections) return false;
    auto offsetOf = [&](DWORD rva) -> size_t {   // a virtual address in the loaded image -> where it is in the file
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const IMAGE_SECTION_HEADER& sec = sections[i];
            if (rva >= sec.VirtualAddress && rva < sec.VirtualAddress + (std::max)(sec.Misc.VirtualSize, sec.SizeOfRawData)) return rva - sec.VirtualAddress + sec.PointerToRawData;
        }
        return (size_t)-1;
    };
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return true;   // a DLL with no exports
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(at(offsetOf(dir.VirtualAddress), sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (!exp) return false;
    const auto* nameRvas = reinterpret_cast<const DWORD*>(at(offsetOf(exp->AddressOfNames), sizeof(DWORD) * exp->NumberOfNames));
    if (!nameRvas) return false;
    for (DWORD i = 0; i < exp->NumberOfNames && i < 100000; ++i) {
        const size_t off = offsetOf(nameRvas[i]);
        std::string name;
        for (size_t k = off; k < data.size() && data[k] && name.size() < 256; ++k) name += data[k];
        names.push_back(name);
    }
    return true;
}

namespace {

// The row's file facts from the cache, the file read again (on a thread of its own) when it is new or changed.
void FillFromCache(RuntimeFile& row, const std::string& shippedSha256, ULONGLONG now) {
    Cache& cache = TheCache();
    const std::wstring path = row.path;
    std::lock_guard<std::mutex> lock(cache.mutex);
    Cached& c = cache.byPath[path];
    if (now - c.checkedAt >= 2000) {   // a new file in its place (a swap by hand, an update) is noticed within two seconds
        c.checkedAt = now;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        const uint64_t size = exists ? (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0;
        const bool changed = !c.file.read || exists != c.exists || size != c.size || CompareFileTime(&a.ftLastWriteTime, &c.written) != 0;
        if (changed && !c.reading) {
            c.exists = exists; c.size = size; c.written = a.ftLastWriteTime; c.reading = true;
            std::thread([path] {
                RuntimeFile f; InspectRuntimeFile(path, f);
                Cache& cc = TheCache();
                std::lock_guard<std::mutex> l(cc.mutex);
                Cached& done = cc.byPath[path]; done.file = f; done.reading = false;
                done.checkedAt = 0;   // look again at once: it may have changed while it was read
            }).detach();
        }
    }
    const RuntimeFile& known = c.file;
    // loaded: a module of this process is this very file (by the file's identity; only modules of the same name are looked at)
    const std::vector<std::wstring>& modules = LoadedModules(cache, now);
    if (!c.loadedAt || now - c.loadedAt >= 2000) {
        c.loadedAt = now; c.loaded = false;
        if (c.exists) {
            const std::wstring name = Lower(BaseName(path));
            FileId mine; bool mineRead = false;
            for (const std::wstring& module : modules) {
                if (Lower(BaseName(module)) != name) continue;
                if (!mineRead) { mine = IdOf(path); mineRead = true; }
                if (IdOf(module) == mine) { c.loaded = true; break; }
            }
        }
    }
    row.loaded = c.loaded;
    row.exists = c.exists; row.read = known.read;
    row.signature = known.signature; row.version = known.version; row.description = known.description; row.company = known.company;
    row.signer = known.signer; row.sha256 = known.sha256; row.size = known.size;
    row.shipped = row.shippedKnown && !known.sha256.empty() && _stricmp(known.sha256.c_str(), shippedSha256.c_str()) == 0;
}

std::wstring LibraryDir(const RuntimeFile& slot) { return slot.addonDir + L"\\runtimes\\" + Wide(slot.label); }

// the shipped file's SHA-256 of a slot, kept by label (DescribeRuntimeFile has only the row)
std::map<std::string, std::string>& ShippedHashes() { static auto* m = new std::map<std::string, std::string>; return *m; }

} // namespace

std::vector<RuntimeFile> RuntimeFiles(const std::vector<AddonInfo>& addons, const std::wstring& lsDir) {
    std::vector<RuntimeFile> rows;
    const ULONGLONG now = GetTickCount64();
    for (const AddonInfo& addon : addons) {
        for (const AddonManifest::Runtime& slot : addon.manifest.runtimes) {
            RuntimeFile row;
            row.label = slot.name; row.addonId = addon.id; row.addonName = addon.GetDisplayName(); row.addonOn = addon.enabled;
            row.configKey = slot.configKey; row.exportsNeeded = slot.exports;
            row.shippedLabel = slot.shippedLabel; row.shippedKnown = !slot.shippedSha256.empty();
            row.addonDir = std::filesystem::path(addon.dllPath).parent_path().wstring();
            // the default: the manifest's path ("{ls}/..." for Lossless Scaling's folder, else inside the addon's)
            std::wstring def;
            if (slot.file.rfind("{ls}", 0) == 0) def = lsDir + Wide(slot.file.substr(4));
            else def = (std::filesystem::path(row.addonDir) / Wide(slot.file)).wstring();
            for (wchar_t& ch : def) if (ch == L'/') ch = L'\\';
            row.defaultPath = def;
            // in use: the addon's setting when it names a file, else the default
            const std::string chosen = slot.configKey.empty() ? std::string() : ConfigManager::Instance().Get(addon.id, slot.configKey, "");
            std::wstring path = chosen.empty() ? def : Wide(chosen);
            for (wchar_t& ch : path) if (ch == L'/') ch = L'\\';
            row.path = path;
            row.usingDefault = _wcsicmp(path.c_str(), def.c_str()) == 0;
            { static std::mutex m; std::lock_guard<std::mutex> l(m); ShippedHashes()[addon.id + "/" + slot.name] = slot.shippedSha256; }
            FillFromCache(row, slot.shippedSha256, now);
            rows.push_back(row);
        }
    }
    return rows;
}

RuntimeFile DescribeRuntimeFile(const std::wstring& path, const RuntimeFile& like) {
    RuntimeFile row = like;
    row.path = path;
    row.usingDefault = _wcsicmp(path.c_str(), like.defaultPath.c_str()) == 0;
    std::string shipped;
    { const auto it = ShippedHashes().find(like.addonId + "/" + like.label); if (it != ShippedHashes().end()) shipped = it->second; }
    FillFromCache(row, shipped, GetTickCount64());
    return row;
}

std::vector<std::wstring> RuntimeLibrary(const RuntimeFile& slot) {
    struct Entry { std::wstring path; std::filesystem::file_time_type when; };
    std::vector<Entry> found;
    std::error_code ec;
    for (const auto& dir : std::filesystem::directory_iterator(LibraryDir(slot), ec)) {
        if (!dir.is_directory(ec)) continue;
        const std::filesystem::path file = dir.path() / Wide(slot.FileName());
        if (std::filesystem::exists(file, ec)) found.push_back({ file.wstring(), std::filesystem::last_write_time(dir.path(), ec) });
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.when > b.when; });
    std::vector<std::wstring> paths;
    for (const Entry& e : found) paths.push_back(e.path);
    return paths;
}

std::string RuntimeFileTitle(const std::wstring& path) {
    std::ifstream in(std::filesystem::path(path).parent_path() / L"ABOUT.txt");
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);   // a UTF-8 byte order mark
    return line.size() > 60 ? line.substr(0, 60) : line;
}

bool AddRuntimeFile(const RuntimeFile& slot, const std::wstring& source, std::wstring& added, std::string& error) {
    std::vector<std::string> exports;
    if (!ReadDllExports(source, exports)) { error = "That's not a 64-bit DLL, so it can't be a " + slot.label + " file."; return false; }
    for (const std::string& need : slot.exportsNeeded)
        if (std::find(exports.begin(), exports.end(), need) == exports.end()) {
            error = "That's not a " + slot.label + " file: it doesn't have the functions " + slot.addonName + " needs.";
            return false;
        }
    const std::string sha = AddonSecurity::ComputeSHA256(source);
    if (sha.size() < 8) { error = "The file could not be read."; return false; }
    const std::filesystem::path dir = std::filesystem::path(LibraryDir(slot)) / Wide(sha.substr(0, 8));
    const std::filesystem::path dest = dir / Wide(slot.FileName());
    std::error_code ec;
    if (std::filesystem::exists(dest, ec) && AddonSecurity::ComputeSHA256(dest.wstring()) == sha) { added = dest.wstring(); return true; }   // added before
    std::filesystem::create_directories(dir, ec);
    if (!CopyFileW(source.c_str(), dest.c_str(), FALSE)) { error = "The file could not be copied (error " + std::to_string(GetLastError()) + ")."; return false; }
    added = dest.wstring();
    return true;
}

void UseRuntimeFile(const RuntimeFile& slot, const std::wstring& path) {
    if (slot.configKey.empty()) return;
    const bool isDefault = path.empty() || _wcsicmp(path.c_str(), slot.defaultPath.c_str()) == 0;
    ConfigManager::Instance().Set(slot.addonId, slot.configKey, isDefault ? std::string() : Utf8(path));
    ConfigManager::Instance().Save();
}

bool RemoveRuntimeFile(const RuntimeFile& slot, const std::wstring& path, std::string& error) {
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (_wcsicmp(dir.parent_path().wstring().c_str(), LibraryDir(slot).c_str()) != 0) { error = "Only files added here can be removed here."; return false; }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32]; swprintf(stamp, 32, L"-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::filesystem::path aside = std::filesystem::path(slot.addonDir) / L"runtimes" / L".removed" / (Wide(slot.label) + L"-" + dir.filename().wstring() + stamp);
    std::error_code ec;
    std::filesystem::create_directories(aside.parent_path(), ec);
    if (!MoveFileExW(dir.c_str(), aside.c_str(), 0)) {
        error = "It's in use right now: switch to another file, then remove it.";
        return false;
    }
    if (_wcsicmp(path.c_str(), slot.path.c_str()) == 0) UseRuntimeFile(slot, L"");   // it was the one in use: back to the default
    return true;
}

} // namespace eam
