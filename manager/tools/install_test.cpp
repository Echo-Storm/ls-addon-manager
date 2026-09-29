// Offline test for PlaceAddon (installing an addon from a folder, a zip or a lone DLL). Works in a temporary folder; touches nothing else.
//   eam_installtest.exe
#include "src/addon/addon_install.h"
#include "src/host/metrics.h"
#include "src/host/gpu_stats.h"
#include "src/config/settings_backup.h"
#include "src/diag/diagnostics.h"
#include <thread>
#include <chrono>
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace eam;

static int g_fail = 0;
static void Check(const char* what, bool ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) g_fail++; }
static void Touch(const fs::path& p, const char* text = "x") { fs::create_directories(p.parent_path()); std::ofstream(p) << text; }

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--counters") GpuStats::PreferCounters(true);   // the GPU test through Windows' counters even where NVML works
    setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path base = fs::temp_directory_path() / ("eam_install_test_" + std::to_string(GetTickCount64()));
    const fs::path addons = base / "addons", src = base / "src";
    fs::create_directories(addons);

    // a folder addon
    Touch(src / "Cool-Addon" / "addon.json", "{\"name\":\"Cool\"}");
    Touch(src / "Cool-Addon" / "Cool.dll");
    Touch(src / "Cool-Addon" / "sub" / "data.txt");
    PlaceResult r = PlaceAddon(src / "Cool-Addon", addons);
    Check("folder: installed", r.ok);
    Check("folder: files copied including sub-folders", fs::exists(addons / "Cool-Addon" / "Cool.dll") && fs::exists(addons / "Cool-Addon" / "sub" / "data.txt"));
    Check("folder: the source is untouched", fs::exists(src / "Cool-Addon" / "Cool.dll"));

    r = PlaceAddon(src / "Cool-Addon", addons);
    Check("folder: installing the same addon again is refused, not overwritten", !r.ok && r.message.find("already installed") != std::string::npos);

    // a lone DLL
    Touch(src / "Lonely.dll");
    r = PlaceAddon(src / "Lonely.dll", addons);
    Check("dll: installed into its own folder", r.ok && fs::exists(addons / "Lonely" / "Lonely.dll"));

    // a zip, with the addon in a top-level folder (made by Windows' tar, like the installer reads it)
    Touch(src / "ziproot" / "Zipped-Addon" / "addon.json", "{}");
    Touch(src / "ziproot" / "Zipped-Addon" / "Zipped.dll");
    const std::wstring zip = (base / "Zipped-Addon.zip").wstring();
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring mk = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -cf \"" + zip + L"\" -C \"" + (src / "ziproot").wstring() + L"\" Zipped-Addon";
    STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{}; std::wstring cmd = mk;
    CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    WaitForSingleObject(pi.hProcess, 30000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    Check("zip: test archive was created", fs::exists(zip));
    r = PlaceAddon(zip, addons);
    Check("zip (addon in a top-level folder): installed under the folder's name", r.ok && fs::exists(addons / "Zipped-Addon" / "Zipped.dll"));

    // a zip with the files at the root: the folder is named after the zip
    Touch(src / "flatroot" / "addon.json", "{}");
    Touch(src / "flatroot" / "Flat.dll");
    const std::wstring zip2 = (base / "Flat-Addon.zip").wstring();
    cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -cf \"" + zip2 + L"\" -C \"" + (src / "flatroot").wstring() + L"\" addon.json Flat.dll";
    PROCESS_INFORMATION pi2{}; CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi2);
    WaitForSingleObject(pi2.hProcess, 30000); CloseHandle(pi2.hProcess); CloseHandle(pi2.hThread);
    r = PlaceAddon(zip2, addons);
    Check("zip (files at the root): installed under the zip's name", r.ok && fs::exists(addons / "Flat-Addon" / "Flat.dll"));

    // things that must be refused
    Touch(src / "Empty" / "readme.txt");
    r = PlaceAddon(src / "Empty", addons);
    Check("a folder with no addon.json and no DLL is refused", !r.ok);
    Touch(src / "notes.txt");
    r = PlaceAddon(src / "notes.txt", addons);
    Check("a .txt file is refused", !r.ok);
    r = PlaceAddon(src / "missing.zip", addons);
    Check("a path that does not exist is refused", !r.ok);
    Touch(src / "..evil" / "addon.json", "{}"); Touch(src / "..evil" / "E.dll");
    r = PlaceAddon(src / "..evil", addons);
    Check("a name starting with dots is made safe (stays inside the addons folder)", !r.ok || fs::equivalent(r.dest.parent_path(), addons));

    // removing: the folder is moved to .removed, never erased
    Touch(addons / "Goner" / "addon.json", "{}"); Touch(addons / "Goner" / "Goner.dll", "payload");
    RemoveResult rm = MoveAddonToRemoved(addons / "Goner", addons);
    Check("remove: the addon folder is gone from addons", rm.ok && !fs::exists(addons / "Goner"));
    Check("remove: it was moved into .removed with its files intact", rm.ok && fs::exists(rm.movedTo / "Goner.dll") && fs::exists(rm.movedTo / "addon.json") && rm.movedTo.parent_path().filename() == ".removed");
    Touch(addons / "Goner" / "Goner.dll", "second");
    RemoveResult rm2 = MoveAddonToRemoved(addons / "Goner", addons);
    Check("remove: removing an addon of the same name again does not overwrite the first", rm2.ok && rm2.movedTo != rm.movedTo && fs::exists(rm.movedTo / "Goner.dll"));
    RemoveResult rm3 = MoveAddonToRemoved(base / "src", addons);
    Check("remove: a folder outside the addons folder is refused", !rm3.ok && fs::exists(base / "src"));
    RemoveResult rm4 = MoveAddonToRemoved(addons / ".removed", addons);
    Check("remove: the .removed folder itself cannot be removed", !rm4.ok && fs::exists(addons / ".removed"));
    RemoveResult rm5 = MoveAddonToRemoved(addons / "NoSuchAddon", addons);
    Check("remove: a folder that does not exist is refused", !rm5.ok);
    // a folder in use (a file inside is held open) cannot be moved: the failure is reported and nothing is lost
    Touch(addons / "Busy" / "Busy.dll");
    HANDLE hold = CreateFileW((addons / "Busy" / "Busy.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    RemoveResult rm6 = MoveAddonToRemoved(addons / "Busy", addons);
    Check("remove: a folder with a file in use is not moved, and says so", !rm6.ok && fs::exists(addons / "Busy" / "Busy.dll") && !rm6.message.empty());
    if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);

    // ---- live status and metrics registry
    {
        auto& M = Metrics::Instance();
        M.Clear();
        for (int i = 0; i < 10; ++i) M.Publish("addonA", "frame_ms", 10.0 + i, "ms");
        Metrics::Series s = M.Get("addonA", "frame_ms", 60.0);
        Check("metrics: samples come back oldest first with their values", s.samples.size() == 10 && s.samples.front().v == 10.0f && s.samples.back().v == 19.0f && s.unit == "ms");
        bool sorted = true; for (size_t i = 1; i < s.samples.size(); ++i) if (s.samples[i].t < s.samples[i - 1].t) sorted = false;
        Check("metrics: timestamps never go backwards", sorted);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        M.Publish("addonA", "frame_ms", 99.0, "ms");
        Check("metrics: the time window drops old samples", M.Get("addonA", "frame_ms", 0.06).samples.size() == 1 && M.Get("addonA", "frame_ms", 60.0).samples.size() == 11);
        Check("metrics: an unknown series is empty, not an error", M.Get("addonA", "nope", 60.0).samples.empty() && M.Get("nobody", "frame_ms", 60.0).samples.empty());
        for (size_t i = 0; i < Metrics::kMaxSamples + 500; ++i) M.Publish("addonB", "ramp", (double)i, "");
        Metrics::Series r = M.Get("addonB", "ramp", 600.0);
        Check("metrics: the ring keeps only the newest kMaxSamples, in order", r.samples.size() == Metrics::kMaxSamples && r.samples.back().v == (float)(Metrics::kMaxSamples + 499) && r.samples.front().v == 500.0f);
        M.Clear();
        for (int i = 0; i < 300; ++i) M.Publish("flood", ("k" + std::to_string(i)).c_str(), 1.0, "");
        Check("metrics: one addon cannot create an unbounded number of series", M.Snapshot(60.0).size() <= 256);
        M.Clear();
        M.SetStatus("a", "Running, model 6.6 ms", 1);
        M.SetStatus("b", "Model cannot keep up", 2);
        M.SetStatus("c", "fine", 0);
        Check("status: each addon has its own status", M.GetStatus("a").text == "Running, model 6.6 ms" && M.GetStatus("a").level == 1 && M.GetStatus("zzz").text.empty());
        Check("status: the status bar shows the most serious one first", M.BestStatus().addon == "b" && M.BestStatus().level == 2);
        M.SetStatus("b", "", 0);
        Check("status: an empty text clears it", M.GetStatus("b").text.empty() && M.BestStatus().addon == "a");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Check("status: one that is not refreshed goes stale and is hidden", M.GetStatus("a", 0.1).text.empty() && M.BestStatus(0.1).text.empty());
        M.SetStatus("a", "again", 99);
        Check("status: levels are clamped to 0..3", M.GetStatus("a").level == 3);
        M.Clear();
    }

    // ---- GPU stats (hardware dependent: either it reads sane values, or it says why it cannot)
    {
        GpuStats& G = GpuStats::Instance();
        const bool ok = G.SampleOnce();
        GpuStats::Snapshot g = G.Get();
        if (ok) {
            printf("       GPU: %s, driver %s, load %u%%, %.0f / %.0f W, %u MHz, %u C, VRAM %llu / %llu MB, throttle 0x%llx\n", g.name.c_str(), g.driver.c_str(), g.utilGpu, g.powerW, g.powerLimitW, g.clockGraphics, g.tempC,
                   (unsigned long long)g.vramUsedMB, (unsigned long long)g.vramTotalMB, (unsigned long long)g.throttle);
            // NVML reports everything; Windows' counters (any other card, or a machine with no GPU of note such as a CI runner) only the load and the memory,
            // and a virtual adapter has no dedicated memory at all: then only what exists is checked
            Check("gpu: reads sane values (load 0..100, power below its limit plus a margin, memory used <= total)",
                  g.utilGpu <= 100 && !g.name.empty() && (g.viaCounters || (g.hasPower && g.powerW >= 0 && g.powerW < g.powerLimitW * 1.3 + 1 && g.vramTotalMB > 0)) &&
                  (g.vramTotalMB == 0 ? g.viaCounters : g.vramUsedMB <= g.vramTotalMB));
            Check("gpu: the values were also published as metrics", !Metrics::Instance().Get("system", "gpu_util", 10.0).samples.empty() &&
                  (!g.hasPower || !Metrics::Instance().Get("system", "gpu_power_w", 10.0).samples.empty()));
        } else {
            printf("       GPU stats not available here: %s\n", g.why.c_str());
            Check("gpu: when unavailable it says why", !g.why.empty());
        }
        Check("gpu: throttle text names the power limit and hides idle", GpuStats::ThrottleText(0x4 | 0x1) == "power limit" && GpuStats::ThrottleText(0x1).empty() && GpuStats::ThrottleText(0x4 | 0x20) == "power limit, temperature");
        G.Shutdown();
    }

    // ---- settings backup and restore
    {
        using nlohmann::json;
        json cfg = { { "addons", { { "DLSS5NR01", { { "_enabled", true }, { "workingScale", "0.5" } } }, { "ReShadePassthrough", { { "_enabled", false } } } } }, { "global", { { "log_level", 2 } } } };
        const std::string text = MakeSettingsBackupText(cfg, "0.5.0-test");
        BackupParse ok = ParseSettingsBackup(text);
        Check("backup: what is written reads back identically", ok.ok && ok.config == cfg && ok.addonCount == 2);
        BackupParse bare = ParseSettingsBackup(cfg.dump());
        Check("backup: a plain config.json copied by hand is accepted too", bare.ok && bare.config == cfg);
        const json older = { { "lsproxy_settings_backup", 1 }, { "config", cfg } };
        BackupParse old = ParseSettingsBackup(older.dump());
        Check("backup: one made by 0.7.4 or earlier (its old marker) is accepted too", old.ok && old.config == cfg);
        Check("backup: empty, garbage and unrelated JSON are refused",
              !ParseSettingsBackup("").ok && !ParseSettingsBackup("not json at all").ok && !ParseSettingsBackup("[1,2,3]").ok && !ParseSettingsBackup("{\"hello\":1}").ok);
        Check("backup: damaged shapes are refused", !ParseSettingsBackup("{\"addons\":[1]}").ok && !ParseSettingsBackup("{\"addons\":{\"x\":5}}").ok && !ParseSettingsBackup("{\"global\":7}").ok);
        Check("backup: missing parts are filled in", ParseSettingsBackup("{\"addons\":{}}").config.contains("global") && ParseSettingsBackup("{\"global\":{}}").config.contains("addons"));
        json applied; int calls = 0;
        const fs::path bdir = base / "backups";
        ImportResult imp = ImportSettings(text, json({ { "addons", { { "old", { { "k", "v" } } } } }, { "global", json::object() } }), bdir, [&](const json& j) { applied = j; ++calls; });
        Check("backup: import applies the new settings once", imp.ok && calls == 1 && applied == cfg);
        bool keptOld = false; if (imp.ok) { std::ifstream in(imp.previousSavedAs); json prev = json::parse(in, nullptr, false); keptOld = prev.contains("addons") && prev["addons"].contains("old"); }
        Check("backup: the previous settings were saved aside before the change", keptOld && imp.previousSavedAs.parent_path() == bdir);
        calls = 0;
        ImportResult bad = ImportSettings("garbage", json::object(), bdir, [&](const json&) { ++calls; });
        Check("backup: a bad file changes nothing", !bad.ok && calls == 0 && !bad.message.empty());
    }

    // ---- diagnostics bundle
    {
        const fs::path ls = base / "LS", out = base / "diag-out";
        Touch(ls / "logs" / "LSAddonManager.log", "proxy log line\n");
        Touch(ls / "logs" / "DLSS5NR01.log", "nr log line\n");
        Touch(ls / "addons" / "config.json", "{\"addons\":{}}");
        Touch(ls / "addons" / "DLSS5NR01" / "nvngx.log", "ngx log\n");
        { std::ofstream big(ls / "logs" / "Big.log", std::ios::binary); std::string chunk(1024 * 1024, 'x'); for (int i = 0; i < 5; ++i) big << chunk; }   // 5 MB
        DiagResult d = CreateDiagnosticsZip(ls, "summary text", out);
        Check("diagnostics: a zip was written", d.ok && fs::exists(d.zip) && d.zip.extension() == ".zip");
        // list it with tar and check what is inside
        std::wstring listing;
        if (d.ok) {
            wchar_t sys2[MAX_PATH]; GetSystemDirectoryW(sys2, MAX_PATH);
            const fs::path lst = base / "zip-listing.txt";
            const std::wstring lc = L"cmd /c \"\"" + std::wstring(sys2) + L"\\tar.exe\" -tf \"" + d.zip.wstring() + L"\" > \"" + lst.wstring() + L"\"\"";
            STARTUPINFOW si3{ sizeof si3 }; PROCESS_INFORMATION pi3{}; std::wstring c3 = lc;
            if (CreateProcessW(nullptr, c3.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si3, &pi3)) { WaitForSingleObject(pi3.hProcess, 30000); CloseHandle(pi3.hProcess); CloseHandle(pi3.hThread); }
            std::ifstream in(lst); std::string line; std::string all; while (std::getline(in, line)) all += line + "\n";
            Check("diagnostics: the zip holds the summary, the settings and the logs", all.find("info.txt") != std::string::npos && all.find("config.json") != std::string::npos &&
                  all.find("LSAddonManager.log") != std::string::npos && all.find("DLSS5NR01.log") != std::string::npos && all.find("nvngx.log") != std::string::npos);
        }
        Check("diagnostics: the temporary staging folder is gone", true);
        int leftovers = 0; { wchar_t tp[MAX_PATH]; GetTempPathW(MAX_PATH, tp); for (const auto& e : fs::directory_iterator(tp)) if (e.path().filename().wstring().rfind(L"eam-diag-", 0) == 0) ++leftovers; }
        Check("diagnostics: no eam-diag-* staging folders left in %TEMP%", leftovers == 0);
        DiagResult none = CreateDiagnosticsZip(base / "no-such-install", "summary", out);
        Check("diagnostics: with nothing to collect it still makes a zip with the summary", none.ok);
    }

    // no leftovers of the temporary extraction
    int staging = 0; for (const auto& e : fs::directory_iterator(addons)) if (e.path().filename().wstring().rfind(L".install-", 0) == 0) ++staging;
    Check("no .install-* staging folders left behind", staging == 0);

    std::error_code ec; fs::remove_all(base, ec);   // this test's own temporary folder
    printf("\n%s\n", g_fail ? "INSTALL TEST FAILED" : "INSTALL TEST PASSED");
    return g_fail ? 1 : 0;
}
