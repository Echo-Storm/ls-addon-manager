// Offline test of the requirements checker: the decision logic with made-up findings, the small text helpers, and a real look at this machine
// (which only has to work and be well formed; what it finds depends on the machine).
//   nr_reqtest.exe [path to nvngx_dlssnr.dll]
#include "addon/requirements.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace req;

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}
static const Row* Find(const Report& r, const char* label) {
    for (const auto& row : r.rows) if (row.label == label) return &row;
    return nullptr;
}
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// files for the placement tests
static void RemoveAll(const std::wstring& p) { std::error_code ec; std::filesystem::remove_all(p, ec); }
static bool Exists(const std::wstring& p) { std::error_code ec; return std::filesystem::exists(p, ec); }
static void MakeFile(const std::wstring& p, int megabytes, char fill) {   // `megabytes` MB of the byte `fill`
    std::ofstream f(std::filesystem::path(p), std::ios::binary);
    const std::string mb(1 << 20, fill);
    for (int i = 0; i < megabytes; ++i) f.write(mb.data(), static_cast<std::streamsize>(mb.size()));
}
static char FileFill(const std::wstring& p) { std::ifstream f(std::filesystem::path(p), std::ios::binary); char c = 0; f.get(c); return c; }
static bool SameContent(const std::wstring& a, const std::wstring& b) {
    std::error_code ec;
    if (std::filesystem::file_size(a, ec) != std::filesystem::file_size(b, ec)) return false;
    std::ifstream fa(std::filesystem::path(a), std::ios::binary), fb(std::filesystem::path(b), std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
}

// Everything present and the tested model.
static Inputs Good() {
    Inputs in;
    in.nvidiaFound = true; in.gpuName = "NVIDIA GeForce RTX 4070 Ti SUPER"; in.gpuMemoryBytes = 12ull << 30;
    in.ngxRegistered = true; in.ngxCoreFound = true; in.ngxCoreVersion = "32.0.16.1692";
    in.modelPath = "D:\\LS\\nvngx_dlssnr.dll"; in.modelFound = true; in.modelSize = kTestedModelSize; in.modelVersion = "310,8,0,0";
    in.helperFound = true;
    return in;
}

int main(int argc, char** argv) {
    // ---- text helpers
    Check("driver number from the NGX core version", DriverFromNgxVersion("32.0.16.1692") == "616.92" && DriverFromNgxVersion("31.0.15.5222") == "552.22");
    Check("...and nothing when the version is not in that shape", DriverFromNgxVersion("").empty() && DriverFromNgxVersion("1.2").empty() && DriverFromNgxVersion("a.b.c.d").empty());
    Check("short version accepts dots and commas", VersionShort("310.8.0.0") == "310.8" && VersionShort("310,8,0,0") == "310.8" && VersionShort("").empty() && VersionShort("7") == "7");
    Check("size text", SizeText(165840496ull) == "158.2 MB" && SizeText(0) == "0 MB" && SizeText(12ull << 30) == "12288.0 MB");

    // ---- all in place
    Report r = Evaluate(Good());
    Check("all in place: overall Ok, six rows in order", r.overall == Level::Ok && r.rows.size() == 6 && r.rows[0].label == "Graphics card" && r.rows[5].label == "Engine");
    Check("all in place: the tested model is named as such", Has(Find(r, "Model file")->value, "310.8") && Has(Find(r, "Model file")->value, "tested") && Find(r, "Model file")->level == Level::Ok);
    Check("all in place: the driver is shown as a driver number", Has(Find(r, "NVIDIA driver")->value, "616.92"));
    Check("all in place: the card shows its memory", Has(Find(r, "Graphics card")->value, "12 GB") && Has(Find(r, "Graphics card")->value, "RTX 4070"));
    Check("all in place: no hints on rows that are fine", Find(r, "Model file")->hint.empty() && Find(r, "Graphics card")->hint.empty());
    Check("all in place: the engine not having started is not a problem", Find(r, "Engine")->level == Level::Ok && Has(Find(r, "Engine")->value, "not started"));
    Check("all in place: the headline says so", Has(r.headline, "in place"));

    // ---- each thing missing
    Inputs in = Good(); in.nvidiaFound = false; r = Evaluate(in);
    Check("no NVIDIA card: Missing, and it says what is needed", r.overall == Level::Missing && Find(r, "Graphics card")->level == Level::Missing && Has(Find(r, "Graphics card")->hint, "RTX"));
    Check("no NVIDIA card: the headline names the card", Has(r.headline, "Graphics card"));

    in = Good(); in.ngxRegistered = false; in.ngxCoreFound = false; r = Evaluate(in);
    Check("no NGX core registered: Missing, hint says to install or repair the driver", Find(r, "NVIDIA driver")->level == Level::Missing && Has(Find(r, "NVIDIA driver")->hint, "driver"));
    in = Good(); in.ngxCoreFound = false; r = Evaluate(in);
    Check("NGX core registered but the file is gone: Missing, and says the file", Find(r, "NVIDIA driver")->level == Level::Missing && Has(Find(r, "NVIDIA driver")->value, "_nvngx.dll"));

    in = Good(); in.modelFound = false; in.modelSize = 0; in.modelVersion.clear(); r = Evaluate(in);
    Check("no model file: Missing, with the path it looked at", Find(r, "Model file")->level == Level::Missing && Has(Find(r, "Model file")->value, "D:\\LS\\nvngx_dlssnr.dll"));
    Check("no model file: the hint says where to put it and that this project does not say where to get it",
          Has(Find(r, "Model file")->hint, "next to LosslessScaling.exe") && Has(Find(r, "Model file")->hint, "does not say where"));
    Check("no model file: the headline is about the model", Has(r.headline, "Model file"));

    in = Good(); in.modelSize = 4096; r = Evaluate(in);
    Check("a tiny model file is not accepted as the model", Find(r, "Model file")->level == Level::Missing && Has(Find(r, "Model file")->value, "too small"));

    in = Good(); in.modelVersion = "311,2,0,0"; r = Evaluate(in);
    Check("another model version: a note, not an error, naming the tested one", Find(r, "Model file")->level == Level::Note && Has(Find(r, "Model file")->value, "311.2") && Has(Find(r, "Model file")->value, "310.8"));
    Check("another model version: overall is Note and the headline says it", r.overall == Level::Note && Has(r.headline, "Model file"));
    in = Good(); in.modelSize = kTestedModelSize + 1; r = Evaluate(in);
    Check("the tested version but another size is also only a note", Find(r, "Model file")->level == Level::Note);
    in = Good(); in.modelVersion.clear(); r = Evaluate(in);
    Check("a model with no readable version is a note", Find(r, "Model file")->level == Level::Note && Has(Find(r, "Model file")->value, "unknown"));

    in = Good(); in.helperFound = false; r = Evaluate(in);
    Check("helper DLL missing: Missing, hint says the addon folder is incomplete", Find(r, "Helper DLL")->level == Level::Missing && Has(Find(r, "Helper DLL")->hint, "incomplete"));

    // ---- engine
    in = Good(); in.engine = EngineState::Running; r = Evaluate(in);
    Check("engine running: Ok", Find(r, "Engine")->level == Level::Ok && Has(Find(r, "Engine")->value, "running"));
    in = Good(); in.engine = EngineState::Ready; r = Evaluate(in);
    Check("engine ready: Ok", Find(r, "Engine")->level == Level::Ok && Has(Find(r, "Engine")->value, "ready"));
    in = Good(); in.engine = EngineState::Failed; in.engineError = "snippet probe 0x0 (D:\\LS\\nvngx_dlssnr.dll)"; r = Evaluate(in);
    Check("engine failed with everything present: Missing, in words, pointing at Restart engine and the log",
          r.overall == Level::Missing && Find(r, "Engine")->level == Level::Missing && Has(Find(r, "Engine")->value, "could not be loaded") &&
          Has(Find(r, "Engine")->hint, "Restart engine") && Has(Find(r, "Engine")->hint, "log"));
    in = Good(); in.modelFound = false; in.engine = EngineState::Failed; in.engineError = "snippet probe 0x0"; r = Evaluate(in);
    Check("engine failed because the model is missing: the first problem in the headline is the model, not the engine", Has(r.headline, "Model file"));
    Check("...and the engine row points back up instead of repeating", Has(Find(r, "Engine")->hint, "above"));

    // ---- plain-language engine messages
    Check("plain: model probe", Has(PlainEngineError("snippet probe 0x0 (x)"), "model file could not be loaded"));
    Check("plain: helper load", Has(PlainEngineError("forwarder LoadLibrary 126 (x)"), "helper DLL could not be loaded"));
    Check("plain: helper export", Has(PlainEngineError("forwarder export nrfwd_probe missing"), "different version"));
    Check("plain: NGX core", Has(PlainEngineError("NGX core Init: NVSDK_NGX_Result_FAIL_NotSupported"), "NGX core did not start") && Has(PlainEngineError("NGX core Init: NVSDK_NGX_Result_FAIL_NotSupported"), "NotSupported"));
    Check("plain: the model cannot run on this card", Has(PlainEngineError("CreateFeature(18): FeatureNotSupported"), "cannot run on your graphics card") && Has(PlainEngineError("CreateFeature(18): FeatureNotSupported"), "RTX 20, 30 and 40"));
    Check("plain: another CreateFeature failure keeps its detail", Has(PlainEngineError("CreateFeature(18): OutOfGPUMemory"), "OutOfGPUMemory"));
    Check("plain: model init",Has(PlainEngineError("snippet Init_Ext: X"), "refused to start"));
    Check("plain: device", Has(PlainEngineError("D3D12CreateDevice 0x887a0004"), "Direct3D 12"));
    Check("plain: unknown messages are shown as they are", PlainEngineError("something new") == "something new");

    // ---- the compatibility self-test: reading its verdict
    {
        SelfTestResult s = ParseSelfTest("graphics card: X\nmodel probe: 0xf\nSELFTEST 0 PASS the model works on X\n[ngx] a log line printed after the verdict\n", 0, false);
        Check("self-test: a pass is read from the last SELFTEST line even when the NGX core logs after it", s.passed && s.code == 0 && s.key == "PASS" && Has(s.text, "works on X"));
        s = ParseSelfTest("SELFTEST 17 NOT_SUPPORTED this model file cannot create its feature on your graphics card\nnoise\n", 17, false);
        Check("self-test: a failure keeps its code, key and words", !s.passed && s.code == 17 && s.key == "NOT_SUPPORTED" && Has(s.text, "cannot create its feature"));
        s = ParseSelfTest("SELFTEST 0 PASS fine\n", 5, false);
        Check("self-test: a verdict the exit code contradicts is not believed", !s.passed && s.key == "UNEXPECTED");
        s = ParseSelfTest("loading...\n", 0xC0000005ul, false);
        Check("self-test: no verdict and a crash code: reported as a crash, with the code", !s.passed && s.key == "CRASH" && Has(s.text, "C0000005"));
        s = ParseSelfTest("loading...\n", 0, false);
        Check("self-test: no verdict at all is not a pass", !s.passed && s.key == "UNEXPECTED");
        s = ParseSelfTest("loading...\n", 1, true);
        Check("self-test: ended for taking too long", !s.passed && s.key == "TIMEOUT" && Has(s.text, "too long"));
        s = ParseSelfTest("", 0, false);
        Check("self-test: no output at all is not a pass", !s.passed);
    }

    // ---- running a program (cmd.exe stands in for nr_selftest)
    {
        ProcessResult p = RunProcess(L"cmd.exe /c \"echo SELFTEST 0 PASS ok & exit 0\"", 15000);
        Check("process: a program that prints and exits 0", p.started && !p.timedOut && p.exitCode == 0 && Has(p.output, "SELFTEST 0 PASS ok"));
        p = RunProcess(L"cmd.exe /c \"echo hello & exit 3\"", 15000);
        Check("process: its exit code is returned", p.started && p.exitCode == 3 && Has(p.output, "hello"));
        const ULONGLONG t0 = GetTickCount64();
        p = RunProcess(L"cmd.exe /c \"ping -n 30 127.0.0.1 >nul\"", 800);
        Check("process: one still running after the time allowed is ended and reported", p.started && p.timedOut && GetTickCount64() - t0 < 8000, std::to_string(GetTickCount64() - t0) + " ms");
        p = RunProcess(L"\"C:\\definitely\\not\\here\\nothing.exe\" --x", 2000);
        Check("process: a program that does not exist is reported as not started", !p.started && p.startError != 0);
        SelfTestResult none = RunSelfTest(L"C:\\definitely\\not\\here", L"C:\\x\\nvngx_dlssnr.dll", L"C:\\x", 2000);
        Check("self-test: no nr_selftest.exe in the addon folder: NOT_FOUND, in words", !none.passed && none.key == "NOT_FOUND" && Has(none.text, "nr_selftest.exe"));
    }

    // ---- the self-test's row in the report
    {
        Inputs g = Good();
        Report rr = Evaluate(g);
        Check("self-test row: six rows now, the compatibility test before the engine", rr.rows.size() == 6 && rr.rows[4].label == "Compatibility test" && rr.rows[5].label == "Engine");
        Check("self-test row: not run yet is not a problem and says how to run it", Find(rr, "Compatibility test")->level == Level::Ok && Has(Find(rr, "Compatibility test")->value, "not run yet") && Has(Find(rr, "Compatibility test")->value, "Test compatibility"));
        g.selfTest = SelfTestState::Running; rr = Evaluate(g);
        Check("self-test row: while it runs it is a note, not an error", Find(rr, "Compatibility test")->level == Level::Note && rr.overall == Level::Note && !Find(rr, "Compatibility test")->hint.empty());
        g.selfTest = SelfTestState::Passed; g.selfTestKey = "PASS"; g.selfTestText = "the model works on X"; rr = Evaluate(g);
        Check("self-test row: passed is OK with its words", Find(rr, "Compatibility test")->level == Level::Ok && Has(Find(rr, "Compatibility test")->value, "works on X") && rr.overall == Level::Ok);
        g.selfTest = SelfTestState::Failed; g.selfTestKey = "NOT_SUPPORTED"; g.selfTestText = "this model file cannot create its feature on your graphics card"; rr = Evaluate(g);
        Check("self-test row: a failure is Missing, makes the whole thing Missing, and says what to try",
              Find(rr, "Compatibility test")->level == Level::Missing && rr.overall == Level::Missing && Has(Find(rr, "Compatibility test")->hint, "another build") && Has(rr.headline, "Compatibility test"));
        for (const char* key : { "MODEL_LOAD", "HELPER", "NGX_CORE", "D3D12", "NO_GPU", "FLOAT_SLOT", "MODEL_INIT", "FEATURE", "EVALUATE", "UNCHANGED", "CRASH", "TIMEOUT", "NOT_FOUND", "UNEXPECTED", "SOMETHING_NEW" }) {
            g.selfTestKey = key; rr = Evaluate(g);
            if (Find(rr, "Compatibility test")->hint.empty()) { Check("self-test row: every failure key has a hint", false, key); break; }
        }
        Check("self-test row: every failure key has a hint", true);
        Inputs noModel = Good(); noModel.modelFound = false; rr = Evaluate(noModel);
        Check("self-test row: with no model file it says it needs one first", Has(Find(rr, "Compatibility test")->value, "model file"));
    }

    // ---- placing a picked model file (throw-away folders in %TEMP%)
    {
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring root = std::wstring(tmp) + L"nr_reqtest_place";
        const std::wstring ls = root + L"\\ls", picked = root + L"\\picked", backups = ls + L"\\backups";
        RemoveAll(root);
        CreateDirectoryW(root.c_str(), nullptr); CreateDirectoryW(ls.c_str(), nullptr); CreateDirectoryW(picked.c_str(), nullptr);
        const std::wstring big = picked + L"\\some_copy.dll";                  // any name: it is placed as nvngx_dlssnr.dll
        MakeFile(big, 21, 'A');
        MakeFile(picked + L"\\small.dll", 1, 'B');
        MakeFile(picked + L"\\model.bin", 21, 'C');

        PlaceResult p = PlaceModel(picked + L"\\missing.dll", ls, backups);
        Check("place: a file that does not exist is refused", !p.ok && Has(p.message, "not found"));
        p = PlaceModel(picked + L"\\small.dll", ls, backups);
        Check("place: a file far too small to be the model is refused", !p.ok && Has(p.message, "too small") && !Exists(ls + L"\\nvngx_dlssnr.dll"));
        p = PlaceModel(picked + L"\\model.bin", ls, backups);
        Check("place: a file that is not a .dll is refused", !p.ok && Has(p.message, ".dll") && !Exists(ls + L"\\nvngx_dlssnr.dll"));
        p = PlaceModel(picked, ls, backups);
        Check("place: a folder is refused", !p.ok);
        p = PlaceModel(big, root + L"\\no_such_folder", backups);
        Check("place: a Lossless Scaling folder that does not exist is refused", !p.ok && Has(p.message, "folder"));

        p = PlaceModel(big, ls, backups);
        Check("place: into a folder with no model yet: placed as nvngx_dlssnr.dll", p.ok && Exists(ls + L"\\nvngx_dlssnr.dll") && SameContent(big, ls + L"\\nvngx_dlssnr.dll") && p.backupPath.empty(), p.message);
        Check("place: the picked file is left where it was", Exists(big) && FileFill(big) == 'A');
        Check("place: no temporary file is left behind", !Exists(ls + L"\\nvngx_dlssnr.dll.part"));

        MakeFile(picked + L"\\newer.dll", 22, 'D');
        p = PlaceModel(picked + L"\\newer.dll", ls, backups);
        Check("place: over an existing model: the new one is in place", p.ok && FileFill(ls + L"\\nvngx_dlssnr.dll") == 'D', p.message);
        Check("place: ...and the old one was moved to the backups folder, not deleted", !p.backupPath.empty() && Exists(p.backupPath) && FileFill(p.backupPath) == 'A' && p.backupPath.find(L"backups") != std::wstring::npos);
        Check("place: ...and the message says a restart is needed", Has(p.message, "restart") || Has(p.message, "Restart"));

        p = PlaceModel(ls + L"\\nvngx_dlssnr.dll", ls, backups);
        Check("place: picking the file that is already in place succeeds and changes nothing", p.ok && FileFill(ls + L"\\nvngx_dlssnr.dll") == 'D' && p.backupPath.empty() && Has(p.message, "already"), p.message);

        // a backup with the same second in its name must not overwrite the earlier one
        MakeFile(picked + L"\\third.dll", 23, 'E');
        PlaceResult a = PlaceModel(picked + L"\\third.dll", ls, backups);
        MakeFile(picked + L"\\fourth.dll", 24, 'F');
        PlaceResult b = PlaceModel(picked + L"\\fourth.dll", ls, backups);
        Check("place: two placements in the same second keep both backups", a.ok && b.ok && a.backupPath != b.backupPath && FileFill(a.backupPath) == 'D' && FileFill(b.backupPath) == 'E');

        // a model that is loaded by a running program cannot be replaced but can be renamed: simulate the lock the copy would meet
        HANDLE lock = CreateFileW((ls + L"\\nvngx_dlssnr.dll").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        MakeFile(picked + L"\\fifth.dll", 25, 'G');
        p = PlaceModel(picked + L"\\fifth.dll", ls, backups);
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check("place: over a model another program has open for reading (renamed aside, new one placed)", p.ok && FileFill(ls + L"\\nvngx_dlssnr.dll") == 'G', p.message);
        RemoveAll(root);
    }

    // ---- this machine
    std::wstring model = L"nvngx_dlssnr.dll";
    if (argc > 1) { int n = MultiByteToWideChar(CP_ACP, 0, argv[1], -1, nullptr, 0); model.assign(n, L'\0'); MultiByteToWideChar(CP_ACP, 0, argv[1], -1, model.data(), n); model.resize(n - 1); }
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe; dir.resize(dir.find_last_of(L'\\'));
    const Inputs real = Gather(model, dir);
    const Report rr = Evaluate(real);
    printf("---- this machine\n");
    for (const auto& row : rr.rows) printf("  [%s] %s: %s%s%s\n", row.level == Level::Ok ? "ok" : (row.level == Level::Note ? "note" : "MISSING"), row.label.c_str(), row.value.c_str(), row.hint.empty() ? "" : "\n        -> ", row.hint.c_str());
    printf("  headline: %s\n", rr.headline.c_str());
    Check("this machine: six rows, each with a label and a value", rr.rows.size() == 6 && !rr.rows[0].value.empty() && !rr.rows[5].value.empty());
    Check("this machine: a row that is not Ok always has a hint (the engine row may just say it has not started)", [&] { for (const auto& row : rr.rows) if (row.level != Level::Ok && row.hint.empty()) return false; return true; }());
    {   // the helper is looked for by name in the addon folder: found where it is, not found in an empty folder
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring empty = std::wstring(tmp) + L"nr_reqtest_empty";
        CreateDirectoryW(empty.c_str(), nullptr);
        const bool inEmpty = Gather(model, empty).helperFound;
        RemoveDirectoryW(empty.c_str());
        const bool onDisk = GetFileAttributesW((dir + L"\\nvngx.dll_dlss5nr01.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
        Check("this machine: the helper DLL is found where it is and not in an empty folder", !inEmpty && real.helperFound == onDisk);
    }

    // the real thing, when this machine has a model file and the test program is beside this one
    if (real.modelFound && Exists(dir + L"\\nr_selftest.exe")) {
        std::wstring lsFolder = model;
        lsFolder.resize(lsFolder.find_last_of(L"\\/"));
        const SelfTestResult st = RunSelfTest(dir, model, lsFolder, 120000);
        printf("  the real self-test: code %d %s: %s\n", st.code, st.key.c_str(), st.text.c_str());
        Check("this machine: the real self-test returns a verdict", !st.key.empty() && !st.text.empty());
        const bool testedBuild = VersionShort(real.modelVersion) == kTestedModelVersion && real.modelSize == kTestedModelSize;
        Check("this machine: ...and passes with the build this addon was tested with", !testedBuild || st.passed);
    } else {
        printf("  (the real self-test is skipped: no model file here, or no nr_selftest.exe beside this program)\n");
    }

    printf("\n%s (%d failed)\n", g_failed ? "REQUIREMENTS TEST FAILED" : "REQUIREMENTS TEST PASSED", g_failed);
    return g_failed ? 1 : 0;
}
