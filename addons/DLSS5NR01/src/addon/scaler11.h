// DLSS Super Resolution as Lossless Scaling's scaler (the DLSS Upscaler): the Lossless Scaling side.
//
// Lossless Scaling scales every frame it presents, real and generated, with one compute pass. With NIS chosen that pass is easy to know: the
// frame at the game's size in (t0), NIS's two 2x64 RGBA32F coefficient tables (t1, t2), the picture at the screen's size out (u0), and one
// thread group per 32x24 pixels of it. The addon skips that pass and puts DLSS's picture in its output instead.
//
// DLSS itself runs on a D3D12 device of our own (SrEngine, engine/sr_engine.h): running NVIDIA's D3D11 DLSS on Lossless Scaling's device
// crashed it. On Lossless Scaling's context this side only copies the frame (and frame generation's flow) into shared textures, signals a
// shared fence, and copies the newest finished picture into the pass's output: usually the frame before's. Lossless Scaling's queue never
// waits on the engine's, and the CPU never waits either (Handoff::Late). Up to two frames may be with the engine at once (two frame buffers,
// three pictures in turn), so a frame that finishes late on a busy GPU delays the picture a little instead of being skipped.
#pragma once
#include <cmath>
#include <d3d11_4.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>

class SrEngine;

namespace nr {

// The NIS pass, recognised from what is bound when it is dispatched; false for any other pass.
// in/out and their sizes are the viewports NIS scales between: the whole textures, or, for a window of another shape than the screen (a 4:3
// game on a 16:9 screen), the part of each NIS reads and writes (inX/inY, outX/outY their top-left corners), taken from NIS's constants.
struct NisPass {
    ID3D11Resource* in = nullptr; ID3D11Resource* out = nullptr; uint32_t inW = 0, inH = 0, outW = 0, outH = 0; DXGI_FORMAT inFmt = DXGI_FORMAT_UNKNOWN, outFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t inX = 0, inY = 0, outX = 0, outY = 0;
    bool Partial() const { return inX || inY || outX || outY; }
};
// AddRef's in/out: ReleaseNisPass. log: where a pass that looks like NIS covers only part of its output, what its constants say (once a shape).
bool FindNisPass(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, NisPass& pass, const std::function<void(const char*)>& log = nullptr);
void ReleaseNisPass(NisPass& pass);
// True when the last NIS pass covered part of its output in a layout that could not be followed (NIS then stays, and the panel says so).
bool NisLayoutRefused();

class ScalerLink {
public:
    using LogFn = std::function<void(const char*)>;
    // On Lossless Scaling's render thread: the shared fences between its device and the engine's.
    bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, SrEngine* engine, LogFn log);
    void Shutdown();   // Unblock, drain, release
    // The engine's queue waits on the GPU for "copied" before each frame. Should a frame's signal never have reached the GPU (a device lost
    // or replaced mid-frame), that wait would hold the queue, and every drain after it, for good: signal it from the CPU up to the newest frame.
    void Unblock();
    // Before letting go of a device Lossless Scaling has replaced: why it went (hung, reset, removed, or not at all), and how far the two
    // fences had got, so the log says whether a wait was left unanswered.
    void ReportDeviceChange();
    bool IsReady() const { return m_copied.d3d11 != nullptr; }
    // How the picture comes back. Late: the newest finished one (the frame before's), nothing waits. Wait: this frame's, Lossless Scaling's
    // queue waits on the GPU for it (the first design; with frame generation off it left Lossless Scaling restarting, 2026-09-24).
    // Observe: DLSS runs but NIS's picture stays, to tell whether running DLSS at all is what upsets Lossless Scaling.
    // AtPresent: NIS runs as usual, and DLSS's newest picture is copied over its output just before Present (PresentCopy).
    enum class Handoff { Late = 0, Wait = 1, Observe = 2, AtPresent = 3 };
    // AtPresent, from the Present hook on Lossless Scaling's presenting thread: the picture chosen at the NIS pass goes into the swap chain's
    // back buffer. Nothing happens for another swap chain (another device or size) or when no picture is waiting.
    void PresentCopy(IDXGISwapChain* sc);
    ID3D11Device* Device() const { return m_dev; }

    // At the NIS pass, on its context: the frame goes to the engine, DLSS's picture comes back into the pass's output. False when DLSS did not
    // run for this frame (the NIS pass should then run as usual). flow: frame generation's newest flow (RGBA16F) or null.
    // estimate: the engine measures the motion from the frames (flow unused).
    // gpuWait: when the engine has not finished a newer picture than the one shown last, Lossless Scaling's queue waits on the GPU for the
    // next one rather than show the same picture again (the CPU never waits).
    // The picture controls on the frame before it is upscaled (all at their defaults: the pass only copies).
    struct Picture {
        float brightness = 0, contrast = 1, gamma = 1, shadows = 0, highlights = 0, saturation = 1, vibrance = 0;
        bool Neutral() const {
            return std::abs(brightness) < 0.0005f && std::abs(contrast - 1.0f) < 0.002f && std::abs(gamma - 1.0f) < 0.002f && std::abs(shadows) < 0.005f &&
                   std::abs(highlights) < 0.005f && std::abs(saturation - 1.0f) < 0.002f && vibrance < 0.002f;
        }
    };
    void SetPicture(const Picture& p) { m_picture = p; }
    // The frame the last Upscale handed to the engine, as the grab pass wrote it (for the recorder: NIS's own input may be a texture a plain
    // copy reads as black, see the grab pass), or null when none was handed over. Taken once.
    ID3D11Texture2D* TakeGrabbed() { ID3D11Texture2D* t = m_grabbed; m_grabbed = nullptr; return t; }
    bool Upscale(const NisPass& pass, ID3D11Resource* flow, uint32_t flowW, uint32_t flowH, float flowUnit, float motionFraction, bool estimate, unsigned preset,
                 float sharpen, bool reset, Handoff handoff = Handoff::Late, bool gpuWait = true);

    // Counted since the link was made: passes that showed a picture, frames not handed over (the engine had kIn already), pictures shown a
    // second time, GPU waits for the next picture; and of passes, repeats and waits, those that came within kClosePassMs of the pass before
    // (two frames close together, as adaptive frame generation makes them).
    struct Counters {
        static constexpr double kClosePassMs = 6.0;
        uint64_t passes = 0, skipped = 0, repeats = 0, waits = 0, closePasses = 0, closeRepeats = 0, closeWaits = 0;
    };
    Counters Count() const { return m_count; }
    bool LastRefusedFormat() const { return m_refusedFormat; }   // the last pass was a frame format the upscaler cannot take (HDR)

private:
    Picture m_picture;
    ID3D11Texture2D* m_grabbed = nullptr;   // not held: one of m_in, valid until the link is shut down
    struct Shared { ID3D11Texture2D* d3d11 = nullptr; ID3D12Resource* d3d12 = nullptr; uint32_t w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; void Release(); };
    struct Fence { ID3D11Fence* d3d11 = nullptr; ID3D12Fence* d3d12 = nullptr; void Release(); };
    bool Fit(Shared& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool engineWrites, const char* name);
    bool MakeFence(Fence& f, const char* name);
    void Log(const char* fmt, ...);
    void DescribeTargets(const NisPass& pass);
    bool MakeGrabShader();
    void Probe(uint64_t shown, bool inFresh);
    void PlacePicture(const NisPass& pass, ID3D11Texture2D* picture);

    LogFn m_log;
    SrEngine* m_engine = nullptr;
    ID3D11Device5* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    ID3D11DeviceContext4* m_ctx4 = nullptr;
    // The frame is read into m_in by a small compute pass through the NIS pass's own t0 view, not copied: with frame generation off that
    // frame is a keyed-mutex texture shared from Lossless Scaling's capture device, and CopyResource from it gave all black (2026-09-24).
    ID3D11ComputeShader* m_grab = nullptr;
    ID3D11Buffer* m_grabOrigin = nullptr;   // the grab's constants: the input viewport's corner
    static const int kIn = 2, kOut = 3;   // frames with the engine at most; pictures in turn (the one shown is never one being written)
    ID3D11UnorderedAccessView* m_inUav[kIn] = {};
    Shared m_in[kIn], m_out[kOut], m_flow[kIn];   // frame n reads m_in[n % kIn] (and m_flow[n % kIn]) and writes m_out[n % kOut]
    Fence m_copied, m_done;           // "done" reaches n when the engine has finished frame n (its queue does them in order)
    uint64_t m_frame = 0;             // the newest frame handed to the engine
    uint64_t m_holds[kOut] = {};      // the frame whose finished picture each m_out holds (0: none)
    Counters m_count;
    uint64_t m_lastShown = 0;         // the frame whose picture the last pass showed
    int64_t m_lastPassQpc = 0;
    Handoff m_handoff = Handoff::Late;
    bool m_pendingReset = false;      // a history reset asked for while the engine was busy: it goes with the next frame handed over
    uint64_t m_atPresent = 0;         // AtPresent: the frame whose picture PresentCopy puts in the back buffer (0: none)
    uint64_t m_copiedAtPresent = 0;
    // once per link: how bright the frame DLSS gets and the picture it makes are (a black picture shows here)
    ID3D11Texture2D* m_probe[2] = {};
    int m_probeState = 0;
    bool m_loggedFormat = false, m_described = false, m_loggedPartial = false, m_refusedFormat = false;
};

} // namespace nr
