// The jobs that run beside the manager's window: the requirements scan (it reads file headers, the model's among them, which is large), the
// compatibility test (nr_selftest.exe tries the model once in a process of its own), and "Browse for the model file" (a modal file dialog,
// then a copy into the Lossless Scaling folder).
//
// Each runs on a detached thread tracked by a flag, never kept as a std::thread: Lossless Scaling ends the process without calling
// AddonShutdown, and a std::thread still joinable when this DLL's statics are destroyed ends the process with std::terminate.
#include "addon/state.h"
#include "addon/log.h"
#include <windows.h>
#include <commdlg.h>
#include <thread>

namespace nr {

namespace {
std::atomic<bool> g_scanning{ false }, g_selfTesting{ false }, g_browsing{ false };
std::mutex g_mutex;   // guards everything below
req::Inputs g_found; bool g_scanned = false;
req::SelfTestState g_selfTest = req::SelfTestState::NotRun; std::string g_selfTestKey, g_selfTestText;
std::string g_browseText; bool g_browseOk = false;

std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? n : 0);
    return w;
}

void Scan(const std::wstring& model, const std::wstring& addonDir) {
    const req::Inputs found = req::Gather(model, addonDir);
    for (const req::Row& r : req::Evaluate(found).rows)
        Log("requirements: %s: %s%s", r.label.c_str(), r.value.c_str(), r.level == req::Level::Ok ? "" : r.level == req::Level::Note ? "  [note]" : "  [MISSING]");
    std::lock_guard<std::mutex> lock(g_mutex);
    g_found = found; g_scanned = true;
}
void ScanGuarded(const std::wstring& model, const std::wstring& addonDir) {   // no objects here: __try cannot unwind them
    __try { Scan(model, addonDir); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("requirements: the check crashed (0x%08lx)", GetExceptionCode()); }
}

void SelfTest() {
    { std::lock_guard<std::mutex> lock(g_mutex); g_selfTest = req::SelfTestState::Running; g_selfTestKey.clear(); g_selfTestText.clear(); }
    std::wstring luid;   // the card Lossless Scaling runs on, once it has been seen: the model runs there, and with two NVIDIA cards that is not always DXGI's first
    if (g_frameCardKnown) { wchar_t b[40]; swprintf(b, 40, L"%x:%x", static_cast<unsigned>(g_frameCard.HighPart), static_cast<unsigned>(g_frameCard.LowPart)); luid = b; }
    const req::SelfTestResult r = req::RunSelfTest(g_addonDir, ModelPath(), g_lsDir, 90000, luid);
    Log("compatibility test on %s%s: %s (%s): %s", r.gpu.empty() ? "?" : r.gpu.c_str(), luid.empty() ? " (the first NVIDIA card: Lossless Scaling has not run yet)" : " (the card Lossless Scaling runs on)",
        r.passed ? "passed" : "FAILED", r.key.c_str(), r.text.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    g_selfTest = r.passed ? req::SelfTestState::Passed : req::SelfTestState::Failed;
    g_selfTestKey = r.key; g_selfTestText = r.text;
}
void SelfTestCrashed(unsigned long code) {
    Log("compatibility test crashed inside the addon (0x%08lx)", code);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_selfTest = req::SelfTestState::Failed;
}
void SelfTestGuarded() {   // no objects here: __try cannot unwind them
    __try { SelfTest(); } __except (EXCEPTION_EXECUTE_HANDLER) { SelfTestCrashed(GetExceptionCode()); }
}

std::wstring AskForModelFile() {
    wchar_t file[MAX_PATH * 2] = {};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = FindWindowW(L"LSAddonManagerClass", nullptr);
    dialog.lpstrFilter = L"DLL files (*.dll)\0*.dll\0All files\0*.*\0";
    dialog.lpstrFile = file; dialog.nMaxFile = MAX_PATH * 2;
    dialog.lpstrTitle = L"Pick your copy of nvngx_dlssnr.dll";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::wstring(file) : std::wstring();
}
void Browse() {
    const std::wstring picked = AskForModelFile();
    if (picked.empty()) { std::lock_guard<std::mutex> lock(g_mutex); g_browseText = "No file picked."; g_browseOk = false; return; }
    const req::PlaceResult r = req::PlaceModel(picked, g_lsDir, g_lsDir + L"\\backups");
    Log("place model: %s (%s)", r.message.c_str(), r.ok ? "ok" : "refused");
    { std::lock_guard<std::mutex> lock(g_mutex); g_browseText = r.message; g_browseOk = r.ok; }
    if (r.ok) { ScanRequirements(); RunSelfTest(); }   // a new file: see at once whether it works on this card
}
void BrowseGuarded() {
    __try { Browse(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("place model crashed (0x%08lx)", GetExceptionCode()); }
}
} // namespace

std::wstring ModelPath() {
    std::string chosen;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); chosen = g_config.snippetPath; }
    return chosen.empty() ? g_lsDir + L"\\nvngx_dlssnr.dll" : Wide(chosen);
}

void ScanRequirements() {
    if (g_scanning.exchange(true)) return;
    const std::wstring model = ModelPath(), addonDir = g_addonDir;
    std::thread([model, addonDir] { ScanGuarded(model, addonDir); g_scanning = false; }).detach();
}
void RunSelfTest() {
    if (g_selfTesting.exchange(true)) return;
    std::thread([] { SelfTestGuarded(); g_selfTesting = false; }).detach();
}
void BrowseForModel() {
    if (g_browsing.exchange(true)) return;
    std::thread([] { BrowseGuarded(); g_browsing = false; }).detach();
}
bool Scanning() { return g_scanning; }
bool SelfTesting() { return g_selfTesting; }
bool Browsing() { return g_browsing; }

bool RequirementsScanned() { std::lock_guard<std::mutex> lock(g_mutex); return g_scanned; }

req::Report Requirements(const EngineView& engine) {
    req::Inputs in;
    { std::lock_guard<std::mutex> lock(g_mutex); in = g_found; in.selfTest = g_selfTest; in.selfTestKey = g_selfTestKey; in.selfTestText = g_selfTestText; }
    in.engine = engine.failed ? req::EngineState::Failed : engine.running ? req::EngineState::Running : engine.ready ? req::EngineState::Ready : req::EngineState::NotStarted;
    if (engine.failed && engine.error) in.engineError = engine.error;
    return req::Evaluate(in);
}

std::string BrowseResult(bool& ok) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ok = g_browseOk;
    return g_browseText;
}

} // namespace nr
