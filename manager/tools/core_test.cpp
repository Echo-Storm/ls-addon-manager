// Offline test of the manager's addon handling: finding addons in a folder, reading their manifests, loading, initialising, switching on and
// off, removing and installing them, the security levels, and a faulting addon. It runs against a throw-away folder in %TEMP% with copies of
// a small test addon (test_addon.cpp), and touches no game and no Lossless Scaling install.
//   eam_coretest.exe        (eam_testaddon.dll must sit beside it; the build puts it there)
#include "src/addon/addon_dependency.h"
#include "src/addon/addon_manager.h"
#include "src/addon/addon_manifest.h"
#include "src/addon/addon_security.h"
#include "src/config/config_manager.h"
#include "src/config/settings_backup.h"
#include "src/core/d3d11_hook.h"
#include "src/core/instance_guard.h"
#include "src/core/shader_hook.h"
#include "src/gui/icon_loader.h"
#include "abi_v1_0.h"
#include "src/event/event_system.h"
#include "src/host/gpu_stats.h"
#include "src/host/host_impl.h"
#include "src/host/metrics.h"
#include "src/log/logger.h"
#include "eam/version.h"
#include "imgui.h"
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace eam;

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

static void WriteFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static fs::path ExeDir() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    return fs::path(b).parent_path();
}

// An addon folder holding a copy of the test addon under `dll`, an optional manifest and an optional mode.
static fs::path MakeAddon(const fs::path& root, const std::string& folder, const std::string& dll, const char* manifest = nullptr, const char* mode = nullptr) {
    const fs::path dir = root / folder;
    fs::create_directories(dir);
    fs::copy_file(ExeDir() / "eam_testaddon.dll", dir / dll, fs::copy_options::overwrite_existing);
    if (manifest) WriteFile(dir / "addon.json", manifest);
    if (mode) WriteFile(dir / "mode.txt", mode);
    return dir;
}

static int Calls(const fs::path& dir, const std::string& what) {
    std::ifstream f(dir / "calls.txt");
    std::string ln;
    int n = 0;
    while (std::getline(f, ln)) if (ln == what) ++n;
    return n;
}

static int IndexOf(AddonManager& m, const std::string& id) {
    auto& a = m.GetAddons();
    for (size_t i = 0; i < a.size(); ++i) if (a[i].id == id) return (int)i;
    return -1;
}

static AddonInfo* Find(AddonManager& m, const std::string& id) {
    const int i = IndexOf(m, id);
    return i < 0 ? nullptr : &m.GetAddons()[i];
}

static bool EnabledInFile(const fs::path& cfg, const std::string& id, bool* present = nullptr) {
    std::ifstream f(cfg);
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    const bool has = !j.is_discarded() && j.contains("addons") && j["addons"].contains(id) && j["addons"][id].contains("_enabled");
    if (present) *present = has;
    return has && j["addons"][id]["_enabled"].get<bool>();
}

static nlohmann::json ReadJson(const fs::path& p) {
    std::ifstream f(p);
    return nlohmann::json::parse(f, nullptr, false);
}

// The settings file: reading, writing, the two kinds of value, the enabled flag, and what happens to a damaged file.
static void TestConfig(const fs::path& T) {
    using nlohmann::json;
    printf("== settings file\n");
    ConfigManager& C = ConfigManager::Instance();
    const fs::path base = T / "cfg";

    {   // a new file, written and read back
        const fs::path dir = base / "new";
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        C.Load(p.wstring());
        Check("with no file, everything reads as its default", C.Get("x", "k", "dflt") == "dflt" && C.GlobalGet(nullptr, "k").is_null() && C.IsAddonEnabled("x", true) && !C.IsAddonEnabled("x", false));
        C.Set("x", "k", "v");
        C.SetAddonEnabled("x", false);
        C.GlobalSet("ui", "size", 125);
        C.GlobalSet(nullptr, "level", 2);
        C.Save();
        Check("Save writes the file", fs::exists(p));
        const json j = ReadJson(p);
        Check("an addon's values, its enabled flag and the host's settings land where documented",
              j["addons"]["x"]["k"] == "v" && j["addons"]["x"]["_enabled"] == false && j["global"]["ui"]["size"] == 125 && j["global"]["level"] == 2);
        Check("no temporary file is left beside it", !fs::exists(fs::path(p) += ".tmp"));
        fs::remove(p);
        C.Save();
        Check("saving settings that have not changed does not rewrite the file", !fs::exists(p));
        C.Set("x", "k", "w");
        C.Save();
        Check("a change is written", fs::exists(p) && ReadJson(p)["addons"]["x"]["k"] == "w");

        C.Load(p.wstring());
        Check("loading the file again gives the same values", C.Get("x", "k") == "w" && !C.IsAddonEnabled("x", true) && C.GlobalGetOr<int>("ui", "size", 0) == 125 && C.GlobalGetOr<int>(nullptr, "level", 0) == 2);
        Check("a host setting of the wrong type reads as the default", C.GlobalGetOr<std::string>("ui", "size", "none") == "none" && C.GlobalGetOr<int>("ui", "absent", 9) == 9);
    }

    {   // the kinds of value, and the enabled flag in its old and new places
        const fs::path dir = base / "types";
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        WriteFile(p, R"({"addons":{"t":{"s":"text","b":true,"n":3.5,"i":7,"o":{"a":1},"_enabled":true,"enabled":"yes"},"l":{"enabled":false},"m":{"enabled":"yes"}}})");
        C.Load(p.wstring());
        Check("text, booleans and numbers all read back as text", C.Get("t", "s") == "text" && C.Get("t", "b") == "1" && C.Get("t", "n") == "3.5" && C.Get("t", "i") == "7");
        Check("an object reads as the default", C.Get("t", "o", "dflt") == "dflt");
        Check("an addon's own 'enabled' text is not the host's flag", C.Get("t", "enabled") == "yes" && C.IsAddonEnabled("t", false));
        Check("the old boolean 'enabled' is still honoured", !C.IsAddonEnabled("l", true));
        Check("a text 'enabled' is not taken as the flag", C.IsAddonEnabled("m", true) && !C.IsAddonEnabled("m", false));
        C.SetAddonEnabled("l", true);
        const json snap = C.Snapshot();
        Check("setting the flag moves it to _enabled and drops the old boolean", snap["addons"]["l"]["_enabled"] == true && !snap["addons"]["l"].contains("enabled"));
        C.SetAddonEnabled("m", false);
        Check("...but leaves an addon's own 'enabled' text alone", C.Snapshot()["addons"]["m"]["enabled"] == "yes");
    }

    for (const char* bad : { "{ this is not json", "[1, 2, 3]" }) {   // a damaged file is kept, not overwritten
        const fs::path dir = base / (std::string("bad") + (bad[0] == '[' ? "_array" : "_text"));
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        WriteFile(p, bad);
        C.Load(p.wstring());
        std::ifstream kept(fs::path(p) += ".corrupt");
        const std::string keptText((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
        Check("a damaged settings file is kept as config.json.corrupt", keptText == bad, bad);
        Check("...and the settings start empty", C.Get("x", "k", "dflt") == "dflt");
        const std::string mark = std::string("v") + (bad[0] == '[' ? "2" : "1");
        C.Set("x", "k", mark);
        C.Save();
        Check("...and saving then writes a valid file", ReadJson(p)["addons"]["x"]["k"] == mark);
    }

    {   // a file that nests tens of thousands deep would overflow the stack when the settings are written back: it is damaged, not parsed
        const fs::path dir = base / "bad_deep";
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        const std::string deep = std::string(200000, '[') + std::string(200000, ']');
        WriteFile(p, deep);
        C.Load(p.wstring());
        Check("a file nested 200000 deep is refused (and the manager is still here)", C.Get("x", "k", "dflt") == "dflt");
        std::ifstream kept(fs::path(p) += ".corrupt");
        const std::string keptText((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
        Check("...kept as config.json.corrupt", keptText == deep);
        C.Set("x", "k", "v");
        C.Save();
        Check("...and saving writes a valid file", ReadJson(p)["addons"]["x"]["k"] == "v");
        // brackets inside a text value are not nesting
        const fs::path dir2 = base / "brackets_in_text";
        fs::create_directories(dir2);
        const fs::path p2 = dir2 / "config.json";
        WriteFile(p2, std::string("{\"addons\":{\"b\":{\"k\":\"") + std::string(500, '[') + "\"}}}");
        C.Load(p2.wstring());
        Check("brackets inside a text value do not count as nesting", C.Get("b", "k", "dflt") == std::string(500, '['));
        Check("a settings backup nested too deeply is refused with a message", !eam::ParseSettingsBackup(deep).message.empty() && eam::ParseSettingsBackup(deep).addonCount == 0);
        // the file is written whole: no leftover temporary
        Check("saving leaves no config.json.tmp behind", !fs::exists(fs::path(p) += ".tmp"));
    }

    {   // a second file loaded later starts from a clean slate
        const fs::path one = base / "first" / "config.json", two = base / "second" / "config.json";
        fs::create_directories(one.parent_path());
        fs::create_directories(two.parent_path());
        C.Load(one.wstring());
        C.Set("q", "k", "same");
        C.Save();
        C.Load(two.wstring());
        C.Set("q", "k", "same");
        C.Save();
        Check("a file loaded later is written even when it holds what the previous file did", fs::exists(two));
    }

    {   // a whole-file replacement, as settings restore does
        const fs::path dir = base / "replace";
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        C.Load(p.wstring());
        C.Replace(json{ { "addons", { { "r", { { "_enabled", false } } } } }, { "global", { { "log_level", 3 } } } });
        Check("Replace swaps everything and saves at once", !C.IsAddonEnabled("r", true) && ReadJson(p)["global"]["log_level"] == 3);
        C.Replace(json::array({ 1, 2 }));
        Check("Replace with something that is not an object leaves an empty object", C.Snapshot().is_object() && C.Snapshot().empty());

        // Loading settings from a file says "restart to apply". Until then the running addons still hold their old settings, and an addon that writes
        // everything back when it shuts down (Neural Rendering does) would put the old ones back over the imported ones.
        C.Replace(json{ { "addons", { { "nr", { { "look", "imported" } } } } }, { "global", { { "ui", { { "size", 150 } } } } } }, /*untilRestart=*/true);
        C.Set("nr", "look", "old, from memory");
        C.SetAddonEnabled("nr", false);
        C.GlobalSet("ui", "size", 100);
        C.Save();
        const json after = ReadJson(p);
        Check("after a settings import, later writes wait for the restart and the imported settings stay in the file",
              after["addons"]["nr"]["look"] == "imported" && !after["addons"]["nr"].contains("_enabled") && after["global"]["ui"]["size"] == 150);
        Check("...and they are what is read until then", C.Get("nr", "look") == "imported" && C.IsAddonEnabled("nr", true));
        Check("...and the manager knows a restart is pending", C.RestartPending());
        C.Load(p.wstring());
        Check("loading the file (the restart) lifts that", !C.RestartPending() && (C.Set("nr", "look", "new"), C.Get("nr", "look") == "new"));
    }

    {   // the very old settings file, addons_config.ini
        const fs::path dir = base / "migrate";
        fs::create_directories(dir / "One");
        fs::create_directories(dir / "Two");
        fs::create_directories(dir / "Three");
        WriteFile(dir / "addons_config.ini", "[Addons]\nOne=0\nTwo=1\n");
        const fs::path p = dir / "config.json";
        C.Load(p.wstring());
        Check("settings from the old .ini are carried over, one per addon folder", !C.IsAddonEnabled("One", true) && C.IsAddonEnabled("Two", false) && C.IsAddonEnabled("Three", false));
        Check("...and written to config.json straight away", fs::exists(p) && ReadJson(p)["addons"]["One"]["_enabled"] == false);
    }

    {   // a hand-edited file whose sections are not objects: must not throw when an addon or the manager writes to it
        const fs::path dir = base / "shapes";
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        WriteFile(p, R"({"addons":[1,2],"global":"oops","keep":"me"})");
        C.Load(p.wstring());
        bool threw = false;
        try { C.Set("x", "k", "v"); C.SetAddonEnabled("y", false); C.GlobalSet("ui", "size", 100); C.Save(); } catch (...) { threw = true; }
        Check("sections that are not objects are replaced, not thrown over", !threw && C.Get("x", "k") == "v" && !C.IsAddonEnabled("y", true) && C.GlobalGetOr<int>("ui", "size", 0) == 100);
        Check("...and other keys in the file are kept", C.Snapshot().value("keep", std::string()) == "me");
        WriteFile(p, R"({"addons":{"z":"not an object","w":{"k":"1"}}})");
        C.Load(p.wstring());
        threw = false;
        try { C.Set("z", "k", "v"); C.SetAddonEnabled("z", true); } catch (...) { threw = true; }
        Check("one addon's entry that is not an object is replaced when it is written, the others are kept", !threw && C.Get("z", "k") == "v" && C.Get("w", "k") == "1");
    }

    {   // text that is not valid UTF-8 (an addon that stores a path in the ANSI code page): saving must not throw and must write the file
        const fs::path dir = base / "utf8";
        fs::create_directories(dir);
        const fs::path p = dir / "config.json";
        C.Load(p.wstring());
        bool threw = false;
        try { C.Set("x", "path", "C:\\Users\\Jos\xE9\\model.dll"); C.GlobalSet(nullptr, "note", std::string("caf\xE9")); C.Save(); } catch (...) { threw = true; }
        Check("invalid UTF-8 from an addon does not make Save throw", !threw);
        Check("...and the file is still written, with the rest intact", fs::exists(p) && ReadJson(p)["addons"]["x"].contains("path"));
        std::string backup;
        threw = false;
        try { backup = MakeSettingsBackupText(C.Snapshot(), "test"); } catch (...) { threw = true; }
        Check("a settings backup of the same settings does not throw either", !threw && backup.find("eam_settings_backup") != std::string::npos);
    }
}

static bool PreSkips(uint32_t, uint32_t, uint32_t, void* user) { ++*(int*)user; return true; }
static bool PreLetsThrough(uint32_t, uint32_t, uint32_t, void* user) { ++*(int*)user; return false; }
static void PostCounts(uint32_t, uint32_t, uint32_t, void* user) { ++*(int*)user; }
static bool PreFaults(uint32_t, uint32_t, uint32_t, void*) { volatile int* p = nullptr; return *p == 1; }
static void PostFaults(uint32_t, uint32_t, uint32_t, void*) { volatile int* p = nullptr; *p = 1; }
static void EventGot(uint32_t id, const void* data, uint32_t size, void* user) {
    auto* seen = (std::pair<uint32_t, std::string>*)user;
    seen->first = id;
    seen->second.assign((const char*)data, size);
}

// What an addon can ask of the host: settings, events, dispatch callbacks, status and metrics.
static void TestHost(const fs::path& T) {
    printf("== host interface\n");
    const fs::path dir = T / "host";
    fs::create_directories(dir);
    ConfigManager::Instance().Load((dir / "config.json").wstring());
    HostImpl h;

    Check("the host reports the addon API version", h.GetHostVersion() == (uint32_t)EAM_API_VERSION_INT);
    h.SetConfig("a", "k", "v");
    Check("a setting written by an addon reads back", std::string(h.GetConfig("a", "k", "")) == "v");
    Check("a missing setting gives the default given", std::string(h.GetConfig("a", "nope", "dflt")) == "dflt");
    const char* nothing = h.GetConfig(nullptr, nullptr, nullptr);
    Check("null arguments are treated as empty text", nothing && *nothing == 0);

    const char* first = h.GetConfig("a", "k", "");
    for (int i = 0; i < 500; ++i) h.GetConfig("a", "k", "");
    Check("a returned setting stays valid for hundreds of later calls", std::string(first) == "v");

    int skip = 0, through = 0, post = 0;
    h.SetPreDispatchCallback(PreSkips, &skip);
    h.SetPreDispatchCallback(PreLetsThrough, &through);
    h.SetPostDispatchCallback(PostCounts, &post);
    Check("every pre-dispatch callback runs, and any of them can skip the dispatch", h.InvokePreDispatch(1, 2, 3) && skip == 1 && through == 1);
    h.InvokePostDispatch(1, 2, 3);
    Check("post-dispatch callbacks run", post == 1);
    h.SetPreDispatchCallback(PreLetsThrough, &skip);   // the same owner again replaces its callback
    Check("a callback set again by the same owner replaces the old one", !h.InvokePreDispatch(1, 2, 3) && skip == 2 && through == 2);
    h.SetPreDispatchCallback(nullptr, &skip);          // no callback removes the owner's entry
    h.InvokePreDispatch(1, 2, 3);
    Check("setting no callback removes that owner's entry", skip == 2 && through == 3);

    h.SetPostDispatchCallback(nullptr, &post);
    h.InvokePostDispatch(1, 2, 3);
    Check("...for post-dispatch as well", post == 1);

    // a dispatch callback that faults is called from Lossless Scaling's render thread: it must be dropped, never crash the process
    int beforeFault = 0, afterFault = 0;
    h.SetPreDispatchCallback(PreLetsThrough, &beforeFault);
    h.SetPreDispatchCallback(PreFaults, &afterFault);
    h.SetPostDispatchCallback(PostFaults, &afterFault);
    const size_t hooksBefore = h.DispatchHookCount();
    h.InvokePreDispatch(1, 2, 3);    // unguarded, this ends the test program with an access violation
    h.InvokePostDispatch(1, 2, 3);
    Check("a faulting pre- or post-dispatch callback does not crash the dispatch, and the others still run", beforeFault == 1);
    Check("...the faulting ones are removed", h.DispatchHookCount() == hooksBefore - 2, std::to_string(h.DispatchHookCount()) + " of " + std::to_string(hooksBefore));
    h.InvokePreDispatch(1, 2, 3);
    Check("...and are not called again", beforeFault == 2);
    h.SetPreDispatchCallback(nullptr, &beforeFault);

    h.SetD3D11Device((void*)0x10, (void*)0x20);
    Check("the device and context handed over by the hook are what addons get", h.GetD3D11Device() == (void*)0x10 && h.GetD3D11DeviceContext() == (void*)0x20);

    std::pair<uint32_t, std::string> seen{ 0, "" };
    const uint32_t evt = EAM_EVENT_CUSTOM + 7;
    h.SubscribeEvent(evt, EventGot, &seen);
    h.PublishEvent(evt, "payload", 7);
    Check("an event published by one addon reaches a subscriber with its data", seen.first == evt && seen.second == "payload");
    seen = { 0, "" };
    h.UnsubscribeEvent(evt, EventGot);
    h.PublishEvent(evt, "again", 5);
    Check("after unsubscribing nothing arrives", seen.first == 0);

    h.SetStatus("hostTest", "Running", 1);
    h.PublishMetric("hostTest", "fps", 59.5, "fps");
    const Metrics::Status st = Metrics::Instance().GetStatus("hostTest");
    const Metrics::Series se = Metrics::Instance().Get("hostTest", "fps", 10.0);
    Check("a status set by an addon can be read back", st.text == "Running" && st.level == 1);
    Check("a metric published by an addon can be read back", !se.samples.empty() && se.samples.back().v > 59.4f && se.unit == "fps");
    h.SetStatus(nullptr, nullptr, 0);
    h.PublishMetric(nullptr, nullptr, 1.0, nullptr);
    Check("null arguments to status and metrics are tolerated", true);
}

static AddonInfo MakeInfo(const std::string& id, std::vector<std::string> deps = {}) {
    AddonInfo a;
    a.id = id;
    a.folderName = std::wstring(id.begin(), id.end());
    a.manifest.dependencies = std::move(deps);
    return a;
}

static std::string Ids(const std::vector<AddonInfo>& v) {
    std::string s;
    for (const AddonInfo& a : v) s += (s.empty() ? "" : ",") + a.id;
    return s;
}

// Putting addons in an order where each comes after the ones it needs.
static void TestDependencies() {
    printf("== dependency order\n");
    {
        std::vector<AddonInfo> v{ MakeInfo("c", { "b" }), MakeInfo("b", { "a" }), MakeInfo("a") };
        const bool ok = AddonDependency::Resolve(v);
        Check("a chain comes out with what is needed first", ok && Ids(v) == "a,b,c", Ids(v));
    }
    {
        std::vector<AddonInfo> v{ MakeInfo("d", { "b", "c" }), MakeInfo("b", { "a" }), MakeInfo("c", { "a" }), MakeInfo("a") };
        const bool ok = AddonDependency::Resolve(v);
        const std::string r = Ids(v);
        Check("a diamond has its root first and its tip last", ok && r.front() == 'a' && r.back() == 'd', r);
    }
    {
        std::vector<AddonInfo> v{ MakeInfo("p", { "ghost" }), MakeInfo("q") };
        Check("a dependency that is not installed does not block", AddonDependency::Resolve(v) && v.size() == 2);
    }
    {
        std::vector<AddonInfo> v{ MakeInfo("m", { "n" }), MakeInfo("n", { "m" }) };
        Check("a circular dependency is reported and the list is left as it was", !AddonDependency::Resolve(v) && Ids(v) == "m,n", Ids(v));
        std::vector<AddonInfo> self{ MakeInfo("s", { "s" }) };
        Check("an addon that needs itself counts as circular", !AddonDependency::Resolve(self));
    }
    {
        std::vector<AddonInfo> none;
        Check("an empty list is fine", AddonDependency::Resolve(none) && none.empty());
    }
    {
        std::vector<AddonInfo> v{ MakeInfo("a1"), MakeInfo("a2"), MakeInfo("a3") };
        AddonDependency::Resolve(v);
        Check("addons that need nothing keep their order when the list is sorted", Ids(v) == "a1,a2,a3", Ids(v));
        AddonDependency::Resolve(v);
        Check("...and when it is sorted again", Ids(v) == "a1,a2,a3", Ids(v));
    }
}

// The SHA-256 of a DLL and what trusted_addons.json says about it.
static void TestSecurity(const fs::path& T) {
    printf("== addon safety checks\n");
    const fs::path dir = T / "sec";
    fs::create_directories(dir);
    WriteFile(dir / "abc.bin", "abc");
    WriteFile(dir / "empty.bin", "");
    WriteFile(dir / "long.bin", std::string(20000, 'a'));   // more than one read buffer

    Check("SHA-256 of 'abc' is the published value", AddonSecurity::ComputeSHA256((dir / "abc.bin").wstring()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Check("SHA-256 of an empty file is the published value", AddonSecurity::ComputeSHA256((dir / "empty.bin").wstring()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Check("SHA-256 of a file longer than one read is right", AddonSecurity::ComputeSHA256((dir / "long.bin").wstring()) == "cc17faaad36649c4603dda4d8ff97cb149722af0bcac0746305a2134ad2d0b97");
    Check("a file that cannot be read gives no hash", AddonSecurity::ComputeSHA256((dir / "nope.bin").wstring()).empty());

    const std::string abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const std::wstring dll = (dir / "abc.bin").wstring();
    WriteFile(dir / "trusted_addons.json", R"({"sec_ok":[")" + abc + R"("],"sec_bad":["00"],"sec_multi":["11",")" + abc + R"("],"sec_junk":"not a list"})");
    AddonSecurity::LoadTrustedHashes(dir.wstring());
    Check("a DLL whose hash is listed is trusted", AddonSecurity::VerifyDll(dll, "sec_ok") == SecurityVerdict::Trusted);
    Check("a listed addon whose DLL differs is tampered", AddonSecurity::VerifyDll(dll, "sec_bad") == SecurityVerdict::Tampered);
    Check("any one of several listed hashes will do", AddonSecurity::VerifyDll(dll, "sec_multi") == SecurityVerdict::Trusted);
    Check("an addon that is not listed is unknown", AddonSecurity::VerifyDll(dll, "sec_absent") == SecurityVerdict::Unknown);
    Check("an entry that is not a list is ignored", AddonSecurity::VerifyDll(dll, "sec_junk") == SecurityVerdict::Unknown);
    Check("a listed addon whose DLL cannot be read is unknown", AddonSecurity::VerifyDll((dir / "nope.bin").wstring(), "sec_ok") == SecurityVerdict::Unknown);

    const fs::path dir2 = T / "sec2";
    fs::create_directories(dir2);
    WriteFile(dir2 / "trusted_addons.json", R"({"sec_other":["00"]})");
    AddonSecurity::LoadTrustedHashes(dir2.wstring());
    Check("loading the list again replaces it", AddonSecurity::VerifyDll(dll, "sec_ok") == SecurityVerdict::Unknown);

    const fs::path dir4 = T / "sec4";
    fs::create_directories(dir4);
    std::string upper = abc;
    for (char& c : upper) c = (char)toupper((unsigned char)c);
    WriteFile(dir4 / "trusted_addons.json", R"({"sec_upper":[")" + upper + R"("]})");
    AddonSecurity::LoadTrustedHashes(dir4.wstring());
    Check("a listed hash may be written in capitals", AddonSecurity::VerifyDll(dll, "sec_upper") == SecurityVerdict::Trusted);

    const fs::path dir3 = T / "sec3";
    fs::create_directories(dir3);
    WriteFile(dir3 / "trusted_addons.json", "{ broken");
    AddonSecurity::LoadTrustedHashes(dir3.wstring());
    Check("a broken list is survived", true);
}

static void CountA(uint32_t, const void*, uint32_t, void* user) { ((std::string*)user)->push_back('A'); }
static void CountB(uint32_t, const void*, uint32_t, void* user) { ((std::string*)user)->push_back('B'); }
static void Boom(uint32_t, const void*, uint32_t, void*) { volatile int* p = nullptr; *p = 1; }
static void RemovesItself(uint32_t id, const void*, uint32_t, void* user) {
    ++*(int*)user;
    EventBus::Instance().Unsubscribe(id, RemovesItself);
}

// The event bus addons publish to and subscribe from.
static void TestEvents() {
    printf("== event bus\n");
    EventBus& bus = EventBus::Instance();
    const uint32_t base = EAM_EVENT_CUSTOM + 100;
    {
        std::string seen;
        bus.Subscribe(base, CountA, &seen);
        bus.Subscribe(base, CountB, &seen);
        bus.Publish(base);
        Check("subscribers are called in the order they subscribed", seen == "AB", seen);
        bus.Publish(base + 1);
        Check("an event nobody subscribed to reaches nobody", seen == "AB");
        bus.Unsubscribe(base, CountA);
        seen.clear();
        bus.Publish(base);
        Check("an unsubscribed callback is not called again", seen == "B", seen);
        bus.Unsubscribe(base, CountB);
    }
    {
        std::string one, two;
        bus.Subscribe(base + 2, CountA, &one);
        bus.Subscribe(base + 2, CountA, &two);
        bus.Unsubscribe(base + 2, CountA);
        bus.Publish(base + 2);
        Check("unsubscribing a callback removes it for every owner", one.empty() && two.empty());
        bus.Subscribe(base + 2, nullptr, nullptr);
        bus.Publish(base + 2);
        Check("a null callback is ignored", true);
    }
    {
        std::string after;
        bus.Subscribe(base + 3, Boom, nullptr);
        bus.Subscribe(base + 3, CountA, &after);
        bus.Publish(base + 3);
        Check("a subscriber that faults does not stop the others", after == "A", after);
        bus.Unsubscribe(base + 3, Boom);
        bus.Unsubscribe(base + 3, CountA);
    }
    {
        int calls = 0;
        bus.Subscribe(base + 4, RemovesItself, &calls);
        bus.Publish(base + 4);
        bus.Publish(base + 4);
        Check("a subscriber may unsubscribe itself while it is being called", calls == 1, std::to_string(calls));
    }
}

static void OnEvent(uint32_t, const void*, uint32_t, void* user) { ++*(int*)user; }

// Runs this test program again as a second process that tries to claim `folder`: exit code 0 = it got it, 3 = another process holds it.
static int ClaimInChild(const std::string& folder) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" claim \"" + std::wstring(folder.begin(), folder.end()) + L"\"";
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}

// One manager per Lossless Scaling folder: a second copy of Lossless Scaling (it exits a moment later) must not start a second manager.
static void TestInstanceGuard(const fs::path& T) {
    printf("== one manager per Lossless Scaling folder\n");
    const std::string a = (T / "lsA").string(), b = (T / "lsB").string();
    Check("the first to start in a folder owns it", instance::Claim(std::wstring(a.begin(), a.end())));
    Check("a second process starting in the same folder sees it taken", ClaimInChild(a) == 3);
    std::string upper = a; for (char& c : upper) c = (char)toupper((unsigned char)c);
    Check("...also when the folder is written in capitals or with a trailing separator", ClaimInChild(upper) == 3 && ClaimInChild(a + "/") == 3);
    Check("a process in another folder is not affected", ClaimInChild(b) == 0);
    instance::Release();
    Check("once the first lets go, the folder is free again", ClaimInChild(a) == 0);
}

// ---- an addon built against API 1.0 (manager 0.7.0), calling today's host through the declarations it was built with

static int g_frozenEvents = 0;
static void FrozenEvent(uint32_t id, const void* data, uint32_t size, void* user) { if (id == 0x10077 && size == 4 && *(const int*)data == 42 && user == &g_frozenEvents) ++g_frozenEvents; }
static bool FrozenPre(uint32_t, uint32_t, uint32_t, void*) { return false; }

static void TestFrozenAbi() {
    printf("== an addon built against API 1.0 still reaches the right calls\n");
    static_assert(sizeof(EamAddonEventData) == sizeof(abi_v1_0::AddonEventData) && offsetof(EamAddonEventData, addonVersion) == offsetof(abi_v1_0::AddonEventData, addonVersion), "event payload layout changed");
    static_assert(sizeof(EamShaderEventData) == sizeof(abi_v1_0::ShaderEventData) && offsetof(EamShaderEventData, dataSize) == offsetof(abi_v1_0::ShaderEventData, dataSize), "event payload layout changed");
    static_assert(EAM_EVENT_ADDON_LOADED == abi_v1_0::ADDON_LOADED && EAM_EVENT_ADDON_UNLOADED == abi_v1_0::ADDON_UNLOADED && EAM_EVENT_SETTINGS_CHANGED == abi_v1_0::SETTINGS_CHANGED &&
                  EAM_EVENT_SHADER_INTERCEPTED == abi_v1_0::SHADER_INTERCEPTED && EAM_EVENT_HOST_SHUTDOWN == abi_v1_0::HOST_SHUTDOWN && EAM_EVENT_SETTINGS_APPLIED == abi_v1_0::SETTINGS_APPLIED &&
                  EAM_EVENT_D3D11_DEVICE_READY == abi_v1_0::D3D11_DEVICE_READY && EAM_EVENT_D3D11_DEVICE_CHANGED == abi_v1_0::D3D11_DEVICE_CHANGED && EAM_EVENT_CUSTOM == abi_v1_0::CUSTOM, "event ids changed");
    HostImpl h;
    abi_v1_0::IHost* const old = reinterpret_cast<abi_v1_0::IHost*>(static_cast<IHost*>(&h));   // what the addon was handed
    Check("GetHostVersion", old->GetHostVersion() == EAM_API_VERSION_INT);
    old->SetConfig("frozen", "key", "value");
    Check("SetConfig and GetConfig", std::string(old->GetConfig("frozen", "key", "")) == "value" && std::string(old->GetConfig("frozen", "absent", "dflt")) == "dflt");
    old->SubscribeEvent(0x10077, FrozenEvent, &g_frozenEvents);
    const int payload = 42;
    old->PublishEvent(0x10077, &payload, sizeof payload);
    old->UnsubscribeEvent(0x10077, FrozenEvent);
    old->PublishEvent(0x10077, &payload, sizeof payload);
    Check("SubscribeEvent, PublishEvent and UnsubscribeEvent", g_frozenEvents == 1);
    h.SetD3D11Device(reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000));
    Check("GetD3D11Device and GetD3D11DeviceContext", old->GetD3D11Device() == reinterpret_cast<void*>(0x1000) && old->GetD3D11DeviceContext() == reinterpret_cast<void*>(0x2000));
    const size_t hooks = h.DispatchHookCount();
    old->SetPreDispatchCallback(FrozenPre, nullptr);
    old->SetPostDispatchCallback(nullptr, nullptr);
    Check("SetPreDispatchCallback and SetPostDispatchCallback", h.DispatchHookCount() == hooks + 1);
    old->SetPreDispatchCallback(nullptr, nullptr);
    Check("GetCurrentComputeShader and GetDispatchCount answer", old->GetCurrentComputeShader() == nullptr && old->GetDispatchCount() == D3D11Hook::GetDispatchCount());
    old->SetStatus("frozen", "status from a 1.0 addon", 1);
    Check("SetStatus", Metrics::Instance().GetStatus("frozen").text == "status from a 1.0 addon");
    old->PublishMetric("frozen", "abi", 7.0, "x");
    const auto series = Metrics::Instance().Get("frozen", "abi", 60.0);
    Check("PublishMetric", !series.samples.empty() && series.samples.back().v == 7.0f);
    old->Log(abi_v1_0::INFO, "a line from a 1.0 addon");
    old->SaveConfig();
    Check("Log and SaveConfig return", true);
}

// ---- images for addons' panels (API 1.2), made on the window's device

static void TestImages() {
    printf("== images for addon panels\n");
    HostImpl h;
    const uint32_t pixels[4] = { 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu };
    Check("without the window's device there is no image, and giving back nothing is harmless", h.CreateImage(pixels, 2, 2, 8) == nullptr);
    h.ReleaseImage(nullptr);
    ID3D11Device* dev = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    if (!dev) { Check("a WARP device for the test", false); return; }
    SetIconDevice(dev);
    void* const image = h.CreateImage(pixels, 2, 2, 8);
    Check("with it, pixels become an image an addon can draw", image != nullptr);
    Check("a picture larger than 4096 pixels a side, or a pitch too small for its width, is refused",
          h.CreateImage(pixels, 5000, 1, 20000) == nullptr && h.CreateImage(pixels, 2, 2, 4) == nullptr);
    h.ReleaseImage(image);
    SetIconDevice(nullptr);
    dev->Release();
}

// ---- the resource hook, on this program's own imports of kernel32's resource functions

static void TestResourceHook() {
    printf("== resource replacement (the hook addons use to replace Lossless Scaling's shaders)\n");
    static const char one[] = "first replacement", two[] = "the second, longer replacement";
    const HRSRC realManifest = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));   // RT_MANIFEST, before the hook
    const bool hooked = ShaderHook::InstallHooks(GetModuleHandleW(nullptr), [&](const wchar_t* name, const wchar_t*, const void** data, uint32_t* size) {
        if (IS_INTRESOURCE(name)) return false;
        if (!wcscmp(name, L"ONE")) { *data = one; *size = sizeof one; return true; }
        if (!wcscmp(name, L"TWO")) { *data = two; *size = sizeof two; return true; }
        return false;
    });
    Check("this program's resource imports are hooked", hooked);
    const HRSRC a = FindResourceW(nullptr, L"ONE", MAKEINTRESOURCEW(10)), b = FindResourceW(nullptr, L"TWO", MAKEINTRESOURCEW(10));
    Check("two replaced resources with names get different handles", a && b && a != b);
    auto bytes = [](HRSRC r) {
        const HGLOBAL loaded = LoadResource(nullptr, r);
        const void* p = LockResource(loaded);
        return p ? std::string(static_cast<const char*>(p), SizeofResource(nullptr, r)) : std::string();
    };
    Check("...and each reads back its own bytes", bytes(a) == std::string(one, sizeof one) && bytes(b) == std::string(two, sizeof two));
    Check("the same resource with the same bytes gets the same handle again", FindResourceW(nullptr, L"ONE", MAKEINTRESOURCEW(10)) == a);
    Check("freeing a replacement reports success, as FreeResource does", FreeResource(LoadResource(nullptr, a)) == FALSE);
    Check("a resource no addon replaces comes from the program as before", FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24)) == realManifest);
    const char* const handedOut = static_cast<const char*>(LockResource(LoadResource(nullptr, b)));
    ShaderHook::UninstallHooks();
    Check("after the hook is taken out, nothing is replaced", FindResourceW(nullptr, L"ONE", MAKEINTRESOURCEW(10)) == nullptr);
    Check("...and replacement bytes handed out earlier are still readable", handedOut && std::string(handedOut) == two);
}

// ---- the dispatch hook, on real D3D11 devices

struct DispatchSeen { int pre = 0, post = 0; ID3D11DeviceContext* ctx = nullptr; void* shader = nullptr; bool skip = false; };
static bool CountPre(uint32_t, uint32_t, uint32_t, void* user) {
    auto* s = static_cast<DispatchSeen*>(user);
    ++s->pre; s->ctx = D3D11Hook::DispatchingContext(); s->shader = D3D11Hook::GetCurrentComputeShader();
    return s->skip;
}
static void CountPost(uint32_t, uint32_t, uint32_t, void* user) { ++static_cast<DispatchSeen*>(user)->post; }

static void TestDispatchHook() {
    printf("== dispatch callbacks around Lossless Scaling's compute passes\n");
    HostImpl h;
    D3D11Hook::Initialize(&h);   // no Lossless_original.dll here: only the host is taken
    Check("the Dispatch implementations are hooked", D3D11Hook::InstallDispatchHooks() > 0);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; ID3D11Device* other = nullptr; ID3D11DeviceContext* otherCtx = nullptr;
    ID3D11Device* second = nullptr; ID3D11DeviceContext* secondCtx = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &other, nullptr, &otherCtx);
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &second, nullptr, &secondCtx);
    if (!ctx || !otherCtx || !secondCtx) { Check("WARP devices for the test", false); return; }
    ID3DBlob* code = nullptr; ID3D11ComputeShader* cs = nullptr;
    const char src[] = "RWTexture2D<float> o : register(u0); [numthreads(1,1,1)] void main(uint3 i : SV_DispatchThreadID) { }";
    if (SUCCEEDED(D3DCompile(src, sizeof src - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr))) {
        dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs); code->Release();
    }
    D3D11Hook::Attach(dev, ctx);
    Check("the device made by Lossless Scaling is handed to the host", h.GetD3D11Device() == dev && h.GetD3D11DeviceContext() == ctx);
    DispatchSeen seen;
    h.SetPreDispatchCallback(CountPre, &seen);
    h.SetPostDispatchCallback(CountPost, &seen);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    Check("a Dispatch on its context runs the pre- and post-dispatch callbacks once each", seen.pre == 1 && seen.post == 1, std::to_string(seen.pre) + "/" + std::to_string(seen.post));
    Check("...and during them the dispatching context and the bound compute shader are known", seen.ctx == ctx && cs && seen.shader == cs);
    Check("outside a dispatch there is no dispatching context", D3D11Hook::DispatchingContext() == nullptr && D3D11Hook::GetCurrentComputeShader() == nullptr);
    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch(1, 1, 1); ctx->Dispatch(1, 1, 1);
    Check("they still run once the context is multithread protected (as Lossless Scaling's is)", seen.pre == 3 && seen.post == 3, std::to_string(seen.pre));
    otherCtx->Dispatch(1, 1, 1);
    Check("a Dispatch on another device's context does not run them", seen.pre == 3);
    Check("the dispatch count is Lossless Scaling's dispatches only", D3D11Hook::GetDispatchCount() == 3, std::to_string(D3D11Hook::GetDispatchCount()));
    D3D11Hook::Attach(second, secondCtx);   // Lossless Scaling makes a second device when scaling starts; its passes run on the first
    ctx->Dispatch(1, 1, 1); secondCtx->Dispatch(1, 1, 1);
    Check("with a second device made, both its contexts are watched (the passes run on the first)", seen.pre == 5 && seen.post == 5, std::to_string(seen.pre));
    Check("...and the host's device is the newest", h.GetD3D11Device() == second);
    seen.skip = true;
    ctx->Dispatch(1, 1, 1);
    Check("a pre-dispatch callback that asks to skip: no post-dispatch callback", seen.pre == 6 && seen.post == 5);
    seen.skip = false;
    D3D11Hook::Shutdown();
    ctx->Dispatch(1, 1, 1);
    Check("after shutdown no callback runs, and dispatching still works", seen.pre == 6);
    h.SetPreDispatchCallback(nullptr, &seen); h.SetPostDispatchCallback(nullptr, &seen);
    if (cs) cs->Release();
    secondCtx->Release(); second->Release(); otherCtx->Release(); other->Release(); ctx->Release(); dev->Release();
}

int main(int argc, char** argv) {
    if (argc > 2 && !strcmp(argv[1], "claim")) {   // the second process of TestInstanceGuard
        const std::string f = argv[2];
        return instance::Claim(std::wstring(f.begin(), f.end())) ? 0 : 3;
    }
    if (argc > 1 && !strcmp(argv[1], "abrupt-gpu")) {
        // The process ends with the Performance tab's sampler thread running and GpuStats::Shutdown never called, as when Lossless Scaling exits.
        // A std::thread still joinable when the singleton is destroyed calls std::terminate: the exit code would be a crash.
        GpuStats::Instance().Wanted();
        Sleep(300);
        printf("abrupt exit with the GPU sampler running\n"); fflush(stdout);
        exit(0);   // runs the static destructors, as a DLL's are run when the process ends
    }
    const fs::path T = fs::temp_directory_path() / ("eam_coretest_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(T, ec);
    const fs::path A = T / "addons";
    fs::create_directories(A);
    if (!fs::exists(ExeDir() / "eam_testaddon.dll")) { printf("FAIL  eam_testaddon.dll is not beside the test program\n"); return 1; }

    // ---- a folder of addons in every shape the manager has to cope with
    const fs::path alpha = MakeAddon(A, "alpha", "alpha.dll", R"({"name":"Alpha","version":"1.2.3","author":"A","description":"first","tags":["x","y"]})");
    WriteFile(alpha / "icon.png", "not really a picture");
    WriteFile(alpha / "settings.ini", "[a]\n");
    const fs::path beta = MakeAddon(A, "beta", "custom.dll", R"({"dll":"custom.dll","icon":"pic.jpg"})");
    fs::copy_file(ExeDir() / "eam_testaddon.dll", beta / "other.dll");
    WriteFile(beta / "pic.jpg", "x");
    const fs::path gamma = MakeAddon(A, "gamma", "zzz.dll");                                   // no manifest, DLL not named after the folder
    WriteFile(gamma / "icon.svg", R"(<svg viewBox="0 0 48 48"><!-- a vector icon --><path d="M4 4h40v40H4Z" stroke="red"/><path fill="none" d="M10 24h28"/></svg>)");
    MakeAddon(A, "newer", "newer.dll", R"({"min_host_version":"9.0.0"})");                     // needs a much newer manager
    const fs::path minok = MakeAddon(A, "minok", "minok.dll", R"({"min_host_version":"1.0.0"})");
    WriteFile(minok / "icon.svg", "<svg viewBox=\"0 0 24 24\"><rect x=\"3\" y=\"3\" width=\"10\" height=\"10\" rx=\"2\"/><circle cx=\"12\" cy=\"12\" r=\"4\"/><path\n  d='M1 1h2'/><line x1=\"0\" y1=\"0\" x2=\"5\" y2=\"5\"/><polygon points=\"0,0 4,0 4,4\"/><rect width=\"0\" height=\"5\"/></svg>");   // (an icon of basic shapes, the attributes written in other ways)
    MakeAddon(A, "mingarbage", "mingarbage.dll", R"({"min_host_version":"abc"})");             // unreadable: must not block
    const fs::path restart = MakeAddon(A, "restart", "restart.dll", nullptr, "restart");
    const fs::path crashy = MakeAddon(A, "crashy", "crashy.dll", nullptr, "crash_init");
    MakeAddon(A, "badjson", "badjson.dll", "{ this is not json");
    MakeAddon(A, "dep_b", "dep_b.dll", R"({"dependencies":["dep_a"]})");
    MakeAddon(A, "dep_a", "dep_a.dll");
    WriteFile(A / "nodll" / "readme.txt", "no DLL in here");
    MakeAddon(A, ".hidden", "hidden.dll");                                                     // dot folders are staging areas
    MakeAddon(A, "renamed_new", "renamed_new.dll", R"({"renamed_from":["renamed_old"]})");                // an addon that changed its folder name
    MakeAddon(A, "renamed_old", "renamed_old.dll");                                            // ...and the folder it used to have
    MakeAddon(A, "LSP-ReShade", "LSP_ReShade.dll");                                            // a retired standalone addon: built in now
    WriteFile(A / "config.json", R"({"addons":{"beta":{"_enabled":false},"renamed_old":{"_enabled":false,"keep":"me"}},"global":{"security_level":0}})");

    setvbuf(stdout, nullptr, _IONBF, 0);
    try { TestInstanceGuard(T); TestConfig(T); TestHost(T); TestDependencies(); TestSecurity(T); TestEvents(); TestFrozenAbi(); TestImages(); TestResourceHook(); TestDispatchHook(); } catch (const std::exception& e) { Check("the settings tests ran to the end", false, e.what()); }

    HostImpl host;
    int loadedEvents = 0, unloadedEvents = 0;
    host.SubscribeEvent(EAM_EVENT_ADDON_LOADED, OnEvent, &loadedEvents);
    host.SubscribeEvent(EAM_EVENT_ADDON_UNLOADED, OnEvent, &unloadedEvents);

    {
        AddonManager mgr(&host, A.wstring());
        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);

        // ---- scanning
        printf("== scanning\n");
        mgr.ScanAddons();
        Check("finds the twelve usable addons (the renamed one counts once)", mgr.GetAddons().size() == 12, "found " + std::to_string(mgr.GetAddons().size()));
        Check("a folder with no DLL is not an addon", IndexOf(mgr, "nodll") < 0);
        Check("a dot folder is not an addon", IndexOf(mgr, ".hidden") < 0);
        Check("the folder of a retired standalone addon is ignored, since that feature is built in", IndexOf(mgr, "LSP-ReShade") < 0);
        Check("an addon that was renamed hides its old folder", IndexOf(mgr, "renamed_new") >= 0 && IndexOf(mgr, "renamed_old") < 0);
        Check("...and keeps the settings it had under the old name, including being switched off", ConfigManager::Instance().Get("renamed_new", "keep") == "me" && !Find(mgr, "renamed_new")->enabled);
        bool oldPresent = true;
        EnabledInFile(A / "config.json", "renamed_old", &oldPresent);
        Check("...moved, not copied, and saved", !oldPresent && ConfigManager::Instance().Get("renamed_old", "keep", "gone") == "gone");
        AddonInfo* a = Find(mgr, "alpha");
        Check("manifest fields are read", a && a->manifest.parsed && a->manifest.name == "Alpha" && a->manifest.version == "1.2.3" && a->manifest.author == "A" &&
              a->manifest.description == "first" && a->manifest.tags.size() == 2);
        Check("the DLL named after the folder is used", a && fs::path(a->dllPath).filename() == "alpha.dll");
        Check("icon.png is picked up", a && fs::path(a->iconPath).filename() == "icon.png");
        Check("an .ini file is picked up as the addon's config", a && fs::path(a->configPath).filename() == "settings.ini");
        AddonInfo* b = Find(mgr, "beta");
        Check("the manifest's DLL name wins over other DLLs", b && fs::path(b->dllPath).filename() == "custom.dll");
        Check("the manifest's icon name wins", b && fs::path(b->iconPath).filename() == "pic.jpg");
        Check("an addon switched off in config.json starts switched off", b && !b->enabled);
        Check("an addon with no entry in config.json starts switched on", a && a->enabled);
        AddonInfo* g = Find(mgr, "gamma");
        Check("with no manifest and no matching name, the first DLL found is used", g && fs::path(g->dllPath).filename() == "zzz.dll");
        Check("with no manifest the display name is the folder name", g && g->GetDisplayName() == "gamma" && !g->manifest.parsed);
        Check("an icon.svg is read as shapes: every path, its viewBox width, and no picture file", g && g->iconSvg.size() == 2 && g->iconSvg[1] == "M10 24h28" &&
              g->iconSvgView == 48.0f && g->iconPath.empty());
        AddonInfo* sh = Find(mgr, "minok");
        Check("an icon.svg of basic shapes (rect, circle, a path with its d on the next line and in single quotes, line, polygon) is read as paths; an empty rect is not", sh && sh->iconSvg.size() == 5 &&
              sh->iconSvg[0] == "M5 3h6a2 2 0 0 1 2 2v6a2 2 0 0 1 -2 2h-6a2 2 0 0 1 -2 -2v-6a2 2 0 0 1 2 -2Z" && sh->iconSvg[1] == "M8 12a4 4 0 1 0 8 0a4 4 0 1 0 -8 0Z" &&
              sh->iconSvg[2] == "M1 1h2" && sh->iconSvg[3] == "M0 0L5 5" && sh->iconSvg[4] == "M0 0L4 0L4 4Z", sh && sh->iconSvg.size() > 0 ? sh->iconSvg[0] : std::string("none"));
        AddonInfo* bj = Find(mgr, "badjson");
        Check("a broken addon.json does not hide the addon", bj && !bj->manifest.parsed);
        Check("dependencies are listed before what needs them", IndexOf(mgr, "dep_a") >= 0 && IndexOf(mgr, "dep_a") < IndexOf(mgr, "dep_b"));

        // ---- loading
        printf("== loading\n");
        mgr.LoadAddons();
        Check("an enabled addon is loaded", Find(mgr, "alpha")->IsLoaded());
        Check("a disabled addon is not loaded", !Find(mgr, "beta")->IsLoaded());
        AddonInfo* nw = Find(mgr, "newer");
        Check("an addon that needs a newer API is refused, with a reason", !nw->IsLoaded() && nw->errorMessage.find("Needs a newer") != std::string::npos, nw->errorMessage);
        Check("a stated minimum API that is met does not block", Find(mgr, "minok")->IsLoaded());
        Check("an unreadable minimum does not block", Find(mgr, "mingarbage")->IsLoaded());
        Check("name and version fall back to what the DLL reports", Find(mgr, "gamma")->manifest.name == "Test Addon" && Find(mgr, "gamma")->manifest.version == "9.9.9");
        Check("the manifest is not overridden by the DLL", Find(mgr, "alpha")->manifest.name == "Alpha");
        Check("a restart-required addon reports it", Find(mgr, "restart")->RequiresRestart() && !Find(mgr, "alpha")->RequiresRestart());

        // ---- initialising
        printf("== initialising\n");
        mgr.InitializeAddons(ctx);
        Check("an addon's initialise is called once", Calls(alpha, "init") == 1);
        AddonInfo* cr = Find(mgr, "crashy");
        Check("an addon that faults in initialise is marked, not fatal", cr->faulted && cr->errorMessage == "Crashed during initialization");
        Check("'loaded' events are sent for the addons that started (eight of nine)", loadedEvents == 8, "events " + std::to_string(loadedEvents));

        // ---- switching on and off
        printf("== switching on and off\n");
        const fs::path cfg = A / "config.json";
        mgr.ToggleAddon(IndexOf(mgr, "alpha"), false);
        Check("switching off unloads the addon", !Find(mgr, "alpha")->IsLoaded() && Calls(alpha, "shutdown") == 1);
        Check("switching off sends an 'unloaded' event", unloadedEvents == 1);
        Check("switching off is saved to config.json", !EnabledInFile(cfg, "alpha"));
        mgr.ToggleAddon(IndexOf(mgr, "alpha"), true);
        Check("switching on loads and starts it again", Find(mgr, "alpha")->IsLoaded() && Calls(alpha, "init") == 2);
        Check("switching on is saved to config.json", EnabledInFile(cfg, "alpha"));
        mgr.ToggleAddon(IndexOf(mgr, "restart"), false);
        Check("a restart-required addon stays loaded when switched off", Find(mgr, "restart")->IsLoaded() && !Find(mgr, "restart")->enabled && Calls(restart, "shutdown") == 0);
        bool present = false;
        EnabledInFile(cfg, "restart", &present);
        Check("...but the choice is saved for the next start", present && !EnabledInFile(cfg, "restart"));
        mgr.ToggleAddon(IndexOf(mgr, "restart"), true);

        // ---- addons that cannot run side by side (addon.json "conflicts", named by either one)
        printf("== conflicting addons\n");
        Find(mgr, "alpha")->manifest.conflicts = { "minok" };
        std::vector<std::string> off = mgr.ToggleAddon(IndexOf(mgr, "alpha"), false);
        off = mgr.ToggleAddon(IndexOf(mgr, "alpha"), true);
        Check("turning an addon on turns off the one it conflicts with, and says which", Find(mgr, "alpha")->IsLoaded() && !Find(mgr, "minok")->enabled &&
              !Find(mgr, "minok")->IsLoaded() && off.size() == 1 && !EnabledInFile(cfg, "minok"), off.empty() ? "none" : off[0]);
        off = mgr.ToggleAddon(IndexOf(mgr, "minok"), true);
        Check("...and the other way round, when only one of them names the other", Find(mgr, "minok")->IsLoaded() && !Find(mgr, "alpha")->enabled && off.size() == 1);
        Find(mgr, "alpha")->enabled = true;   // both on, as an older config.json could have them
        mgr.LoadAddons();
        Check("both on at start: the first in the list stays on, the other is switched off", Find(mgr, "alpha")->IsLoaded() && !Find(mgr, "minok")->enabled && !EnabledInFile(cfg, "minok"));
        Find(mgr, "alpha")->manifest.conflicts.clear();
        mgr.ToggleAddon(IndexOf(mgr, "minok"), true);
        Check("without a conflict both run", Find(mgr, "alpha")->IsLoaded() && Find(mgr, "minok")->IsLoaded());

        // ---- work in progress (addon.json "wip": true): listed, never switched on
        printf("== work in progress\n");
        {
            const fs::path wipJson = A / "wip_manifest.json";
            WriteFile(wipJson, "{ \"name\": \"W\", \"wip\": true }");
            AddonManifest m; ReadManifest(wipJson, m);
            AddonManifest plain; WriteFile(wipJson, "{ \"name\": \"W\" }"); ReadManifest(wipJson, plain);
            Check("the wip key is read, and is off when absent", m.wip && !plain.wip);
            WriteFile(wipJson, "{ \"name\": \"W\", \"enabled_by_default\": false }");
            AddonManifest off; ReadManifest(wipJson, off);
            Check("enabled_by_default is read, and is true when absent", !off.enabledByDefault && plain.enabledByDefault);
            // paths in addon.json stay inside the addon's folder
            WriteFile(wipJson, "{ \"name\": \"P\", \"dll\": \"..\\\\evil.dll\", \"icon\": \"C:\\\\x.svg\", \"runtimes\": [ { \"name\": \"up\", \"file\": \"../x.dll\" }, { \"name\": \"ok\", \"file\": \"sub/y.dll\" } ] }");
            AddonManifest esc; ReadManifest(wipJson, esc);
            Check("a dll or an icon that climbs out of the folder or names a drive is dropped", esc.dll.empty() && esc.icon.empty());
            Check("...and so is a runtime file that does, while one in a subfolder stays", esc.runtimes.size() == 1 && esc.runtimes[0].name == "ok");
            WriteFile(wipJson, "{ \"name\": \"Q\", \"dll\": \"sub\\\\q.dll\" }");
            AddonManifest sub; ReadManifest(wipJson, sub);
            Check("a dll in a subfolder is kept", sub.dll == "sub\\q.dll");
            Check("a version part over 255 does not wrap into a lower version", ParseApiVersion("1.256.0") >= ParseApiVersion("1.255.0"));
            fs::remove(wipJson);
        }
        Find(mgr, "beta")->manifest.wip = true;
        mgr.ToggleAddon(IndexOf(mgr, "beta"), true);
        Check("a work-in-progress addon cannot be switched on", !Find(mgr, "beta")->enabled && !Find(mgr, "beta")->IsLoaded() && Calls(beta, "init") == 0);
        Find(mgr, "beta")->manifest.wip = false;

        // ---- resources, settings panels
        printf("== resources and settings panels\n");
        const void* data = nullptr; uint32_t size = 0;
        Check("an addon can supply a resource", mgr.InterceptResource(L"test.shader", L"TEXT", &data, &size) && size == 5 && data && !memcmp(data, "HELLO", 5));
        Check("an unknown resource is not supplied", !mgr.InterceptResource(L"other", L"TEXT", &data, &size));
        mgr.RenderAddonSettings(IndexOf(mgr, "alpha"));
        Check("a loaded addon's settings panel is drawn", Calls(alpha, "settings") == 1);
        mgr.RenderAddonSettings(IndexOf(mgr, "beta"));
        Check("an unloaded addon's settings panel is not", Calls(beta, "settings") == 0);
        WriteFile(alpha / "mode.txt", "crash_panel");   // read at every call: its panel now crashes
        mgr.RenderAddonSettings(IndexOf(mgr, "alpha"));
        mgr.RenderAddonSettings(IndexOf(mgr, "alpha"));
        Check("a settings panel that crashes is survived, and switched off after the first time", Calls(alpha, "settings") == 2 && Find(mgr, "alpha")->settingsFaulted &&
              Find(mgr, "alpha")->errorMessage.find("crashed") != std::string::npos, Find(mgr, "alpha")->errorMessage);
        fs::remove(alpha / "mode.txt");
        for (int i = 0; i < (int)mgr.GetAddons().size(); ++i) mgr.ToggleAddon(i, false);
        Check("switched-off addons are not asked for resources", !mgr.InterceptResource(L"test.shader", L"TEXT", &data, &size));
        mgr.ToggleAddon(IndexOf(mgr, "alpha"), true);
        Check("...and one that is switched on again is", mgr.InterceptResource(L"test.shader", L"TEXT", &data, &size));
        mgr.ToggleAddon(IndexOf(mgr, "gamma"), true);

        // ---- security level
        printf("== security level\n");
        ConfigManager::Instance().GlobalSet(nullptr, "security_level", 2);
        mgr.ToggleAddon(IndexOf(mgr, "gamma"), false);
        mgr.ToggleAddon(IndexOf(mgr, "gamma"), true);
        Check("level 2 blocks an addon that is not on the trusted list", !Find(mgr, "gamma")->IsLoaded() && Find(mgr, "gamma")->errorMessage.rfind("Blocked:", 0) == 0, Find(mgr, "gamma")->errorMessage);
        ConfigManager::Instance().GlobalSet(nullptr, "security_level", 1);
        mgr.LoadAddonNow(IndexOf(mgr, "gamma"));
        Check("level 1 warns but still loads it", Find(mgr, "gamma")->IsLoaded());
        ConfigManager::Instance().GlobalSet(nullptr, "security_level", 0);

        // ---- removing
        printf("== removing\n");
        const size_t before = mgr.GetAddons().size();
        auto rm = mgr.RemoveAddon(IndexOf(mgr, "gamma"));
        Check("removing an addon succeeds", rm.ok && rm.id == "gamma", rm.message);
        Check("its folder is gone from addons", !fs::exists(gamma));
        bool inRemoved = false;
        if (fs::exists(A / ".removed")) for (auto& e : fs::directory_iterator(A / ".removed")) if (e.path().filename().string().rfind("gamma", 0) == 0) inRemoved = true;
        Check("...and moved to addons/.removed, not deleted", inRemoved);
        Check("it leaves the list", mgr.GetAddons().size() == before - 1 && IndexOf(mgr, "gamma") < 0);
        Check("it is recorded as switched off", !EnabledInFile(cfg, "gamma"));
        auto rr = mgr.RemoveAddon(IndexOf(mgr, "restart"));
        Check("a loaded restart-required addon cannot be removed yet", !rr.ok && rr.message.find("restarted") != std::string::npos, rr.message);
        Check("removing something that is not in the list is refused", !mgr.RemoveAddon(999).ok);

        // ---- installing
        printf("== installing\n");
        const fs::path incoming = T / "incoming";
        MakeAddon(incoming, "newone", "newone.dll", R"({"name":"New One"})");
        auto in = mgr.InstallAddon((incoming / "newone").wstring());
        Check("installing a folder succeeds", in.ok && in.id == "newone", in.message);
        Check("...into the addons folder, switched off", fs::exists(A / "newone" / "newone.dll") && Find(mgr, "newone") && !Find(mgr, "newone")->enabled && !EnabledInFile(cfg, "newone"));
        WriteFile(incoming / "empty" / "readme.txt", "nothing loadable");
        auto bad = mgr.InstallAddon((incoming / "empty").wstring());
        Check("a folder with no DLL is refused and leaves nothing behind", !bad.ok && !fs::exists(A / "empty"), bad.message);

        ImGui::DestroyContext(ctx);
    }   // the manager unloads everything here, including the addon that faulted

    Check("shutting down unloads what was loaded", Calls(alpha, "shutdown") == 5, std::to_string(Calls(alpha, "shutdown")));   // 2 switch-offs, 2 in the conflict checks, and this

    {   // an addon that forgets to take back its callbacks: after it is unloaded they would point into freed code (the dispatch one on Lossless Scaling's render thread)
        printf("== an addon that leaves its callbacks behind\n");
        const fs::path L = T / "leaky_addons";
        fs::create_directories(L);
        const fs::path leaky = MakeAddon(L, "leaky", "leaky.dll", nullptr, "leaky");
        HostImpl h2;
        AddonManager m2(&h2, L.wstring());
        ImGuiContext* c2 = ImGui::CreateContext();
        ImGui::SetCurrentContext(c2);
        m2.ScanAddons();
        m2.LoadAddons();
        m2.InitializeAddons(c2);
        const size_t subs = EventBus::Instance().SubscriberCount(0x7E57);
        h2.PublishEvent(0x7E57, nullptr, 0);
        h2.InvokePreDispatch(1, 1, 1);
        Check("its callbacks are called while it is loaded", subs >= 1 && Calls(leaky, "event") == 1 && Calls(leaky, "dispatch") == 1 && h2.DispatchHookCount() == 1);
        h2.SetPreDispatchCallback(PreLetsThrough, reinterpret_cast<void*>(0x7E57));   // the same userData as the addon's, from another module
        Check("another addon's callback with the same userData is kept beside it, not in its place", h2.DispatchHookCount() == 2);
        h2.SetPreDispatchCallback(nullptr, reinterpret_cast<void*>(0x7E57));
        Check("...and removing it removes only that one", h2.DispatchHookCount() == 1);
        m2.ToggleAddon(IndexOf(m2, "leaky"), false);
        Check("switching it off removes the callbacks it left registered", EventBus::Instance().SubscriberCount(0x7E57) == subs - 1 && h2.DispatchHookCount() == 0,
              std::to_string(EventBus::Instance().SubscriberCount(0x7E57)) + " subscribers, " + std::to_string(h2.DispatchHookCount()) + " dispatch hooks");
        h2.PublishEvent(0x7E57, nullptr, 0);
        h2.InvokePreDispatch(1, 1, 1);
        Check("...so nothing calls into the unloaded DLL any more", Calls(leaky, "event") == 1 && Calls(leaky, "dispatch") == 1);
        m2.ToggleAddon(IndexOf(m2, "leaky"), true);
        h2.InvokePreDispatch(1, 1, 1);
        Check("switched on again, it registers afresh and works", Calls(leaky, "dispatch") == 2 && h2.DispatchHookCount() == 1);
        ImGui::DestroyContext(c2);
    }
    fs::remove_all(T, ec);
    printf("\n%s\n", g_failed ? "CORE TEST FAILED" : "CORE TEST PASSED");
    return g_failed ? 1 : 0;
}
