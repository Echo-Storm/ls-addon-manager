// DLSS5NR01: DLSS 5 Neural Rendering, an LS Addon Manager addon. It runs NVIDIA's DLSS 5 neural model on the frames Lossless Scaling captures
// and adds the result to every frame it presents, on the graphics card Lossless Scaling uses, without ever making Lossless Scaling wait.
//
// The parts: runtime.cpp (the frame path), panel.cpp (the settings panel), tasks.cpp (the jobs beside the window), settings.cpp (the
// settings and looks), log.cpp, and this file (what they share, and the exports the manager calls).
#include "addon/state.h"
#include "addon/log.h"
#include "addon/present_hook.h"
#include "addon/hud_editor.h"
#include "addon/screenshot.h"
#include "imgui.h"
#include <eam/widgets.h>
#include <windows.h>

namespace nr {

IHost* g_host = nullptr;
std::wstring g_lsDir, g_addonDir;
std::mutex g_settingsMutex;
Config g_config;
std::vector<Look> g_looks;
std::mutex g_frameMutex;
Recorder& g_recorder = *new Recorder;
NrEngine g_engine;
Bridge g_bridge;
FrameTap g_tap;
Compose11 g_compose;
std::atomic<bool> g_off{ false }, g_offByWatchdog{ false };
std::atomic<uint64_t> g_backOnAtMs{ 0 };
std::atomic<int> g_backOnCount{ 0 };
std::atomic<bool> g_engineStarting{ false }, g_resetRequested{ false };
std::atomic<bool> g_engineCardKnown{ false };
LUID g_engineCard{};
LUID g_frameCard{}; std::atomic<bool> g_frameCardKnown{ false };
std::atomic<int> g_compare{ 0 };
std::atomic<float> g_splitPos{ 0.5f };
std::atomic<bool> g_showHud{ false };
std::mutex g_autoMutex;
AutoQuality g_auto;
std::mutex g_textMutex;
std::vector<nr::diag::Finding> g_findings;
std::string g_scalerGame;
std::string g_encodingText = "SDR";
std::string g_status = "waiting for device", g_offReason, g_frameText, g_cardName, g_tappedDeviceText = "none yet", g_focusExe;
bool g_cardDrivesDisplay = false;
uint64_t g_runs = 0, g_otherPasses = 0, g_lsPresents = 0, g_composed = 0, g_lastDelta = 0;
double g_lastModelMs = 0, g_avgModelMs = 0, g_lastRunMs = 0, g_lastOffset = 0;

void SetStatus(const std::string& text) { std::lock_guard<std::mutex> lock(g_textMutex); g_status = text; }
std::string Status() { std::lock_guard<std::mutex> lock(g_textMutex); return g_status; }

void SwitchOff(const std::string& why) {
    g_off = true;
    { std::lock_guard<std::mutex> lock(g_textMutex); g_offReason = why; }
    Log("DISABLED: %s", why.c_str());
}
void SwitchOn() {
    g_off = false; g_offByWatchdog = false; g_backOnCount = 0;
    ResetWatchdog();
}

void ApplyTapRoles() {
    std::string tickText, tapText; int mode, slot; bool fresh;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); tickText = g_config.tickSig; tapText = g_config.tapSig; mode = g_config.tapMode; slot = g_config.frameSlot; fresh = g_config.freshFlow; }
    DispatchSig tick, tap;
    tick.Parse(tickText); tap.Parse(tapText);
    g_tap.SetRoles(tick, tap, mode == 1 ? FrameTap::Manual : FrameTap::Auto, slot);
    g_tap.SetFreshFlow(fresh);
}

void Commit(const Config& config, bool tapRolesChanged, bool modelSizeChanged) {
    std::vector<Look> looks; Config saved;
    { std::lock_guard<std::mutex> lock(g_settingsMutex);
      g_config = config;
      if (kScalerAddon && g_config.scalerPerGame) KeepForGame(g_config, g_scalerGame);   // a change made for the game in play is its own
      saved = g_config; looks = g_looks; }
    SaveSettings(g_host, kAddonId, saved, looks);
    if (tapRolesChanged) ApplyTapRoles();
    if (modelSizeChanged) g_resetRequested = true;   // the engine makes the feature again at the next frame
}

} // namespace nr

using namespace nr;

namespace {

std::wstring FolderOf(HMODULE module) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    return path;
}

void Start(IHost* host, ImGuiContext* ctx, void* allocFunc, void* freeFunc, void* userData) {
    ImGui::SetCurrentContext(ctx);
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFunc), reinterpret_cast<ImGuiMemFreeFunc>(freeFunc), userData);
    eam::ui::InitAddonImGui();
    g_host = host;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Start), &self);
    g_lsDir = FolderOf(nullptr);
    g_addonDir = FolderOf(self);
    OpenLog(g_lsDir, host);
    InstallCrashReports();
    // A second start of the same DLL (the manager unloaded it, but its Present hook pinned it in memory) begins from a clean slate.
    SwitchOn();
    ForgetFramePath();

    Loaded loaded = LoadSettings(host, kAddonId);
    { std::lock_guard<std::mutex> lock(g_settingsMutex); g_config = loaded.config; g_looks = std::move(loaded.looks);
      g_config.model = kScalerAddon ? 1 : 0; }   // each addon of the pair runs its own model
    SettleFramesAtStart();
    g_compare = loaded.compareStart; g_splitPos = loaded.splitStart;
    ApplyTapRoles();
    PublishRuntimeForOthers();
    if (!kScalerAddon) ScanRequirements();   // Neural Rendering's model file, helper and self-test; DLAA's runtime ships with it
    // a switch for the offline test host: run the compatibility test without a click
    if (std::string(host->GetConfig(kAddonId, "selfTestOnStart", "0")) == "1") RunSelfTest();
    if (std::string(host->GetConfig(kAddonId, "snapshotOnStart", "0")) == "1") screenshot::RequestSnapshot();   // likewise: a snapshot for the HUD editor
    host->SubscribeEvent(EAM_EVENT_D3D11_DEVICE_READY, OnDeviceEvent, nullptr);
    host->SubscribeEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, OnDeviceEvent, nullptr);
    Log("%s initialised (host version 0x%x), addon dir %ls", kAddonId, host->GetHostVersion(), g_addonDir.c_str());
    // Lossless Scaling's passes come from the manager's dispatch callback; the present hook needs a device to find DXGI's table, so it is put
    // in place at the first tapped frame. No device is taken from the host here: the newest one it has seen may be gone already (Lossless
    // Scaling makes and drops devices as it starts and stops scaling); devices are only touched in the events or from a live pass.
    if (host->GetHostVersion() >= 0x010100) {
        host->SetPreDispatchCallback(OnPass, nullptr);
        if (kScalerAddon) host->SetPostDispatchCallback(OnPostPass, nullptr);   // NIS's half of the before / after pair
    }
    else SwitchOff("needs LS Addon Manager with addon API 1.1 or newer (for its dispatch callback)");
}

int OnStartFault(EXCEPTION_POINTERS* e) {
    void* const at = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : nullptr;
    const unsigned code = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
    HMODULE module = nullptr; wchar_t path[MAX_PATH] = L"?";
    if (at && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(at), &module)) GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    char text[256];
    snprintf(text, sizeof text, "exception 0x%08x in AddonInitialize at %ls+0x%llx", code, name ? name + 1 : path,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(module)));
    SwitchOff(text);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

EAM_EXPORT void AddonInitialize(IHost* host, ImGuiContext* ctx, void* allocFunc, void* freeFunc, void* userData) {
    __try { Start(host, ctx, allocFunc, freeFunc, userData); }
    __except (OnStartFault(GetExceptionInformation())) {}
}

EAM_EXPORT void AddonShutdown() {
    Log("shutting down");
    ExportFrameTrace("at shutdown");
    ClearRuntimeTrial();
    g_off = true;
    // First let go of Lossless Scaling: no pass or device event reaches the addon while it tears down. (The upscaler's teardown used to come
    // first, holding the frame lock; if it stalled, Lossless Scaling's next device event waited on that lock for ever: a hang, 2026-09-24.)
    // The manager waits for a pass already in the callback before the call returns.
    if (g_host) {
        g_host->SetPreDispatchCallback(nullptr, nullptr);
        if (kScalerAddon) g_host->SetPostDispatchCallback(nullptr, nullptr);
        g_host->UnsubscribeEvent(EAM_EVENT_D3D11_DEVICE_READY, OnDeviceEvent);
        g_host->UnsubscribeEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, OnDeviceEvent);
    }
    ReleaseFrames();
    if (kScalerAddon) StopScaler();
    PresentHook::Uninstall();
    DropHudSnapshot();
    for (int i = 0; i < 3000 && g_engineStarting; ++i) Sleep(10);   // a model that is loading is let finish
    for (int i = 0; i < 300 && Scanning(); ++i) Sleep(10);          // a requirements scan takes milliseconds (an open file dialog is left: the process is ending)
    { std::lock_guard<std::mutex> lock(g_frameMutex); g_recorder.Forget(); g_bridge.Shutdown(); g_compose.Shutdown(); g_engine.Shutdown(); }
    g_recorder.Shutdown();
    Config config; std::vector<Look> looks;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); config = g_config; looks = g_looks; }
    SaveSettings(g_host, kAddonId, config, looks);
    CloseLog();
    g_host = nullptr;
}

EAM_EXPORT void AddonRenderSettings() { DrawPanel(); }
EAM_EXPORT uint32_t GetAddonCapabilities() { return EAM_CAP_HAS_SETTINGS | EAM_CAP_D3D11_DEVICE_ACCESS | EAM_CAP_DISPATCH_HOOK; }
EAM_EXPORT const char* GetAddonName() { return kProductName; }
EAM_EXPORT const char* GetAddonVersion() { return "0.9.12"; }
EAM_EXPORT const char* GetAddonAuthor() { return "Echo-Storm"; }
EAM_EXPORT const char* GetAddonDescription() {
    return kXessScaler
        ? "Upscales with Intel XeSS Super Resolution in place of Lossless Scaling's NIS scaler, on any graphics card with Shader Model 6.4, with the motion measured from the frames. Intel's XeSS runtime is included."
        : kFsrScaler
        ? "Upscales with AMD FidelityFX Super Resolution in place of Lossless Scaling's NIS scaler, on any graphics card, with the motion measured from the frames: AMD's FSR 3.1, or FSR 4 (OptiScaler's build) from the Runtimes list. AMD's FSR runtime is included."
        : kScalerAddon
        ? "Upscales with NVIDIA DLSS Super Resolution in place of Lossless Scaling's NIS scaler, on every frame it presents, real and generated. NVIDIA's DLSS runtime is included."
        : "Runs NVIDIA DLSS 5 Neural Rendering on Lossless Scaling's real frames on the display GPU and applies the result to every presented frame, without ever making LS wait. Needs your own copy of nvngx_dlssnr.dll (not included, never downloaded).";
}
