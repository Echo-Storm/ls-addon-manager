// IHost: what the manager offers an addon. The addon gets it in AddonInitialize and may keep it until AddonShutdown returns. Every call may be
// made from any thread. The order of the calls is part of the addon API: new ones are only ever added at the end, and each says which API
// version added it, so an addon checks GetHostVersion() before using a call newer than its min_host_version.
#pragma once
#include "events.h"
#include <cstdint>

enum EamLogLevel : uint8_t {
    EAM_LOG_TRACE = 0,
    EAM_LOG_DEBUG = 1,
    EAM_LOG_INFO  = 2,
    EAM_LOG_WARN  = 3,
    EAM_LOG_ERROR = 4
};

// Called around each of Lossless Scaling's compute passes (ID3D11DeviceContext::Dispatch on the immediate context of one of its devices), on
// its render thread, with the pass's thread group counts. A pre-dispatch callback returns true to have the pass skipped (and then no
// post-dispatch callback runs). Dispatches an addon makes itself from inside a callback do not call back. Keep them short: Lossless Scaling
// waits for them. The host holds its hook list locked while it calls them: SetPreDispatchCallback / SetPostDispatchCallback called from inside
// a dispatch callback are refused (and logged), because they would wait for that lock; set and clear callbacks from AddonInitialize,
// AddonShutdown or a thread of your own.
typedef bool (*EamPreDispatchCallback)(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ, void* userData);
typedef void (*EamPostDispatchCallback)(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ, void* userData);

struct IHost {
    virtual ~IHost() = default;

    // A line in the manager's log, marked as coming from an addon.
    virtual void Log(EamLogLevel level, const char* message) = 0;

    // The addon's settings: text values under addons.<addonId> in config.json. GetConfig's result stays valid for hundreds of later calls,
    // not for ever: copy it if it is kept. SaveConfig writes the file (only if something changed).
    virtual const char* GetConfig(const char* addonId, const char* key, const char* defaultVal = "") = 0;
    virtual void SetConfig(const char* addonId, const char* key, const char* value) = 0;
    virtual void SaveConfig() = 0;

    // The addon API version the manager provides: (major << 16) | (minor << 8) | patch.
    virtual uint32_t GetHostVersion() = 0;

    // Events (events.h). A callback is called on the publishing thread; unsubscribing removes every subscription of that callback to the event.
    virtual void SubscribeEvent(uint32_t eventId, EamEventCallback callback, void* userData = nullptr) = 0;
    virtual void UnsubscribeEvent(uint32_t eventId, EamEventCallback callback) = 0;
    virtual void PublishEvent(uint32_t eventId, const void* data = nullptr, uint32_t dataSize = 0) = 0;

    // Lossless Scaling's newest D3D11 device and its immediate context (ID3D11Device*, ID3D11DeviceContext*; borrowed, do not Release);
    // null before it makes one. It makes new ones as it starts and stops scaling: take them in EAM_EVENT_D3D11_DEVICE_READY rather than
    // keeping old pointers. An addon that uses them declares EAM_CAP_D3D11_DEVICE_ACCESS.
    virtual void* GetD3D11Device() = 0;
    virtual void* GetD3D11DeviceContext() = 0;

    // The dispatch callbacks (see above); an addon that sets them declares EAM_CAP_DISPATCH_HOOK. Each addon has one pre- and one
    // post-dispatch callback per userData value: setting another replaces it, setting null removes it. They are removed by themselves when
    // the addon is unloaded.
    virtual void SetPreDispatchCallback(EamPreDispatchCallback callback, void* userData = nullptr) = 0;
    virtual void SetPostDispatchCallback(EamPostDispatchCallback callback, void* userData = nullptr) = 0;

    // Inside a dispatch callback: the ID3D11ComputeShader bound for the pass (borrowed, do not Release). Null outside one.
    virtual void* GetCurrentComputeShader() = 0;

    // Lossless Scaling's compute passes since its first device was made (for diagnostics).
    virtual uint32_t GetDispatchCount() = 0;

    // ---- API 1.0 (GetHostVersion() >= 0x010000)

    // A one-line live status, shown on the addon's card and in the status bar ("Running, model 6.6 ms"). level: 0 info (grey), 1 ok
    // (green), 2 warning (amber), 3 error (red). Refresh it at most a few times a second; one not refreshed for a few seconds is hidden as
    // stale. An empty text clears it.
    virtual void SetStatus(const char* addonId, const char* text, int level) = 0;

    // A sample for the Performance tab: one series per (addonId, key), with short snake_case keys ("frame_ms") and a free-text unit ("ms",
    // "%") for the labels. The last few thousand samples of each series are kept. At most once a frame.
    virtual void PublishMetric(const char* addonId, const char* key, double value, const char* unit) = 0;

    // ---- API 1.1 (GetHostVersion() >= 0x010100)

    // Inside a dispatch callback: the ID3D11DeviceContext the pass runs on (borrowed, do not Release). Lossless Scaling makes more than one
    // device, and this tells them apart. Null outside a callback.
    virtual void* GetDispatchingContext() = 0;

    // ---- API 1.2 (GetHostVersion() >= 0x010200)

    // An RGBA picture, 8 bits a channel, rows top to bottom `pitch` bytes apart, made into an image to draw with ImGui::Image (the result is its
    // ImTextureID). Null when the manager's window has no device yet, or the picture is larger than 4096 pixels a side. The pixels are copied at
    // once. Give it back with ReleaseImage when it is no longer drawn. Any thread.
    virtual void* CreateImage(const void* rgba, uint32_t width, uint32_t height, uint32_t pitch) = 0;
    virtual void ReleaseImage(void* image) = 0;

    // ---- API 1.3 (GetHostVersion() >= 0x010300)

    // The manager's recorder (the Features tab, "Recording"): the last few seconds of Lossless Scaling's frames, saved as a .lsrec file with a hotkey. The
    // manager finds the frames Lossless Scaling captured by itself. An addon offers the frames only it has: the picture presented before its own is
    // drawn on it, what an upscaler took in, its own made frames.
    // RecorderFlags: bit 0 (1) the recorder is on; bit 1 (2) it records what is shown, so the addon should offer the presented frames (and an addon that
    // has none of them offers nothing). Offer nothing while bit 0 is clear: it costs the addon's work for nothing. Any thread.
    virtual uint32_t RecorderFlags() = 0;
    // A frame for the recorder, from the render thread that owns d3d11Context (ID3D11DeviceContext*, ID3D11Texture2D*; borrowed, copied on the card
    // before the call returns, never kept). source: 2 the presented frame, 3 an upscaler's input (1, the captured frame, is the manager's own and ignored).
    // content: 0 the frame's own encoding, 1 an SDR view, 2 linear light. tag: 0 none, 1 a frame made between real ones, 2 a real frame. cropWidth x
    // cropHeight (0: all): only the middle of the frame is kept, at full size.
    virtual void RecorderOffer(void* d3d11Context, void* d3d11Texture2D, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropWidth, uint32_t cropHeight) = 0;
    // Saves what the recorder holds now as a file named after `name` (UTF-8; the date is added), and with clear takes the frames out of the recorder (the
    // upscalers' comparison saves each mode's frames by themselves). Returns 1: saving has begun; 0: there was nothing to save (or the recorder is off);
    // 2: a save is still running, try again in a moment.
    virtual int RecorderSave(const char* name, bool clear) = 0;

    // The manager's GPU limit (the Features tab, "GPU limit"). GpuLimitPercent: 0 while it is off, else 80..100, the load the graphics card should stay under. GpuLoadPercent: the load of
    // the card Lossless Scaling runs on, in percent, read by the manager about once a second (the call itself is cheap; it keeps the reading going while it is asked for); 0 when it is not
    // known. An addon that can make its work cheaper (a lower working size, a lighter pass) does so while the load stays at or over the limit and goes back when it is well under.
    virtual uint32_t GpuLimitPercent() = 0;
    virtual uint32_t GpuLoadPercent() = 0;

    // ---- API 1.4 (GetHostVersion() >= 0x010400)

    // Lossless Scaling's frame rate counter ("60 / 120", config.ini [fps_counter]) counts the frames Lossless Scaling presents, so an addon that makes frames of its own
    // (EchoFG) is not in it. Tell the manager how many frames the screen gets for each one Lossless Scaling presents (1: as it counts; 2: twice as many) and the counter's
    // second number shows the rate the screen gets. Returns true when the counter is hooked; false when it is not (a Lossless Scaling build the manager does not know): the
    // addon then shows a readout of its own. Call it with 1 when the addon stops making frames. Any thread; cheap.
    virtual bool SetFpsCounterMultiplier(float multiplier) = 0;

    // What Lossless Scaling was last told by its window (its ApplySettings, which the manager sees): LosslessFrameGenType is its frame generation, 0 off, else the kind (1 LSFG 3, 2 LSFG 2,
    // 3 LSFG 1, 4 and 5 the older interpolators); LosslessDrawsFps whether its frame rate counter is on. Both are 0 before the first ApplySettings. An addon that makes frames itself
    // (EchoFG) can warn when Lossless Scaling's own frame generation is on too.
    virtual int LosslessFrameGenType() = 0;
    virtual bool LosslessDrawsFps() = 0;
};
