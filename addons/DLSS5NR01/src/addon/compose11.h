// Compose11: puts the model's delta onto a frame Lossless Scaling is about to present, on its own D3D11 device: one compute pass per presented
// frame, real or generated.
//
//   presented = frame + delta of real frame d (enlarged to the frame's size, taken from where this pixel's content was in frame d)
//
// The presented frame lies `offset` real frames after d: 0 is d itself, fractions are the generated frames after it, 1 is the next real frame
// while its own delta is not ready yet, more than 1 means the model is behind. The delta is moved with LSFG's flow: forward along the
// previous -> current field (zw) when it is older than the frame, back along current -> previous (xy) when it is newer. The same pass also
// does the look settings (sharpening, tone, colour, grain), the protected HUD areas, the comparison views and the debug views.
#pragma once
#include <d3d11.h>
#include <cstdint>
#include <functional>

class Compose11 {
public:
    using LogFn = std::function<void(const char*)>;
    struct Args {
        ID3D11Texture2D* target = nullptr;             // the swap chain buffer about to be presented (read and written)
        ID3D11ShaderResourceView* delta = nullptr;     // the delta at the working size, RGBA16F
        ID3D11Resource* flow = nullptr; uint32_t flowW = 0, flowH = 0; float flowUnit = 2.0f;   // LSFG's flow (RGBA16F), or null
        ID3D11ShaderResourceView* motion = nullptr;    // or the model's motion for frame d (RG16F, working-size pixels, d -> the frame before):
                                                       // the result is moved along it, `offset` frames on (frame generation off)
        float offset = 0;                              // the presented frame minus d, in real frames
        float intensity = 1, maxDelta = 0.5f, hiProtect = 0.85f; uint32_t debugView = 0; bool isGen = false;
        float ghostGuard = 0;                          // 0 = off: fades the delta where the two motion fields disagree, and with its age
        float sharpen = 0;                             // 0 = off; contrast-adaptive, on the composed frame
        float saturation = 1, vibrance = 0;            // colour: 1 and 0 = unchanged
        float brightness = 0, contrast = 1, gamma = 1; // tone: 0, 1, 1 = unchanged
        float shadows = 0, highlights = 0;             // tonal ranges: 0 = unchanged
        float grain = 0, grainSize = 1; uint32_t grainSeed = 0;   // film grain (0 = off), its cell size in pixels, a new seed at each present
        uint32_t hudCount = 0; float hud[6][4] = {}; float hudFeather = 0.004f; bool hudShow = false;   // areas left as they are
        uint32_t compare = 0;                          // 0 enhanced, 1 split (the left side original), 2 original only
        float splitPos = 0.5f;                         // where the split is, 0..1 of the width
        uint32_t marker = 0;                           // a corner square as feedback: 1 green, 2 red, 3 amber, 4 blue, 5 purple
        uint32_t encoding = 0; float whiteNits = 200;  // what the target holds (0 SDR, 1 scRGB, 2 HDR10, hdr.h) and the SDR white in nits
        float viewport[4] = { 0, 0, 1, 1 };            // where the frame is drawn in the target (x, y, w, h in uv): Lossless Scaling's bars (a 16:9 frame on an ultrawide screen) are left alone
    };
    bool Init(ID3D11Device* dev, LogFn log);
    void Shutdown();
    bool IsReady() const { return m_shader != nullptr; }
    bool Run(ID3D11DeviceContext* ctx, const Args& a);
    double CpuMs() const { return m_cpuMs; }            // CPU time a Run takes on Lossless Scaling's render thread, smoothed
    const char* TargetInfo() const { return m_targetInfo; }
    uint64_t Runs() const { return m_runs; }

private:
    bool FitCopy(const D3D11_TEXTURE2D_DESC& target, DXGI_FORMAT view);
    ID3D11UnorderedAccessView* TargetView(ID3D11Texture2D* target, DXGI_FORMAT view);   // a new view of the buffer (released by Run), or m_out
    ID3D11ShaderResourceView* FlowView(ID3D11Resource* flow);                            // cached per flow texture
    void DropCopy();
    void Log(const char* fmt, ...);

    LogFn m_log;
    ID3D11Device* m_dev = nullptr;
    ID3D11ComputeShader* m_shader = nullptr; ID3D11Buffer* m_constants = nullptr; ID3D11SamplerState* m_sampler = nullptr;
    // The pass reads a copy of the buffer (it samples neighbours, so it cannot read and write the buffer itself). A buffer that cannot be
    // written by a compute pass gets the result in m_out, which is then copied over it.
    ID3D11Texture2D* m_copy = nullptr; ID3D11ShaderResourceView* m_copyView = nullptr;
    ID3D11Texture2D* m_out = nullptr; ID3D11UnorderedAccessView* m_outView = nullptr;
    uint32_t m_w = 0, m_h = 0; DXGI_FORMAT m_fmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Resource* m_flow = nullptr; ID3D11ShaderResourceView* m_flowView = nullptr;
    double m_cpuMs = 0; uint64_t m_runs = 0; char m_targetInfo[96] = "none yet"; uint64_t m_lastFailure = 0;
};
