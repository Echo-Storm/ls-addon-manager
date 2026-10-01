#include "requirements.h"
#include "forwarder/nr_api.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <dxgi.h>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

namespace req {

namespace {

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
std::string After(const std::string& s, const char* marker) {
    const size_t at = s.find(marker);
    return at == std::string::npos ? std::string() : s.substr(at + strlen(marker));
}

Row MakeRow(const char* label, Level level, std::string value, std::string hint = "") {
    Row r;
    r.label = label;
    r.level = level;
    r.value = std::move(value);
    r.hint = std::move(hint);
    return r;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// "a.b.c.d" of a file's fixed version resource; empty when it has none.
// The first 16 hex digits of the file's SHA-256, or empty (the file cannot be read, or the hash cannot be made).
std::string FileHash16(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20); DWORD n = 0; bool ok = true;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n) if (BCryptHashData(h, buf.data(), n, 0) != 0) { ok = false; break; }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(h, digest, sizeof digest, 0) == 0) { char hex[40]; for (int i = 0; i < 8; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]); out = hex; }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

std::string FileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return std::string();
    std::string block(size, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return std::string();
    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &len) || !fixedInfo || len < sizeof(VS_FIXEDFILEINFO)) return std::string();
    char v[64];
    snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fixedInfo->dwFileVersionMS), LOWORD(fixedInfo->dwFileVersionMS), HIWORD(fixedInfo->dwFileVersionLS), LOWORD(fixedInfo->dwFileVersionLS));
    return v;
}

bool FileSize(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

std::wstring RegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return std::wstring();
    wchar_t buf[1024] = {};
    DWORD bytes = sizeof buf - sizeof(wchar_t), type = 0;
    const bool ok = RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    return ok ? std::wstring(buf) : std::wstring();
}

} // namespace

std::string DriverFromNgxVersion(const std::string& fileVersion) {
    // NVIDIA's file versions end with the driver number: 32.0.16.1692 is driver 616.92, 31.0.15.5222 is 552.22.
    unsigned a, b, c, d;
    char extra;
    if (sscanf(fileVersion.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return std::string();
    char digits[32];
    snprintf(digits, sizeof digits, "%u%04u", c % 10, d);   // the last digit of the third part, then the fourth part padded to four
    const std::string s = digits;
    return s.substr(0, s.size() - 2) + "." + s.substr(s.size() - 2);
}

std::string VersionShort(const std::string& fileVersion) {
    std::string out;
    int parts = 0;
    for (const char c : fileVersion) {
        if (c == '.' || c == ',') { if (++parts == 2) break; out += '.'; }
        else if (c != ' ') out += c;
    }
    return out;
}

std::string SizeText(uint64_t bytes) {
    char b[48];
    if (bytes == 0) return "0 MB";
    snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return b;
}

std::string PlainEngineError(const std::string& raw) {
    if (StartsWith(raw, "snippet probe")) return "the model file could not be loaded (it may be the wrong file or damaged)";
    if (StartsWith(raw, "forwarder LoadLibrary")) return "the helper DLL could not be loaded";
    if (StartsWith(raw, "forwarder export")) return "the helper DLL is from a different version of this addon";
    if (StartsWith(raw, "NGX core Init")) return "the NVIDIA NGX core did not start: " + After(raw, ": ");
    if (StartsWith(raw, "snippet Init_Ext")) return "the model file refused to start: " + After(raw, ": ");
    if (StartsWith(raw, "D3D12CreateDevice")) return "could not create a Direct3D 12 device on this graphics card";
    if (StartsWith(raw, "CreateFeature") && Has(raw, "FeatureNotSupported"))
        return "this model file cannot run on your graphics card (some builds only support newer RTX cards; on RTX 20, 30 and 40 cards only a build made for them works)";
    if (StartsWith(raw, "CreateFeature")) return "the model could not create its Neural Rendering feature: " + After(raw, ": ");
    return raw;
}

Report Evaluate(const Inputs& in) {
    Report rep;

    // Graphics card
    if (in.nvidiaFound) {
        std::string v = in.gpuName;
        if (in.gpuMemoryBytes) v += " (" + std::to_string(static_cast<unsigned long long>((in.gpuMemoryBytes + (1ull << 29)) >> 30)) + " GB)";
        rep.rows.push_back(MakeRow("Graphics card", Level::Ok, v));
    } else {
        rep.rows.push_back(MakeRow("Graphics card", Level::Missing, "no NVIDIA graphics card found",
                                   "DLSS 5 Neural Rendering runs only on an NVIDIA RTX graphics card."));
    }

    // NVIDIA driver (its NGX core)
    if (in.ngxRegistered && in.ngxCoreFound) {
        const std::string drv = DriverFromNgxVersion(in.ngxCoreVersion);
        std::string v = drv.empty() ? "NGX core " + (in.ngxCoreVersion.empty() ? std::string("found") : in.ngxCoreVersion)
                                    : "driver " + drv + " (_nvngx.dll " + in.ngxCoreVersion + ")";
        rep.rows.push_back(MakeRow("NVIDIA driver", Level::Ok, v));
    } else {
        rep.rows.push_back(MakeRow("NVIDIA driver", Level::Missing,
                                   in.ngxRegistered ? "the driver names its NGX core, but _nvngx.dll is missing" : "the NVIDIA driver's NGX core is not registered",
                                   "Install or repair the NVIDIA graphics driver (a clean install). The NGX core, _nvngx.dll, is part of it."));
    }

    // Model file
    if (!in.modelFound) {
        rep.rows.push_back(MakeRow("Model file", Level::Missing, "not found: " + in.modelPath,
                                   "Put your copy of nvngx_dlssnr.dll in the Lossless Scaling folder, next to LosslessScaling.exe, or set its path under Advanced. "
                                   "It is not included with this addon, and this project does not say where to get it."));
    } else if (in.modelSize < kSmallestPlausibleModel) {
        rep.rows.push_back(MakeRow("Model file", Level::Missing, "found, but only " + SizeText(in.modelSize) + ": too small to be the model",
                                   "The file looks damaged or is not the model (the model is about 150 MB). Replace it with a complete copy."));
    } else {
        const std::string ver = VersionShort(in.modelVersion);
        const bool tested = ver == kTestedModelVersion && in.modelSize == kTestedModelSize;
        const std::string what = "version " + (ver.empty() ? std::string("unknown") : ver) + ", " + SizeText(in.modelSize);
        bool known = false; for (const KnownModel& k : kKnownModels) if (!in.modelHash.empty() && in.modelHash == k.hash) known = true;
        if (known) rep.rows.push_back(MakeRow("Model file", Level::Ok, what + ": a build seen working (file " + in.modelHash + ")"));
        else if (tested && !in.modelHash.empty())   // the number and size of the tested build, another file: models that are called the same differ
            rep.rows.push_back(MakeRow("Model file", Level::Note, what + ": the same version and size as the build this addon was tested with, but not the same file (" + in.modelHash + ")",
                                       "Builds of the model with the same name and version are not all alike. If the compatibility test fails, this file is the first thing to check: docs/model-compatibility.md lists the files seen working."));
        else if (tested) rep.rows.push_back(MakeRow("Model file", Level::Ok, what + ": the build this addon was tested with"));
        else rep.rows.push_back(MakeRow("Model file", Level::Note, what + ": not the build this addon was tested with (" + std::string(kTestedModelVersion) + ")",
                                        "It may still work. If the engine fails to start, this file is the first thing to check."));
    }

    // Helper DLL
    if (in.helperFound) rep.rows.push_back(MakeRow("Helper DLL", Level::Ok, "present"));
    else rep.rows.push_back(MakeRow("Helper DLL", Level::Missing, "nvngx.dll_dlss5nr01.dll is missing from the addon folder",
                                    "The addon folder is incomplete: copy nvngx.dll_dlss5nr01.dll from the release zip next to DLSS5NR01.dll."));

    // Compatibility test
    switch (in.selfTest) {
    case SelfTestState::NotRun:
        rep.rows.push_back(MakeRow("Compatibility test", Level::Ok, in.modelFound ? "not run yet: press Test compatibility to try the model on this graphics card" : "needs the model file first"));
        break;
    case SelfTestState::Running:
        rep.rows.push_back(MakeRow("Compatibility test", Level::Note, "running: it loads the model and tries it on the graphics card (a few seconds)", "Wait for it to finish; the result appears here."));
        break;
    case SelfTestState::Passed:
        rep.rows.push_back(MakeRow("Compatibility test", Level::Ok, in.selfTestText.empty() ? std::string("passed") : in.selfTestText));
        break;
    case SelfTestState::Failed: {
        const std::string& k = in.selfTestKey;
        std::string hint;
        if (k == "NOT_SUPPORTED") hint = "This model file cannot run on your graphics card. Try another build of it: on RTX 20, 30 and 40 cards only a build made for them works.";
        else if (k == "MODEL_LOAD") hint = "Check the Model file row above, then test again.";
        else if (k == "HELPER" || k == "NOT_FOUND") hint = "The addon folder is incomplete: copy nvngx.dll_dlss5nr01.dll and nr_selftest.exe from the release zip next to DLSS5NR01.dll.";
        else if (k == "NGX_CORE") hint = "Install or repair the NVIDIA graphics driver (a clean install).";
        else if (k == "D3D12" || k == "NO_GPU") hint = "DLSS 5 Neural Rendering needs an NVIDIA RTX graphics card with Direct3D 12.";
        else if (k == "FLOAT_SLOT" || k == "MODEL_INIT" || k == "FEATURE" || k == "EVALUATE" || k == "UNCHANGED")
            hint = "This model file did not work on this graphics card. If you have another build of it, try that one; the log has the details.";
        else if (k == "CRASH" || k == "UNEXPECTED" || k == "TIMEOUT")
            hint = "The test did not finish normally. Try again; if it repeats, the model file or the graphics driver is the likely cause. The log has the details.";
        else hint = "The log (logs\\DLSS5NR01.log in the Lossless Scaling folder) has the details.";
        rep.rows.push_back(MakeRow("Compatibility test", Level::Missing, in.selfTestText.empty() ? std::string("failed") : in.selfTestText, hint));
        break;
    }
    }

    // Engine
    bool problemAbove = false;
    for (const auto& r : rep.rows) if (r.level == Level::Missing) problemAbove = true;
    switch (in.engine) {
    case EngineState::NotStarted: rep.rows.push_back(MakeRow("Engine", Level::Ok, "not started yet: it starts when Lossless Scaling scales a game")); break;
    case EngineState::Ready:      rep.rows.push_back(MakeRow("Engine", Level::Ok, "ready")); break;
    case EngineState::Running:    rep.rows.push_back(MakeRow("Engine", Level::Ok, "running")); break;
    case EngineState::Failed:
        rep.rows.push_back(MakeRow("Engine", Level::Missing, in.engineError.empty() ? std::string("failed to start") : PlainEngineError(in.engineError),
                                   problemAbove ? "See the problem above; press Restart engine under Advanced once it is fixed."
                                                : "Press Restart engine under Advanced after fixing it. The log (logs\\DLSS5NR01.log in the Lossless Scaling folder) has the details."));
        break;
    }

    // Overall and the one-line headline: the first problem, else the first note
    for (const auto& r : rep.rows) {
        if (r.level == Level::Missing) { rep.overall = Level::Missing; rep.headline = r.label + ": " + r.value; break; }
    }
    if (rep.overall == Level::Ok) {
        for (const auto& r : rep.rows) {
            if (r.level == Level::Note) { rep.overall = Level::Note; rep.headline = r.label + ": " + r.value; break; }
        }
    }
    if (rep.overall == Level::Ok) rep.headline = "Everything Neural Rendering needs is in place.";
    return rep;
}

// ---- the compatibility self-test

ProcessResult RunProcess(const std::wstring& commandLine, unsigned timeoutMs) {
    ProcessResult r;
    wchar_t tempDir[MAX_PATH] = {}, tempFile[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    GetTempFileNameW(tempDir, L"nrs", 0, tempFile);

    // What the program prints goes to a temporary file (a pipe would have to be drained while waiting, and a full pipe would block it)
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, nullptr, TRUE };
    HANDLE out = CreateFileW(tempFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) {
        r.startError = GetLastError();
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        DeleteFileW(tempFile);
        return r;
    }

    // A job that ends the program if this process ends first, so a test never outlives Lossless Scaling
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }

    // The program is started normally, already inside the job (Windows 10 and later take the job as a creation attribute). It is deliberately not
    // started suspended and resumed: that is how malware starts processes it wants to tamper with, and security software watches for it.
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul; si.StartupInfo.hStdOutput = out; si.StartupInfo.hStdError = out;
    std::vector<unsigned char> attributeStorage;
    bool jobAtCreation = false, attributesReady = false;
    if (job) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);   // fails on purpose: it reports the size needed
        attributeStorage.resize(bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
            attributesReady = true;
            HANDLE jobs[1] = { job };
            if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof jobs, nullptr, nullptr)) {
                si.lpAttributeList = attributes;
                jobAtCreation = true;
            }
        }
    }
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = commandLine;   // CreateProcess may write into it
    BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (jobAtCreation ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr, &si.StartupInfo, &pi);
    if (!created && jobAtCreation) {   // a system that does not know the job attribute: start it plainly and put it in the job straight after
        si.lpAttributeList = nullptr;
        jobAtCreation = false;
        created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si.StartupInfo, &pi);
    }
    if (attributesReady) DeleteProcThreadAttributeList(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data()));
    if (created) {
        if (job && !jobAtCreation) AssignProcessToJobObject(job, pi.hProcess);   // without it (already in a job that forbids nesting) the program just is not ended with us
        r.started = true;
        if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT) {
            r.timedOut = true;
            if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = code;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        r.startError = GetLastError();
    }
    if (job) CloseHandle(job);   // ends anything the program left running
    CloseHandle(out);
    CloseHandle(nul);

    // what it printed, the last 256 KB of it
    HANDLE in = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size = {};
        GetFileSizeEx(in, &size);
        const LONGLONG keep = 256 * 1024;
        if (size.QuadPart > keep) { LARGE_INTEGER at; at.QuadPart = size.QuadPart - keep; SetFilePointerEx(in, at, nullptr, FILE_BEGIN); }
        r.output.resize(static_cast<size_t>(size.QuadPart > keep ? keep : size.QuadPart));
        DWORD got = 0;
        if (!r.output.empty() && !ReadFile(in, r.output.data(), static_cast<DWORD>(r.output.size()), &got, nullptr)) got = 0;
        r.output.resize(got);
        CloseHandle(in);
    }
    DeleteFileW(tempFile);
    return r;
}

SelfTestResult ParseSelfTest(const std::string& output, unsigned long exitCode, bool timedOut) {
    SelfTestResult r;
    if (timedOut) {
        r.key = "TIMEOUT";
        r.text = "the test took too long and was stopped";
        return r;
    }
    // the verdict is the last line that starts with SELFTEST: the NGX core may still print after it
    std::string verdict;
    for (size_t pos = 0; pos < output.size();) {
        size_t end = output.find('\n', pos);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(pos, end - pos);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("SELFTEST ", 0) == 0) verdict = line;
        pos = end + 1;
    }
    char buf[64];
    if (verdict.empty()) {
        snprintf(buf, sizeof buf, "0x%08lX", exitCode);
        if (exitCode >= 0xC0000000ul) { r.key = "CRASH"; r.text = std::string("the test program crashed (exit code ") + buf + "), so the model or the graphics driver failed when it was tried"; }
        else { r.key = "UNEXPECTED"; r.text = std::string("the test program ended without a result (exit code ") + buf + ")"; }
        return r;
    }
    int code = -1;
    char key[64] = {};
    int consumed = 0;
    if (sscanf(verdict.c_str() + 9, "%d %63s%n", &code, key, &consumed) < 2) {
        r.key = "UNEXPECTED";
        r.text = "the test program's result could not be read";
        return r;
    }
    if (static_cast<unsigned long>(code) != exitCode) {
        r.key = "UNEXPECTED";
        r.text = "the test program's result and its exit code do not agree";
        return r;
    }
    r.code = code;
    r.key = key;
    const size_t textAt = 9 + static_cast<size_t>(consumed);
    r.text = textAt < verdict.size() ? verdict.substr(textAt) : std::string();
    while (!r.text.empty() && r.text.front() == ' ') r.text.erase(0, 1);
    r.passed = code == 0 && r.key == "PASS";
    return r;
}

SelfTestResult RunSelfTest(const std::wstring& addonDir, const std::wstring& modelPath, const std::wstring& lsDir, unsigned timeoutMs, const std::wstring& luid) {
    SelfTestResult r;
    const std::wstring exe = addonDir + L"\\nr_selftest.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        r.key = "NOT_FOUND";
        r.text = "the compatibility test program (nr_selftest.exe) is missing from the addon folder";
        return r;
    }
    DeleteFileW((addonDir + L"\\compatibility_report.txt").c_str());   // a report left by an earlier run must not be taken for this one's (a crashed test writes none)
    const ProcessResult p = RunProcess(L"\"" + exe + L"\" --model \"" + modelPath + L"\" --lsdir \"" + lsDir + L"\" --report \"" + addonDir + L"\\compatibility_report.txt\"" +
                                       (luid.empty() ? std::wstring() : L" --luid " + luid), timeoutMs);
    if (!p.started) {
        r.key = "UNEXPECTED";
        r.text = "the test program could not be started (error " + std::to_string(p.startError) + ")";
        return r;
    }
    r = ParseSelfTest(p.output, p.exitCode, p.timedOut);
    const size_t at = p.output.find("graphics card: ");   // the test says which card it ran on
    if (at != std::string::npos) {
        const size_t end = p.output.find_first_of("\r\n", at);
        r.gpu = p.output.substr(at + 15, end == std::string::npos ? std::string::npos : end - at - 15);
    }
    return r;
}

namespace {

std::wstring FullPathOf(const std::wstring& p) {
    wchar_t b[2 * MAX_PATH];
    const DWORD n = GetFullPathNameW(p.c_str(), 2 * MAX_PATH, b, nullptr);
    return (n > 0 && n < 2 * MAX_PATH) ? std::wstring(b) : p;
}

bool IsFolder(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EndsWithDll(const std::wstring& p) { return p.size() >= 4 && _wcsicmp(p.c_str() + p.size() - 4, L".dll") == 0; }

std::wstring TimeStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

PlaceResult Refuse(const std::string& why) {
    PlaceResult r;
    r.message = why;
    return r;
}

} // namespace

PlaceResult PlaceModel(const std::wstring& source, const std::wstring& lsDir, const std::wstring& backupDir) {
    const std::wstring dest = lsDir + L"\\nvngx_dlssnr.dll";

    const DWORD attr = GetFileAttributesW(source.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return Refuse("That file was not found: " + Utf8(source));
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return Refuse("That is a folder, not a file. Pick nvngx_dlssnr.dll itself.");
    if (!EndsWithDll(source)) return Refuse("That is not a .dll file. Pick your copy of nvngx_dlssnr.dll.");
    uint64_t size = 0;
    if (!FileSize(source, size)) return Refuse("That file could not be read: " + Utf8(source));
    if (size < kSmallestPlausibleModel) return Refuse("That file is only " + SizeText(size) + ", too small to be the model (it is about 150 MB). Pick the complete file.");
    if (!IsFolder(lsDir)) return Refuse("The Lossless Scaling folder was not found: " + Utf8(lsDir));

    PlaceResult r;
    r.placedPath = dest;
    if (_wcsicmp(FullPathOf(source).c_str(), FullPathOf(dest).c_str()) == 0) {
        r.ok = true;
        r.message = "That file is already in place in the Lossless Scaling folder. Nothing changed.";
        return r;
    }

    // Move a model that is already there aside (a rename works even while a program has it loaded, a delete or an overwrite would not)
    std::wstring backup;
    uint64_t existing = 0;
    if (FileSize(dest, existing)) {
        CreateDirectoryW(backupDir.c_str(), nullptr);
        const std::wstring stem = backupDir + L"\\nvngx_dlssnr-" + TimeStamp();
        backup = stem + L".dll";
        for (int n = 2; GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n) backup = stem + L"-" + std::to_wstring(n) + L".dll";
        if (!MoveFileExW(dest.c_str(), backup.c_str(), MOVEFILE_COPY_ALLOWED)) {
            return Refuse("The model file that is there could not be moved aside (error " + std::to_string(GetLastError()) + "). Close Lossless Scaling and try again.");
        }
    }

    // Write the new copy under a temporary name, then rename it into place; put the old one back if anything fails
    const std::wstring part = dest + L".part";
    auto restore = [&] { if (!backup.empty()) MoveFileExW(backup.c_str(), dest.c_str(), MOVEFILE_COPY_ALLOWED); };
    if (!CopyFileW(source.c_str(), part.c_str(), FALSE)) {
        const DWORD e = GetLastError();
        DeleteFileW(part.c_str());
        restore();
        return Refuse("The file could not be copied (error " + std::to_string(e) + "). Is there room in the Lossless Scaling folder's drive?");
    }
    if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        DeleteFileW(part.c_str());
        restore();
        return Refuse("The copy could not be put in place (error " + std::to_string(e) + ").");
    }

    r.ok = true;
    r.backupPath = backup;
    r.message = "Placed nvngx_dlssnr.dll (" + SizeText(size) + ") in the Lossless Scaling folder. Restart Lossless Scaling, or press Restart engine under Advanced, to use it.";
    if (!backup.empty()) r.message += " The file that was there is in the backups folder.";
    return r;
}

Inputs Gather(const std::wstring& modelPath, const std::wstring& addonDir) {
    Inputs in;

    // First NVIDIA hardware adapter
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d = {};
            adapter->GetDesc1(&d);
            adapter->Release();
            if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                in.nvidiaFound = true;
                in.gpuName = Utf8(d.Description);
                in.gpuMemoryBytes = d.DedicatedVideoMemory;
                break;
            }
        }
        factory->Release();
    }

    // The driver's NGX core: the registry names the driver folder, _nvngx.dll is in it
    const std::wstring ngxFolder = RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath");
    in.ngxRegistered = !ngxFolder.empty();
    if (in.ngxRegistered) {
        const std::wstring core = ngxFolder + L"\\_nvngx.dll";
        uint64_t size = 0;
        in.ngxCoreFound = FileSize(core, size);
        if (in.ngxCoreFound) in.ngxCoreVersion = FileVersion(core);
    }

    // The model and this addon's helper
    in.modelPath = Utf8(modelPath);
    in.modelFound = FileSize(modelPath, in.modelSize);
    if (in.modelFound) { in.modelVersion = FileVersion(modelPath); in.modelHash = FileHash16(modelPath); }
    uint64_t helperSize = 0;
    in.helperFound = FileSize(addonDir + L"\\" NR_FORWARDER_FILENAME, helperSize);
    return in;
}

} // namespace req
