// Lossless.dll, as Lossless Scaling loads it. Every export but ApplySettings is forwarded to Lossless_original.dll by the linker
// (proxy_exports.h). Loading it starts the manager: the log, the settings and addons, the hooks, the built-in features and the window.
#include "proxy_exports.h"
#include "shader_hook.h"
#include "d3d11_hook.h"
#include "fps_counter_hook.h"
#include "instance_guard.h"
#include "../addon/addon_manager.h"
#include "../config/config_manager.h"
#include "../event/event_system.h"
#include "../features/features.h"
#include "../features/recorder_feature.h"
#include "../features/gpu_limit.h"
#include "../host/gpu_stats.h"
#include "../host/host_impl.h"
#include "../log/logger.h"
#include "../gui/gui_manager.h"
#include "../../sdk/include/eam/version.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <windows.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------------------------------
// ApplySettings: Lossless Scaling calls it with the settings of the profile being applied. Ours logs them, calls the real one, then
// tells the addons (EAM_EVENT_SETTINGS_APPLIED), which may adjust things now that Lossless Scaling has.
// ---------------------------------------------------------------------------------------------------------------------------------

extern "C" __declspec(dllexport) void __fastcall ApplySettings(
    int scalingMode, int scalingFitMode, int scalingType, int scalingSubtype, float scaleFactor, uint8_t resizeBeforeScale, int sharpness,
    uint8_t vrs, int frameGenType, int frameGenSize, int frameGenMode, float frameGenMultiplier, float frameGenTarget, int frameGenFlowScale,
    uint8_t clipCursor, uint8_t adjustCursorSpeed, uint8_t hideCursor, uint8_t scaleCursor, int syncMode, int maxFrameLatency, uint8_t gsyncSupport,
    uint8_t hdrSupport, int captureApi, int queueTarget, uint8_t drawFps, int gpuId, int displayId, int cropLeft, int cropTop, int cropRight,
    int cropBottom, uint8_t multiDisplayMode);

namespace {

decltype(&ApplySettings) g_realApplySettings = nullptr;

// What the manager runs, from DllMain's attach to its detach.
struct Manager {
    std::unique_ptr<eam::HostImpl> host;
    std::unique_ptr<eam::AddonManager> addons;
    bool passive = false;   // another Lossless Scaling from this folder already runs a manager: this copy only forwards

    bool Start();
    void Stop();
};
std::unique_ptr<Manager> g_manager;

// Before any window exists (the manager's own is created on its thread), or Windows scales it as a bitmap instead of at the real DPI.
void BecomeDpiAware() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setContext = user32 ? reinterpret_cast<SetContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")) : nullptr;
    if (setContext) setContext(reinterpret_cast<HANDLE>(-4));   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Windows 10 1703 and later)
    else SetProcessDPIAware();
}

fs::path ProgramFolder() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

// Runs inside DllMain (the loader lock is held): nothing here may wait for another thread or use D3D. What needs to is started on threads.
bool Manager::Start() {
    BecomeDpiAware();
    const fs::path folder = ProgramFolder();
    std::error_code ec;
    fs::create_directories(folder / "logs", ec);   // the logs sit in <Lossless Scaling>\logs, not beside the program
    eam::Logger::Instance().Init((folder / "logs" / EAM_PRODUCT_FILE ".log").wstring());
    LOG_INFO("Core", "%s %s (built %s) starting", EAM_PRODUCT_NAME, EAM_VERSION_STRING, __DATE__);
    eam::InstallCrashLogging();

    const HMODULE lossless = LoadLibraryW(L"Lossless_original.dll");
    if (!lossless) { LOG_ERROR("Core", "Lossless_original.dll could not be loaded: Lossless Scaling cannot run"); return false; }
    g_realApplySettings = reinterpret_cast<decltype(&ApplySettings)>(GetProcAddress(lossless, "ApplySettings"));
    if (!g_realApplySettings) LOG_ERROR("Core", "Lossless_original.dll has no ApplySettings: settings will not reach Lossless Scaling");

    // Lossless Scaling runs one copy of itself: started again, it loads this DLL, hands over to the running copy and exits. That second copy
    // must not start another window, tray icon, hotkey or set of addons, or write the settings and logs the first one uses.
    if (!eam::instance::Claim(folder.wstring())) {
        passive = true;
        LOG_INFO("Core", "A manager already runs for this Lossless Scaling folder: this copy only forwards to Lossless Scaling");
        return true;
    }

    host = std::make_unique<eam::HostImpl>();
    addons = std::make_unique<eam::AddonManager>(host.get());   // loads the settings
    eam::Logger::Instance().SetMinLevel(static_cast<eam::LogLevel>(std::clamp(eam::ConfigManager::Instance().GlobalGetOr<int>(nullptr, "log_level", 2), 0, 4)));
    try {
        addons->ScanAddons();
    } catch (const std::exception& e) {   // an unreadable addons folder must not take Lossless Scaling down with it
        LOG_ERROR("Core", "The addons folder could not be read: %s", e.what());
    }

    eam::AddonManager* const addonList = addons.get();
    ShaderHook::InstallHooks(lossless, [addonList](const wchar_t* name, const wchar_t* type, const void** data, uint32_t* size) {
        return addonList->InterceptResource(name, type, data, size);
    });
    eam::fpscounter::Install(lossless);   // false for a Lossless Scaling build it does not know: its counter is then left alone
    D3D11Hook::Initialize(host.get());
    {   // the recorder feature's way into the hook, the host and the event bus (the tests of the features link without them)
        eam::features::recorder::Backends backends;
        backends.setDispatchObserver = &D3D11Hook::SetDispatchObserver;
        backends.setHostHooks = &eam::HostImpl::SetRecorderHooks;
        backends.publishSaved = [](const wchar_t* path) { EamRecordingSavedData data{ path }; eam::EventBus::Instance().Publish(EAM_EVENT_RECORDING_SAVED, &data, sizeof data); };
        eam::features::recorder::SetBackends(backends);
        eam::features::gpulimit::Backends gpu;
        gpu.setLimit = &eam::HostImpl::SetGpuLimit;
        gpu.readLoad = [] { eam::GpuStats::Instance().Wanted(); const auto s = eam::GpuStats::Instance().Get(); return s.ok && s.ageSeconds < 5.0 ? s.utilGpu : 0u; };
        eam::features::gpulimit::SetBackends(gpu);
    }
    eam::features::Start();
    // The window thread hooks Dispatch first (that needs D3D), then loads the addons, then opens the window.
    eam::GuiManager::StartGuiThread(addons.get(), [] { D3D11Hook::InstallDispatchHooks(); });
    return true;
}

void Manager::Stop() {
    if (passive) { eam::Logger::Instance().Shutdown(); return; }
    LOG_INFO("Core", "Shutting down");
    eam::EventBus::Instance().Publish(EAM_EVENT_HOST_SHUTDOWN);
    eam::features::Stop();
    D3D11Hook::Shutdown();
    eam::fpscounter::Uninstall();
    ShaderHook::UninstallHooks();
    addons.reset();   // stops and unloads the addons
    host.reset();
    eam::instance::Release();
    eam::Logger::Instance().Shutdown();
}

} // namespace

extern "C" __declspec(dllexport) void __fastcall ApplySettings(
    int scalingMode, int scalingFitMode, int scalingType, int scalingSubtype, float scaleFactor, uint8_t resizeBeforeScale, int sharpness,
    uint8_t vrs, int frameGenType, int frameGenSize, int frameGenMode, float frameGenMultiplier, float frameGenTarget, int frameGenFlowScale,
    uint8_t clipCursor, uint8_t adjustCursorSpeed, uint8_t hideCursor, uint8_t scaleCursor, int syncMode, int maxFrameLatency, uint8_t gsyncSupport,
    uint8_t hdrSupport, int captureApi, int queueTarget, uint8_t drawFps, int gpuId, int displayId, int cropLeft, int cropTop, int cropRight,
    int cropBottom, uint8_t multiDisplayMode) {
    // In the log, so a comparison of scalers or frame generation settings can be lined up with the addon logs (Lossless Scaling's own numbers).
    LOG_INFO("Core", "ApplySettings: scalingMode=%d fit=%d type=%d subtype=%d factor=%.2f sharpness=%d | frameGen type=%d size=%d mode=%d mult=%.2f target=%.1f flowScale=%d | capture=%d sync=%d maxLatency=%d queue=%d",
             scalingMode, scalingFitMode, scalingType, scalingSubtype, scaleFactor, sharpness, frameGenType, frameGenSize, frameGenMode, frameGenMultiplier,
             frameGenTarget, frameGenFlowScale, captureApi, syncMode, maxFrameLatency, queueTarget);
    if (g_realApplySettings)
        g_realApplySettings(scalingMode, scalingFitMode, scalingType, scalingSubtype, scaleFactor, resizeBeforeScale, sharpness, vrs, frameGenType,
                            frameGenSize, frameGenMode, frameGenMultiplier, frameGenTarget, frameGenFlowScale, clipCursor, adjustCursorSpeed, hideCursor,
                            scaleCursor, syncMode, maxFrameLatency, gsyncSupport, hdrSupport, captureApi, queueTarget, drawFps, gpuId, displayId,
                            cropLeft, cropTop, cropRight, cropBottom, multiDisplayMode);
    eam::HostImpl::SetLosslessState(frameGenType, drawFps != 0);
    eam::EventBus::Instance().Publish(EAM_EVENT_SETTINGS_APPLIED);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_manager = std::make_unique<Manager>();
        if (!g_manager->Start()) { g_manager.reset(); return FALSE; }
    } else if (reason == DLL_PROCESS_DETACH && g_manager) {
        g_manager->Stop();
        g_manager.reset();
    }
    return TRUE;
}
