// Offline test of the installer's core. Everything happens in fake Lossless Scaling folders under %TEMP%: the "original" Lossless.dll is a stand-in that only
// carries Lossless Scaling's version resource, and the payload's Lossless.dll is the real one from the manager's build. Nothing here touches a real Lossless
// Scaling install or the person's remembered folder (the tests use a registry key of their own).
//   setup_test.exe [path to the manager's Lossless.dll]
#include "fileinfo.h"
#include "installer.h"
#include "lsfolder.h"
#include "state.h"
#include "../../manager/sdk/include/eam/version.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace setup;

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static fs::path g_exeDir, g_root, g_ours;

static void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}
static std::string Read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string Hash(const fs::path& p) { return Sha256File(p.wstring()); }
static bool Same(const fs::path& a, const fs::path& b) { const std::string x = Hash(a); return !x.empty() && x == Hash(b); }
static bool Gone(const fs::path& p) { std::error_code ec; return !fs::exists(p, ec); }

// A fake Lossless Scaling folder: a runnable LosslessScaling.exe (a copy of cmd.exe), Lossless.dll as the original, and things the installer must leave alone.
static fs::path MakeLs(const std::wstring& name, const wchar_t* originalDll = L"fake_original.dll") {
    const fs::path dir = g_root / name;
    fs::create_directories(dir);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::copy_file(fs::path(sys) / L"cmd.exe", dir / L"LosslessScaling.exe", fs::copy_options::overwrite_existing);
    fs::copy_file(g_exeDir / originalDll, dir / L"Lossless.dll", fs::copy_options::overwrite_existing);
    Write(dir / L"LosslessScaling.dll", "the managed part");
    Write(dir / L"addons" / L"config.json", "{ \"addons\": { \"mine\": { \"x\": \"1\" } } }");
    Write(dir / L"addons" / L"OtherAddon" / L"other.dll", "someone else's addon");
    Write(dir / L".removed_placeholder.txt", "x");
    Write(dir / L"addons" / L".removed" / L"old.txt", "removed earlier");
    return dir;
}

// A payload laid out like the release zip. `oursDll` is the real Lossless.dll (or a stand-in for an older one).
static fs::path MakePayload(const std::wstring& name, const fs::path& oursDll, const std::string& tag = "A") {
    const fs::path dir = g_root / name;
    fs::create_directories(dir);
    fs::copy_file(oursDll, dir / L"Lossless.dll", fs::copy_options::overwrite_existing);
    Write(dir / L"manager-icon.ico", "icon " + tag);
    Write(dir / L"manager-icon.png", "png " + tag);
    Write(dir / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll", "addon " + tag);
    Write(dir / L"addons" / L"DLSS5NR01" / L"nvngx.dll_dlss5nr01.dll", "helper " + tag);
    Write(dir / L"addons" / L"DLSS5NR01" / L"nr_selftest.exe", "selftest " + tag);
    Write(dir / L"addons" / L"DLSS5NR01" / L"dlss" / L"nvngx_dlss.dll", "runtime " + tag);
    Write(dir / L"addons" / L"DLSS5NR01" / L"addon.json", "{ \"id\": \"DLSS5NR01\", \"tag\": \"" + tag + "\" }");
    Write(dir / L"addons" / L"config.json", "{ \"payload\": \"must never be copied\" }");
    return dir;
}

static bool UntouchedByInstall(const fs::path& ls) {
    return Read(ls / L"addons" / L"config.json") == "{ \"addons\": { \"mine\": { \"x\": \"1\" } } }" && Read(ls / L"addons" / L"OtherAddon" / L"other.dll") == "someone else's addon" &&
           Read(ls / L"addons" / L".removed" / L"old.txt") == "removed earlier" && Read(ls / L"LosslessScaling.dll") == "the managed part";
}

int wmain(int argc, wchar_t** argv) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = fs::path(exe).parent_path();
    g_ours = argc > 1 ? fs::path(argv[1]) : g_exeDir / L".." / L".." / L".." / L"manager" / L"build" / L"Release" / L"Lossless.dll";
    g_root = fs::temp_directory_path() / (L"setup_test_" + std::to_wstring(GetCurrentProcessId()));
    { std::error_code fresh; fs::remove_all(g_root, fresh); }
    fs::create_directories(g_root);
    UseRegistryKeyForTest(L"Software\\LSAddonManager_SetupTest");
    if (!fs::exists(g_ours) || !fs::exists(g_exeDir / L"fake_original.dll")) { printf("FAIL  the manager's Lossless.dll or the fake DLLs are not where the test expects them\n"); return 2; }

    printf("== reading files without loading them\n");
    {
        const VersionResource orig = ReadVersionResource((g_exeDir / L"fake_original.dll").wstring()), mine = ReadVersionResource(g_ours.wstring());
        Check("a stand-in for Lossless Scaling's DLL reads as Lossless Scaling 3.2.2.0", orig.ok && orig.product == "Lossless Scaling" && orig.fileVersion == "3.2.2.0" && orig.company == "THS");
        Check("the manager's real Lossless.dll carries its version resource", mine.ok && mine.product == "Addon Manager for Lossless Scaling", mine.product);
        Check("...and its version is the release version, EAM_VERSION_STRING", mine.fileVersion == EAM_VERSION_STRING, mine.fileVersion + " vs " + EAM_VERSION_STRING);
        Write(g_root / L"plain.txt", "no resource here");
        Check("a file with no version resource reads as none", !ReadVersionResource((g_root / L"plain.txt").wstring()).ok && !ReadVersionResource(L"C:\\no\\such.dll").ok);
        Check("the kinds are told apart", InspectDll((g_exeDir / L"fake_original.dll").wstring()).kind == DllKind::Original && InspectDll(g_ours.wstring()).kind == DllKind::Ours &&
              InspectDll((g_exeDir / L"fake_other.dll").wstring()).kind == DllKind::Unknown && InspectDll(L"C:\\no\\such.dll").kind == DllKind::Missing);
        Check("SHA-256 of a known text", [] { Write(g_root / L"abc.txt", "abc"); return Hash(g_root / L"abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"; }());
        Check("versions compare as numbers", CompareVersions("0.10.0", "0.9.0") > 0 && CompareVersions("3.2.2.0", "3.2.2") == 0 && CompareVersions("0.4.1", "0.5.0") < 0 && CompareVersions("", "0.1") < 0);
    }

    printf("== finding the folder\n");
    {
        const auto libs = ParseSteamLibraries("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t\t\"label\"\t\t\"\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n\t\"2\"\n\t{\n\t\t\"path\"\t\t\"E:\\\\broken\n\t}\n}\n");
        Check("Steam's library list is read, backslashes undone, a broken line skipped", libs.size() == 2 && libs[0] == L"C:\\Program Files (x86)\\Steam" && libs[1] == L"D:\\SteamLibrary");
        Check("no libraries in an empty or unrelated file", ParseSteamLibraries("").empty() && ParseSteamLibraries("hello").empty());
        const fs::path ls = MakeLs(L"Lossless Scaling folder");
        Check("a folder with LosslessScaling.exe and a Lossless.dll looks like Lossless Scaling", LooksLikeLosslessScaling(ls.wstring()));
        Check("an empty folder, a missing one and an empty name do not", !LooksLikeLosslessScaling(g_root.wstring()) && !LooksLikeLosslessScaling(L"C:\\no\\such") && !LooksLikeLosslessScaling(L""));
        RememberFolder(ls.wstring());
        bool remembered = false;
        for (const auto& c : FindCandidates()) if (LowerCase(c.dir) == LowerCase(ls.wstring()) && c.how == "used last time") remembered = true;
        Check("the folder used last time is offered", RememberedFolder() == ls.wstring() && remembered);
        for (const auto& c : FindCandidates()) if (!LooksLikeLosslessScaling(c.dir)) { Check("every folder offered looks like Lossless Scaling", false, Narrow(c.dir)); break; }
        Check("every folder offered looks like Lossless Scaling", true);

        // Copies that are not from Steam live anywhere: the usual places on a drive are scanned (one level under the drive's root and under a short list of
        // usual parent folders), and only folders that look like Lossless Scaling count
        MakeLs(L"drive\\Utilities\\Lossless Scaling");
        MakeLs(L"drive\\Games\\Lossless Scaling 3.2.2");
        MakeLs(L"drive\\Lossless Scaling");
        MakeLs(L"drive\\Steam\\steamapps\\common\\Lossless Scaling");
        MakeLs(L"drive\\Program Files (x86)\\LosslessScaling");
        MakeLs(L"drive\\Utilities\\deeper\\Lossless Scaling");
        fs::create_directories(g_root / L"drive" / L"Utilities" / L"Lossless Scaling Notes");
        fs::create_directories(g_root / L"drive" / L"Utilities" / L"Other tool");
        Write(g_root / L"drive" / L"Utilities" / L"Other tool" / L"LosslessScaling.exe", "not it: the folder's name says nothing about Lossless Scaling");
        const auto scanned = ScanDrive((g_root / L"drive").wstring());
        auto has = [&](const wchar_t* rel) { for (const auto& d : scanned) if (LowerCase(d) == LowerCase((g_root / L"drive" / rel).wstring())) return true; return false; };
        Check("a copy under Utilities, one under Games (with a version in its name), one in the root, one in a Steam library and one named LosslessScaling are all found",
              has(L"Utilities\\Lossless Scaling") && has(L"Games\\Lossless Scaling 3.2.2") && has(L"Lossless Scaling") && has(L"Steam\\steamapps\\common\\Lossless Scaling") &&
              has(L"Program Files (x86)\\LosslessScaling"), std::to_string(scanned.size()) + " found");
        Check("a folder with the right name but no LosslessScaling.exe is not offered", !has(L"Utilities\\Lossless Scaling Notes"));
        Check("a folder with the wrong name is not offered, and nothing two levels down is searched", !has(L"Utilities\\Other tool") && !has(L"Utilities\\deeper\\Lossless Scaling") && scanned.size() == 5, std::to_string(scanned.size()));
        Check("a drive that does not exist scans as empty", ScanDrive(L"Q:\\no\\such\\drive").empty() && ScanDrive(L"").empty());
    }

    printf("== reading the state of a folder\n");
    {
        const fs::path plain = MakeLs(L"plain");
        State s = Inspect(plain.wstring());
        Check("a plain Lossless Scaling: NotInstalled, version 3.2.2.0, not running", s.situation == Situation::NotInstalled && s.lsVersion == "3.2.2.0" && !s.running);
        Check("a folder with no LosslessScaling.exe: NotLosslessScaling", Inspect(g_root.wstring()).situation == Situation::NotLosslessScaling);
        const fs::path noOriginal = MakeLs(L"no_original");
        fs::copy_file(g_ours, noOriginal / L"Lossless.dll", fs::copy_options::overwrite_existing);
        Check("ours with no original to forward to: NoOriginal", Inspect(noOriginal.wstring()).situation == Situation::NoOriginal);
        const fs::path both = MakeLs(L"both_ours");
        fs::copy_file(g_ours, both / L"Lossless.dll", fs::copy_options::overwrite_existing);
        fs::copy_file(g_ours, both / L"Lossless_original.dll", fs::copy_options::overwrite_existing);
        Check("both ours: BothOurs", Inspect(both.wstring()).situation == Situation::BothOurs);
        const fs::path other = MakeLs(L"other", L"fake_other.dll");
        Check("a Lossless.dll that is neither: Unrecognised", Inspect(other.wstring()).situation == Situation::Unrecognised);
        const fs::path stale = MakeLs(L"stale");
        fs::copy_file(g_exeDir / L"fake_original_new.dll", stale / L"Lossless.dll", fs::copy_options::overwrite_existing);
        fs::copy_file(g_exeDir / L"fake_original.dll", stale / L"Lossless_original.dll", fs::copy_options::overwrite_existing);
        s = Inspect(stale.wstring());
        Check("Lossless Scaling updated over ours (a new original and a stale one): AfterLsUpdate, new version", s.situation == Situation::AfterLsUpdate && s.lsVersion == "3.2.3.0");
    }

    printf("== what to offer\n");
    {
        State s; s.dir = L"x"; s.situation = Situation::NotInstalled; s.lsVersion = "3.2.2.0";
        Advice a = Advise(s, "0.5.0");
        Check("not installed: Install", a.action == Action::Install && a.actionLabel == "Install" && !a.blocked && !a.canUninstall && Has(a.headline, "not installed"));
        s.situation = Situation::Installed; s.installedVersion = "0.4.1";
        a = Advise(s, "0.5.0");
        Check("installed and older: Update to the new version, with Uninstall offered", a.action == Action::Update && a.actionLabel == "Update to 0.5.0" && a.canUninstall);
        a = Advise(s, "0.4.1");
        Check("installed and the same: Reinstall", a.action == Action::Reinstall && Has(a.headline, "up to date"));
        a = Advise(s, "0.3.0");
        Check("installed and newer than the installer: nothing to do", a.action == Action::None);
        s.situation = Situation::AfterLsUpdate;
        a = Advise(s, "0.4.1");
        Check("after a Lossless Scaling update: Repair, and it says why", a.action == Action::Repair && Has(a.headline, "updated") && a.canUninstall);
        s.situation = Situation::NotInstalled; s.running = true;
        a = Advise(s, "0.4.1");
        Check("running: the action is blocked, with the reason", a.blocked && Has(a.blockedReason, "Close Lossless Scaling"));
        for (Situation broken : { Situation::NoOriginal, Situation::BothOurs, Situation::Unrecognised, Situation::NotLosslessScaling }) {
            s.situation = broken; s.running = false;
            a = Advise(s, "0.4.1");
            if (!a.blocked || a.action != Action::None || a.blockedReason.empty()) { Check("a folder that cannot be mended is blocked with a reason", false); break; }
        }
        Check("a folder that cannot be mended is blocked with a reason", true);
    }

    printf("== installing (a folder name with spaces and accented letters)\n");
    const fs::path pay = MakePayload(L"payload_current", g_ours, "A");
    const std::string ourVersion = ReadVersionResource(g_ours.wstring()).fileVersion;
    {
        Check("a good payload is accepted, with its version", CheckPayload(pay.wstring()).ok && CheckPayload(pay.wstring()).version == ourVersion);
        Check("a payload with no Lossless.dll, or one that is not ours, is refused", !CheckPayload((g_root / L"nothing").wstring()).ok && !CheckPayload(MakePayload(L"payload_wrong", g_exeDir / L"fake_original.dll").wstring()).ok);

        const fs::path ls = MakeLs(L"Lossless Scaling \u00c4\u00d6 test");
        const std::string originalHash = Hash(ls / L"Lossless.dll");
        Result r = Install(ls.wstring(), pay.wstring());
        Check("install into a plain folder succeeds, with a message", r.ok && Has(r.message, "Installed"), r.message);
        Check("Lossless Scaling's file is now Lossless_original.dll, byte for byte", Hash(ls / L"Lossless_original.dll") == originalHash);
        Check("Lossless.dll is ours, byte for byte", Same(ls / L"Lossless.dll", g_ours));
        Check("the icons and every addon file arrived", Read(ls / L"manager-icon.ico") == "icon A" && Same(ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll", pay / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") &&
              Same(ls / L"addons" / L"DLSS5NR01" / L"nr_selftest.exe", pay / L"addons" / L"DLSS5NR01" / L"nr_selftest.exe") && Read(ls / L"addons" / L"DLSS5NR01" / L"addon.json").find("\"A\"") != std::string::npos);
        Check("the person's settings, other addons, removed addons and Lossless Scaling's other files were not touched", UntouchedByInstall(ls));
        Check("the original was also copied to the backups", !r.backupDir.empty() && Hash(fs::path(r.backupDir) / L"Lossless.dll") == originalHash);
        Check("no temporary file is left in the folder", Gone(ls / L"Lossless.dll.setup-new") && Gone(ls / L"addons" / L"DLSS5NR01" / L"addon.json.setup-new"));
        const State s = Inspect(ls.wstring());
        Check("the folder now reads as Installed, at the payload's version, with Lossless Scaling's version kept", s.situation == Situation::Installed && s.installedVersion == ourVersion && s.lsVersion == "3.2.2.0");
        Check("the folder is remembered for next time", RememberedFolder() == ls.wstring());
        Check("the log says what was done", r.log.size() >= 4 && Has(r.log[0] + r.log[1] + r.log[2], "renamed"));

        r = Install(ls.wstring(), pay.wstring());
        Check("installing the same version again changes nothing and is refused as up to date... or accepted as a reinstall", r.ok || Has(r.message, "up to date"), r.message);
        Check("...and the folder is still Installed with the same original", Inspect(ls.wstring()).situation == Situation::Installed && Hash(ls / L"Lossless_original.dll") == originalHash && Same(ls / L"Lossless.dll", g_ours));

        // an update: the files that changed are replaced and the old ones saved
        const fs::path pay2 = MakePayload(L"payload_next", g_ours, "B");
        r = Install(ls.wstring(), pay2.wstring());
        Check("a payload with changed addon files updates them", r.ok && Read(ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") == "addon B" && Read(ls / L"manager-icon.png") == "png B", r.message);
        Check("...saves the old files in the backups", !r.backupDir.empty() && Read(fs::path(r.backupDir) / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") == "addon A");
        Check("...and still leaves the person's settings and the original alone", UntouchedByInstall(ls) && Hash(ls / L"Lossless_original.dll") == originalHash);
    }

    printf("== an update replaces every file of ours, and leaves a vendor runtime the person replaced with their own\n");
    {
        const fs::path ls = MakeLs(L"keep_ls", L"fake_original.dll");
        const fs::path runtime = ls / L"addons" / L"DLSS5NR01" / L"dlss" / L"nvngx_dlss.dll";   // NVIDIA's runtime: the person's to swap for a newer one
        const fs::path helper = ls / L"addons" / L"DLSS5NR01" / L"nvngx.dll_dlss5nr01.dll";      // ours
        Result r = Install(ls.wstring(), MakePayload(L"payload_keep_a", g_ours, "A").wstring());
        Check("a first install works and writes the record of what it placed", r.ok && r.kept.empty() && Exists((ls / L"addons" / L".installed-files.txt").wstring()), r.message);
        Write(runtime, "my own newer runtime");   // the person swapped the vendor's file for one of their own
        Write(helper, "a test build of ours");     // and a test build of one of ours is in the folder too
        r = Install(ls.wstring(), MakePayload(L"payload_keep_b", g_ours, "B").wstring());
        Check("an update replaces the files of ours, a test build of one of them included...", r.ok && Read(helper) == "helper B" && Read(ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") == "addon B" && Read(ls / L"addons" / L"DLSS5NR01" / L"addon.json").find("\"B\"") != std::string::npos, r.message);
        Check("...saves the test build in the backups...", !r.backupDir.empty() && Read(fs::path(r.backupDir) / L"addons" / L"DLSS5NR01" / L"nvngx.dll_dlss5nr01.dll") == "a test build of ours");
        Check("...and leaves the vendor runtime the person replaced as it is, and says so", Read(runtime) == "my own newer runtime" && r.kept.size() == 1 && Has(r.kept[0], "nvngx_dlss.dll") && Has(r.message, "Kept 1 file"), r.message);
        Check("...with the settings and the person's other addons untouched", UntouchedByInstall(ls));
        r = Install(ls.wstring(), MakePayload(L"payload_keep_c", g_ours, "C").wstring());
        Check("the next update keeps it again (it is still not what was put there)", r.ok && Read(runtime) == "my own newer runtime" && r.kept.size() == 1 && Read(helper) == "helper C", r.message);
        Check("a runtime that is put back as it was is replaced again", [&] { Write(runtime, "runtime A"); return Install(ls.wstring(), MakePayload(L"payload_keep_d", g_ours, "D").wstring()).ok && Read(runtime) == "runtime D"; }());
        fs::remove(runtime);
        r = Install(ls.wstring(), MakePayload(L"payload_keep_e", g_ours, "E").wstring());
        Check("a file that was deleted is put back", r.ok && Read(runtime) == "runtime E" && r.kept.empty(), r.message);
    }

    printf("== an install from before the record existed: earlier releases' files are known by their hashes\n");
    {
        const fs::path ls = MakeLs(L"keep_known_ls", L"fake_original.dll");
        const fs::path payA = MakePayload(L"payload_known_a", g_ours, "A");
        Result r = Install(ls.wstring(), payA.wstring());
        fs::remove(ls / L"addons" / L".installed-files.txt");   // as an install of an earlier release left it: no record
        const fs::path runtime = ls / L"addons" / L"DLSS5NR01" / L"dlss" / L"nvngx_dlss.dll";
        Write(runtime, "my own newer runtime");
        // a payload that carries the list of what release A shipped
        const fs::path payB = MakePayload(L"payload_known_b", g_ours, "B");
        {
            std::string lines;
            for (const char* name : { "DLSS5NR01.dll", "addon.json" })
                lines += Hash(payA / L"addons" / L"DLSS5NR01" / Widen(name)) + "\tdlss5nr01\\" + name + "\n";
            lines += Hash(payA / L"addons" / L"DLSS5NR01" / L"dlss" / L"nvngx_dlss.dll") + "\tdlss5nr01\\dlss\\nvngx_dlss.dll\n";
            Write(payB / L"known-hashes.txt", lines);
        }
        r = Install(ls.wstring(), payB.wstring());
        Check("files of ours are replaced, the vendor runtime the person replaced is kept", r.ok && Read(ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") == "addon B" && Read(runtime) == "my own newer runtime" && r.kept.size() == 1, r.message);
        // with no record and no list, a file of ours is replaced all the same; only a vendor runtime that differs stays
        const fs::path ls2 = MakeLs(L"keep_unknown_ls", L"fake_original.dll");
        r = Install(ls2.wstring(), MakePayload(L"payload_unknown_a", g_ours, "A").wstring());
        fs::remove(ls2 / L"addons" / L".installed-files.txt");
        Write(ls2 / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll", "an old test build");
        Write(ls2 / L"addons" / L"DLSS5NR01" / L"dlss" / L"nvngx_dlss.dll", "a runtime from somewhere");
        r = Install(ls2.wstring(), MakePayload(L"payload_unknown_b", g_ours, "B").wstring());
        Check("with neither a record nor a list: ours is replaced (a test build too), a vendor runtime that differs is kept, Lossless.dll and the icons are ours", r.ok && Read(ls2 / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") == "addon B" && Read(ls2 / L"addons" / L"DLSS5NR01" / L"dlss" / L"nvngx_dlss.dll") == "a runtime from somewhere" && r.kept.size() == 1 && Same(ls2 / L"Lossless.dll", g_ours) && Read(ls2 / L"manager-icon.png") == "png B", r.message);
    }

    printf("== an older install (0.1.0, when it was called Echo Addon Manager) is updated\n");
    {
        const fs::path ls = MakeLs(L"old_install");
        const std::string originalHash = Hash(ls / L"Lossless.dll");
        fs::rename(ls / L"Lossless.dll", ls / L"Lossless_original.dll");
        fs::copy_file(g_exeDir / L"fake_ours_old.dll", ls / L"Lossless.dll");
        Write(ls / L"addons" / L"DLSS5NR01" / L"nvngx.dll_lspnr.dll", "the old helper");
        Write(ls / L"LP-icon.ico", "old icon");
        State s = Inspect(ls.wstring());
        Check("an older install reads as Installed at 0.1.0 and the installer offers an update", s.situation == Situation::Installed && s.installedVersion == "0.1.0" && Advise(s, ourVersion).action == Action::Update);
        const Result r = Install(ls.wstring(), pay.wstring());
        Check("updating replaces our old Lossless.dll and keeps the original", r.ok && Same(ls / L"Lossless.dll", g_ours) && Hash(ls / L"Lossless_original.dll") == originalHash, r.message);
        Check("...saves the old one, and moves the old helper DLL aside instead of leaving it", !r.backupDir.empty() && Hash(fs::path(r.backupDir) / L"Lossless.dll.ours") == Hash(g_exeDir / L"fake_ours_old.dll") &&
              Gone(ls / L"addons" / L"DLSS5NR01" / L"nvngx.dll_lspnr.dll") && Read(fs::path(r.backupDir) / L"nvngx.dll_lspnr.dll") == "the old helper");
        Check("...and moves the icon under its old name (LP-icon) aside too", Gone(ls / L"LP-icon.ico") && Read(fs::path(r.backupDir) / L"LP-icon.ico") == "old icon" && fs::exists(ls / L"manager-icon.ico"));
    }

    printf("== an install from before the version resource existed (0.4.1 and earlier)\n");
    {
        // Such a Lossless.dll has no version resource but carries the name of its own log file (as UTF-16 in a wide string literal, as plain text in others)
        const auto legacyBytes = [](bool wide) {
            std::string filler(3000, 'x');
            std::string marker = "EchoAddonManager.log", stored;
            for (const char c : marker) { stored += c; if (wide) stored += '\0'; }
            return filler + stored + filler;
        };
        const fs::path ls = MakeLs(L"legacy_install");
        const std::string originalHash = Hash(ls / L"Lossless.dll");
        fs::rename(ls / L"Lossless.dll", ls / L"Lossless_original.dll");
        Write(ls / L"Lossless.dll", legacyBytes(true));
        DllInfo d = InspectDll((ls / L"Lossless.dll").wstring());
        Check("a DLL with no version resource but its log file name inside is an earlier Echo Addon Manager", d.kind == DllKind::Ours && d.legacy && d.version.empty());
        Write(g_root / L"legacy_narrow.dll", legacyBytes(false));
        Write(g_root / L"proxy_upstream.dll", "....LosslessProxy.log....");
        Check("...whether the name is stored as plain text or UTF-16, and the proxy this project started from counts too", InspectDll((g_root / L"legacy_narrow.dll").wstring()).legacy && InspectDll((g_root / L"proxy_upstream.dll").wstring()).legacy);
        Check("a DLL with neither is not taken for ours", InspectDll((g_exeDir / L"fake_other.dll").wstring()).kind == DllKind::Unknown && !InspectDll((g_exeDir / L"fake_original.dll").wstring()).legacy);
        {   // the name straddling the edge of a 1 MB read must still be found
            std::string big(1024 * 1024 - 5, 'y');
            big += "EchoAddonManager.log";
            big += std::string(2 * 1024 * 1024, 'z');
            Write(g_root / L"straddle.bin", big);
            Write(g_root / L"absent.bin", std::string(3 * 1024 * 1024, 'q'));
            Check("a name that falls across two reads is found, and one that is absent is not", FileContainsText((g_root / L"straddle.bin").wstring(), "EchoAddonManager.log") && !FileContainsText((g_root / L"absent.bin").wstring(), "EchoAddonManager.log"));
        }
        const State s = Inspect(ls.wstring());
        const Advice a = Advise(s, ourVersion);
        Check("the folder reads as Installed and the installer offers an update, not a refusal", s.situation == Situation::Installed && s.installedVersion.empty() && a.action == Action::Update && Has(a.headline, "earlier"), a.headline);
        Check("with nothing known about the payload it says an earlier version is installed rather than 'up to date'", Advise(s, "").action == Action::Reinstall && Has(Advise(s, "").headline, "earlier") && !Has(Advise(s, "").headline, "up to date"));
        const Result r = Install(ls.wstring(), pay.wstring());
        Check("updating works and keeps Lossless Scaling's own file", r.ok && Same(ls / L"Lossless.dll", g_ours) && Hash(ls / L"Lossless_original.dll") == originalHash, r.message);
        Check("...and the old one is saved in the backups", Read(fs::path(r.backupDir) / L"Lossless.dll.ours") == legacyBytes(true));
    }

    printf("== repairing after a Lossless Scaling update\n");
    {
        const fs::path ls = MakeLs(L"repair");
        Check("first, an install", Install(ls.wstring(), pay.wstring()).ok);
        // Lossless Scaling updates itself: its new Lossless.dll replaces ours; the old Lossless_original.dll stays behind
        fs::copy_file(g_exeDir / L"fake_original_new.dll", ls / L"Lossless.dll", fs::copy_options::overwrite_existing);
        const std::string newOriginal = Hash(ls / L"Lossless.dll"), staleOriginal = Hash(ls / L"Lossless_original.dll");
        State s = Inspect(ls.wstring());
        Check("that state is recognised and Repair is offered", s.situation == Situation::AfterLsUpdate && Advise(s, ourVersion).action == Action::Repair);
        const Result r = Install(ls.wstring(), pay.wstring());
        Check("repair succeeds", r.ok && Has(r.message, "Repaired"), r.message);
        Check("...the NEW Lossless Scaling file is now Lossless_original.dll, and ours is in front again", Hash(ls / L"Lossless_original.dll") == newOriginal && Same(ls / L"Lossless.dll", g_ours));
        Check("...the stale original went to the backups, not away", Hash(fs::path(r.backupDir) / L"Lossless_original.dll.old") == staleOriginal);
        Check("...and the folder is Installed with the new Lossless Scaling version", Inspect(ls.wstring()).situation == Situation::Installed && Inspect(ls.wstring()).lsVersion == "3.2.3.0" && UntouchedByInstall(ls));
    }

    printf("== refusing, and rolling back\n");
    {
        // Lossless Scaling running (a copy of cmd.exe named LosslessScaling.exe, started from the folder)
        const fs::path ls = MakeLs(L"running");
        STARTUPINFOW si = {}; si.cb = sizeof si;
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + (ls / L"LosslessScaling.exe").wstring() + L"\" /c ping -n 40 127.0.0.1 >nul";
        const bool started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
        Sleep(400);
        Check("a stand-in Lossless Scaling could be started", started);
        Check("a running Lossless Scaling in the folder is seen", Inspect(ls.wstring()).running && LosslessScalingRunning(ls.wstring()));
        Check("...and not one running from another folder", !LosslessScalingRunning((g_root / L"elsewhere").wstring()));
        const std::string before = Hash(ls / L"Lossless.dll");
        const Result r = Install(ls.wstring(), pay.wstring());
        Check("install is refused while it runs, saying to close it", !r.ok && Has(r.message, "Close Lossless Scaling"), r.message);
        Check("...and the folder is exactly as it was", Hash(ls / L"Lossless.dll") == before && Gone(ls / L"Lossless_original.dll") && Gone(ls / L"addons" / L"DLSS5NR01") && Gone(ls / L"backups"));
        Check("uninstall is refused while it runs too", !Uninstall(ls.wstring(), false).ok);
        if (started) { TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        Sleep(200);
        Check("once it has ended, the folder is free to install into", !Inspect(ls.wstring()).running);
    }
    {
        const fs::path noOriginal = MakeLs(L"refuse_no_original");
        fs::copy_file(g_ours, noOriginal / L"Lossless.dll", fs::copy_options::overwrite_existing);
        const Result r = Install(noOriginal.wstring(), pay.wstring());
        Check("a folder with ours but no original is refused, with the way out", !r.ok && Has(r.message, "Lossless Scaling") && Same(noOriginal / L"Lossless.dll", g_ours) && Gone(noOriginal / L"Lossless_original.dll"), r.message);
        const Result n = Install(g_root.wstring(), pay.wstring());
        Check("a folder that is not Lossless Scaling is refused", !n.ok && Has(n.message, "not found"), n.message);
        Check("an unrecognised Lossless.dll is refused and left alone", !Install(MakeLs(L"refuse_other", L"fake_other.dll").wstring(), pay.wstring()).ok);
        Check("a bad payload is refused", !Install(MakeLs(L"refuse_payload").wstring(), (g_root / L"nothing").wstring()).ok);
    }
    {
        // a file that cannot be replaced part-way through: Lossless.dll has already been swapped by then, and everything must go back
        const fs::path ls = MakeLs(L"rollback");
        Write(ls / L"addons" / L"DLSS5NR01" / L"addon.json", "{ \"mine\": true }");
        const std::string originalHash = Hash(ls / L"Lossless.dll");
        HANDLE lock = CreateFileW((ls / L"addons" / L"DLSS5NR01" / L"addon.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);   // nobody else may open it
        Check("a file was locked for the test", lock != INVALID_HANDLE_VALUE);
        const Result r = Install(ls.wstring(), pay.wstring());
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        Check("the install fails when a file cannot be replaced, and says nothing was changed", !r.ok && r.rolledBack && Has(r.message, "Nothing was changed"), r.message);
        Check("...Lossless.dll is Lossless Scaling's own again, byte for byte, and no Lossless_original.dll is left", Hash(ls / L"Lossless.dll") == originalHash && Gone(ls / L"Lossless_original.dll"));
        Check("...nothing that was added is left behind, and the locked file kept its content", Gone(ls / L"manager-icon.ico") && Gone(ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll") && Read(ls / L"addons" / L"DLSS5NR01" / L"addon.json") == "{ \"mine\": true }");
        Check("...the person's settings are intact and the folder reads as a plain Lossless Scaling", UntouchedByInstall(ls) && Inspect(ls.wstring()).situation == Situation::NotInstalled);
        Check("...and trying again once the file is free works", Install(ls.wstring(), pay.wstring()).ok);
    }

    printf("== uninstalling\n");
    {
        const fs::path ls = MakeLs(L"uninstall");
        const std::string originalHash = Hash(ls / L"Lossless.dll");
        Install(ls.wstring(), pay.wstring());
        Result r = Uninstall(ls.wstring(), false);
        Check("uninstall puts Lossless Scaling's own Lossless.dll back, byte for byte", r.ok && Hash(ls / L"Lossless.dll") == originalHash && Gone(ls / L"Lossless_original.dll"), r.message);
        Check("...ours is in the backups, and the addons and settings are still there", Same(fs::path(r.backupDir) / L"Lossless.dll.ours", g_ours) && Exists((ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll").wstring()) && UntouchedByInstall(ls));
        Check("...and the folder reads as a plain Lossless Scaling again", Inspect(ls.wstring()).situation == Situation::NotInstalled);
        r = Uninstall(ls.wstring(), false);
        Check("uninstalling again says there is nothing to do", r.ok && Has(r.message, "not installed"));

        const fs::path ls2 = MakeLs(L"uninstall_all");
        Install(ls2.wstring(), pay.wstring());
        r = Uninstall(ls2.wstring(), true);
        Check("uninstall with the addons removed moves the whole addons folder to the backups", r.ok && Gone(ls2 / L"addons") && Read(fs::path(r.backupDir) / L"addons" / L"config.json").find("mine") != std::string::npos);

        const fs::path ls3 = MakeLs(L"uninstall_stale");
        Install(ls3.wstring(), pay.wstring());
        fs::copy_file(g_exeDir / L"fake_original_new.dll", ls3 / L"Lossless.dll", fs::copy_options::overwrite_existing);
        const std::string newHash = Hash(ls3 / L"Lossless.dll");
        r = Uninstall(ls3.wstring(), false);
        Check("uninstalling after a Lossless Scaling update keeps its new file and moves the stale copy aside", r.ok && Hash(ls3 / L"Lossless.dll") == newHash && Gone(ls3 / L"Lossless_original.dll") && Exists((fs::path(r.backupDir) / L"Lossless_original.dll.old").wstring()));
        Check("uninstalling a folder with ours but no original is refused", !Uninstall([&] { const fs::path p = MakeLs(L"uninstall_broken"); fs::copy_file(g_ours, p / L"Lossless.dll", fs::copy_options::overwrite_existing); return p.wstring(); }(), false).ok);
    }

    printf("== awkward paths, read-only files, backups made in the same second\n");
    {
        // Lossless Scaling is running, but the person's folder is written another way than the process's own path
        const fs::path ls = MakeLs(L"awkward running folder");
        STARTUPINFOW si = {}; si.cb = sizeof si;
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + (ls / L"LosslessScaling.exe").wstring() + L"\" /c ping -n 40 127.0.0.1 >nul";
        const bool started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
        Sleep(400);
        Check("a stand-in Lossless Scaling could be started", started);
        const std::wstring plain = ls.wstring();
        wchar_t shortName[MAX_PATH] = {};
        const DWORD shortLen = GetShortPathNameW(plain.c_str(), shortName, MAX_PATH);
        std::wstring fwd = plain, upper = plain;
        for (wchar_t& c : fwd) if (c == L'\\') c = L'/';
        for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
        const std::wstring dotdot = (ls.parent_path() / L"nowhere" / L".." / ls.filename()).wstring();
        Check("running: seen when the folder is given with a trailing backslash", LosslessScalingRunning(plain + L"\\"));
        Check("running: seen with forward slashes", LosslessScalingRunning(fwd));
        Check("running: seen in capitals", LosslessScalingRunning(upper));
        Check("running: seen with a .. in the path", LosslessScalingRunning(dotdot));
        Check("running: seen by the folder's short (8.3) name", shortLen > 0 && shortLen < MAX_PATH && LosslessScalingRunning(shortName), Narrow(shortName));
        Check("running: still not seen for another folder", !LosslessScalingRunning((g_root / L"elsewhere").wstring()) && !LosslessScalingRunning(L""));
        const std::string before = Hash(ls / L"Lossless.dll");
        Check("install through the short name is refused while it runs, and changes nothing", shortLen > 0 && !Install(shortName, pay.wstring()).ok && Hash(ls / L"Lossless.dll") == before && Gone(ls / L"backups"));
        if (started) { TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        Sleep(200);
    }
    {
        // the folder written with a trailing backslash or forward slashes installs like any other
        const fs::path ls = MakeLs(L"slashes");
        std::wstring fwd = ls.wstring() + L"/";
        for (wchar_t& c : fwd) if (c == L'\\') c = L'/';
        const Result r = Install(fwd, pay.wstring());
        Check("a folder given with forward slashes and a trailing slash installs", r.ok && Inspect(ls.wstring()).situation == Situation::Installed && UntouchedByInstall(ls), r.message);
        Check("...and uninstalls through a trailing backslash", Uninstall(ls.wstring() + L"\\", false).ok && Inspect(ls.wstring()).situation == Situation::NotInstalled);
    }
    {
        // read-only files: a Lossless.dll copied off read-only media, and installed files someone marked read-only
        const fs::path ls = MakeLs(L"read only files");
        SetFileAttributesW((ls / L"Lossless.dll").c_str(), FILE_ATTRIBUTE_READONLY);
        Result r = Install(ls.wstring(), pay.wstring());
        Check("a read-only Lossless.dll is no obstacle to installing", r.ok && Inspect(ls.wstring()).situation == Situation::Installed, r.message);
        const fs::path files[] = {ls / L"Lossless.dll", ls / L"manager-icon.ico", ls / L"addons" / L"DLSS5NR01" / L"DLSS5NR01.dll", ls / L"addons" / L"DLSS5NR01" / L"addon.json"};
        for (const auto& f : files) SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_READONLY);
        const fs::path payB = MakePayload(L"payload_readonly_b", g_ours, "B");
        r = Install(ls.wstring(), payB.wstring());
        Check("read-only installed files are replaced by an update", r.ok && Has(Read(ls / L"addons" / L"DLSS5NR01" / L"addon.json"), "\"B\"") && Read(ls / L"manager-icon.ico") == "icon B", r.message);
        Check("...and the folder is still Installed, with the person's things untouched", Inspect(ls.wstring()).situation == Situation::Installed && UntouchedByInstall(ls));
        std::error_code ec;
        for (fs::recursive_directory_iterator it(ls, ec), end; !ec && it != end; it.increment(ec)) SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    {
        // backups made in the same second must not overwrite each other (the folder name has a time stamp: fixed here to force it)
        SetBackupStampForTest(L"same-second");
        const fs::path ls = MakeLs(L"same second");
        Result first = Install(ls.wstring(), pay.wstring());   // A: the plain folder becomes installed
        std::vector<Result> updates;
        for (const char* tag : {"B", "C", "D"}) updates.push_back(Install(ls.wstring(), MakePayload(L"payload_ss_" + Widen(tag), g_ours, tag).wstring()));
        SetBackupStampForTest(L"");
        bool allOk = first.ok;
        std::vector<std::wstring> dirs;
        for (const auto& u : updates) { allOk = allOk && u.ok; dirs.push_back(u.backupDir); }
        Check("four installs in the same second all work", allOk);
        bool distinct = !dirs.empty();
        for (size_t i = 0; i < dirs.size(); ++i) for (size_t j = i + 1; j < dirs.size(); ++j) if (dirs[i] == dirs[j] || dirs[i].empty()) distinct = false;
        Check("each one keeps its own backups folder", distinct);
        Check("the first update's backup still holds what it replaced (A), the second's holds B", updates.size() == 3 &&
              Has(Read(fs::path(updates[0].backupDir) / L"addons" / L"DLSS5NR01" / L"addon.json"), "\"A\"") && Has(Read(fs::path(updates[1].backupDir) / L"addons" / L"DLSS5NR01" / L"addon.json"), "\"B\""));
    }

    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\LSAddonManager_SetupTest");
    std::error_code ec;
    fs::remove_all(g_root, ec);
    printf("\n%s (%d failed)\n", g_failed ? "SETUP TEST FAILED" : "SETUP TEST PASSED", g_failed);
    return g_failed ? 1 : 0;
}
