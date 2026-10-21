#pragma once
#include "../../sdk/include/eam/ihost.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace eam {

// The host as an addon sees it: settings, events, logging, Direct3D 11 access, dispatch callbacks, and live status and metrics. Every
// call may come from any thread. Most of it hands straight to the registry that owns the data (settings, events, metrics).
class HostImpl : public IHost {
public:
    HostImpl() = default;
    ~HostImpl() override = default;

    void Log(EamLogLevel level, const char* message) override;

    const char* GetConfig(const char* addonId, const char* key, const char* defaultVal) override;
    void SetConfig(const char* addonId, const char* key, const char* value) override;
    void SaveConfig() override;
    uint32_t GetHostVersion() override;

    void SubscribeEvent(uint32_t eventId, EamEventCallback callback, void* userData) override;
    void UnsubscribeEvent(uint32_t eventId, EamEventCallback callback) override;
    void PublishEvent(uint32_t eventId, const void* data, uint32_t dataSize) override;

    void* GetD3D11Device() override;
    void* GetD3D11DeviceContext() override;
    void SetD3D11Device(void* device, void* context);   // the device capture hook hands these over

    // One callback per owner (the userData): setting one again replaces it, and passing no callback removes it.
    void SetPreDispatchCallback(EamPreDispatchCallback callback, void* userData) override;
    void SetPostDispatchCallback(EamPostDispatchCallback callback, void* userData) override;
    void* GetCurrentComputeShader() override;
    uint32_t GetDispatchCount() override;
    void* GetDispatchingContext() override;
    void* CreateImage(const void* rgba, uint32_t width, uint32_t height, uint32_t pitch) override;
    void ReleaseImage(void* image) override;
    uint32_t RecorderFlags() override;
    int RecorderSave(const char* name, bool clear) override;
    uint32_t GpuLimitPercent() override;
    uint32_t GpuLoadPercent() override;
    bool SetFpsCounterMultiplier(float multiplier) override;
    int LosslessFrameGenType() override;
    bool LosslessDrawsFps() override;
    static void SetLosslessState(int frameGenType, bool drawsFps);   // from main.cpp's ApplySettings
    static void SetGpuLimit(uint32_t percent);   // the GPU limit feature: 0 off, else 80..100
    void RecorderOffer(void* d3d11Context, void* d3d11Texture2D, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropWidth, uint32_t cropHeight) override;
    // The recorder feature (features/recorder_feature.cpp) says what these two do; without it (or when it is off) the flags are 0 and an offer is dropped.
    using RecorderFlagsFn = uint32_t (*)();
    using RecorderOfferFn = void (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using RecorderSaveFn = int (*)(const char*, bool);
    static void SetRecorderHooks(RecorderFlagsFn flags, RecorderOfferFn offer, RecorderSaveFn save);

    void SetStatus(const char* addonId, const char* text, int level) override;
    void PublishMetric(const char* addonId, const char* key, double value, const char* unit) override;

    // Called from the Dispatch hook. InvokePreDispatch runs every callback and says whether any of them wants the dispatch skipped.
    // A callback that faults is removed (and logged) instead of taking Lossless Scaling's render thread down.
    bool InvokePreDispatch(uint32_t x, uint32_t y, uint32_t z);
    void InvokePostDispatch(uint32_t x, uint32_t y, uint32_t z);

    // Removes every dispatch callback whose code lies in [begin, end): an addon's DLL that is being unloaded. Returns how many.
    size_t ForgetCode(uintptr_t begin, uintptr_t end);
    size_t DispatchHookCount();   // pre and post together (for tests)

private:
    template <class Callback>
    struct Hook { Callback callback; void* owner; void* module; };   // an owner is the addon (its module) together with its userData

    // GetConfig returns a pointer into one of these. The newest kReturnBufferCount stay alive, so a pointer stays valid for that many
    // later calls; nothing is freed on a timer, because another thread may still be reading an older one.
    static constexpr size_t kReturnBufferCount = 512;
    mutable std::mutex m_bufferMutex;
    mutable std::deque<std::string> m_returnBuffers;

    void* m_d3d11Device = nullptr;    // borrowed, not owned
    void* m_d3d11Context = nullptr;

    std::mutex m_dispatchMutex;
    std::vector<Hook<EamPreDispatchCallback>> m_preHooks;
    std::vector<Hook<EamPostDispatchCallback>> m_postHooks;
    // How many of each there are, read without the lock: every one of Lossless Scaling's dispatches asks, and usually there are none.
    std::atomic<size_t> m_preCount{ 0 }, m_postCount{ 0 };
    void Recount() { m_preCount.store(m_preHooks.size(), std::memory_order_relaxed); m_postCount.store(m_postHooks.size(), std::memory_order_relaxed); }
};

} // namespace eam
