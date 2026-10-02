// eam_runtimetest: the Runtimes list's facts about a file (runtime_files.h), with no window. A DLL loaded under another spelling of its path
// (the mixed slashes NVIDIA's loader uses) must count as loaded; the version, signature and SHA-256 must be read; a file not loaded must not
// count. The DLL is AMD's signed FidelityFX runtime the FSR Upscaler ships (argument 1).
#include "src/addon/runtime_files.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

using namespace eam;

static int g_failures = 0;
static void Check(bool ok, const char* what, const std::string& detail = "") {
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_failures;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: eam_runtimetest <folder holding fsr\\amd_fidelityfx_dx12.dll>\n"); return 2; }
    std::wstring dir(argv[1], argv[1] + strlen(argv[1]));
    for (wchar_t& ch : dir) if (ch == L'/') ch = L'\\';
    AddonInfo addon; addon.id = "FSR3UPSC"; addon.manifest.name = "FSR Upscaler"; addon.enabled = true;
    addon.dllPath = dir + L"\\FSR3UPSC.dll";
    AddonManifest::Runtime slot; slot.name = "FSR"; slot.file = "fsr/amd_fidelityfx_dx12.dll";
    slot.shippedSha256 = "12a5081257ec95b0b53ad51b4a87fb3c03f97fe0bbb59f9496968f8d50ef93a6"; slot.shippedLabel = "3.1.4";
    addon.manifest.runtimes.push_back(slot);
    const std::vector<AddonInfo> addons = { addon };

    auto rows = [&] {   // the facts are read on a thread of their own: wait for them
        std::vector<RuntimeFile> r;
        for (int i = 0; i < 100; ++i) { r = RuntimeFiles(addons, dir); if (!r.empty() && r[0].read) break; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        return r;
    };
    std::vector<RuntimeFile> before = rows();
    Check(before.size() == 1 && before[0].exists && before[0].read, "the file is found and read");
    if (before.empty()) return 1;
    Check(before[0].signature == RuntimeFile::Signature::Signed && before[0].signer.find("Advanced Micro Devices") != std::string::npos, "signed by AMD", before[0].signer);
    Check(before[0].shipped && before[0].ShownVersion() == "3.1.4", "the shipped file, shown as 3.1.4", before[0].ShownVersion());
    Check(!before[0].loaded, "not loaded yet: a cross");

    // loaded the way NVIDIA's loader spells paths: the folder with backslashes, then a forward slash
    const std::wstring spelled = dir + L"\\fsr/amd_fidelityfx_dx12.dll";
    HMODULE module = LoadLibraryExW(spelled.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Check(module != nullptr, "loaded under a mixed-slash path");
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));   // the module list is read at most every two seconds
    std::vector<RuntimeFile> after = rows();
    Check(!after.empty() && after[0].loaded, "now a tick: the same file, however its path was written");
    if (module) FreeLibrary(module);

    {   // a DLL a person picked by hand that is damaged: the export directory's address lies outside every section (and there are none). It must be refused, not read from a wild pointer.
        std::string pe(0x200, '\0');
        IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x40;
        IMAGE_NT_HEADERS64 nt{}; nt.Signature = IMAGE_NT_SIGNATURE; nt.FileHeader.Characteristics = IMAGE_FILE_DLL; nt.FileHeader.NumberOfSections = 0;
        nt.FileHeader.SizeOfOptionalHeader = sizeof nt.OptionalHeader; nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress = 0x5000; nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size = 0x100;
        memcpy(&pe[0], &dos, sizeof dos); memcpy(&pe[0x40], &nt, sizeof nt);
        const std::wstring bad = dir + L"\\damaged_test.dll";
        if (FILE* f = _wfopen(bad.c_str(), L"wb")) { fwrite(pe.data(), 1, pe.size(), f); fclose(f); }
        std::vector<std::string> names;
        const bool read = ReadDllExports(bad, names);
        Check(!read, "a DLL whose export directory points outside its sections is refused", read ? "was accepted" : "");
        DeleteFileW(bad.c_str());
    }

    printf(g_failures ? "\nRUNTIME TEST FAILED (%d)\n" : "\nRUNTIME TEST PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
