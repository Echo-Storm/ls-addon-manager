#include "addon/scaler11.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

// Recognising Lossless Scaling's NIS pass and reading NIS's viewports from its constants (moved out of scaler11.cpp so that an addon without the Direct3D 12 engine, the Video Super
// Resolution prototype, can use it too).
namespace nr {

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool Texture2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& desc) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&desc);
    return true;
}


// ---- NIS's viewports
//
// NVIDIA's NIS scales between two viewports, which its constant buffer (NISConfig, b0) holds, and dispatches one group per 32x24 pixels of
// the output viewport. For a window of the screen's shape both are the whole textures; for another shape Lossless Scaling scales the window
// into part of the screen. The constants are read once per pass shape without waiting (copied now, mapped on a later pass), and only a
// layout that agrees with everything else (the dispatch, the textures, NIS's own scale factor) is used.
struct NisConfigView {   // NISConfig (NVIDIA Image Scaling SDK 1.0), the part read here: 18 floats, then the viewports
    float f[18]; uint32_t inX, inY, inW, inH, outX, outY, outW, outH;
};
static_assert(sizeof(NisConfigView) == 104, "NISConfig's viewports start at byte 72");

// How much smaller than its input an output viewport may be and still count as 1:1 (issue #13: a 3440x1441 window drawn into 3438x1440 of a 3440x1440 screen, a shrink of 0.06 %): half a percent, at least 4 pixels.
uint32_t Slack(uint32_t size) { return std::max<uint32_t>(4u, size / 200u); }

struct ViewportReader {
    struct Key { uint32_t inW, inH, outW, outH, x, y; bool operator==(const Key& k) const { return !memcmp(this, &k, sizeof k); } };
    ID3D11Device* dev = nullptr;
    ID3D11Buffer* staging = nullptr;
    Key key{}; int state = 0;         // 0 nothing, 1 a copy in flight, 2 known good, 3 known unusable
    NisConfigView cfg{};
    uint32_t tries = 0;
    uint32_t cbBytes = 0;   // the size of the constant buffer the pass had bound: another pass with the same bindings has constants of another size (not the buffer's address: Lossless Scaling makes a new one at every frame for the 48-byte pass, issue #13)
    void Reset() { if (staging) staging->Release(); staging = nullptr; dev = nullptr; state = 0; tries = 0; cbBytes = 0; }
};
// One reader for each pass that looks like NIS: by device, shape and constant buffer. A single one was disturbed by a second NIS-looking pass in the same frame (issue #13, a
// 3440x1440 screen: one pass with 48 bytes of constants that is refused, one that is taken): each one's turn reset what the other had found, so the constants were read and
// logged again every frame and the upscaler took the pass only now and then.
std::vector<ViewportReader> g_viewports;
int g_lastViewportState = 0;   // the state of the reader the last pass used

// True with the viewports filled in, once known; false meanwhile (NIS runs as usual) and for a layout that does not fit.
bool ResolveViewports(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& in, const D3D11_TEXTURE2D_DESC& o, uint32_t x, uint32_t y, NisPass& pass,
                      const std::function<void(const char*)>& log) {
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (dev) dev->Release();   // only compared
    const ViewportReader::Key key{ in.Width, in.Height, o.Width, o.Height, x, y };
    ID3D11Buffer* bound = nullptr; ctx->CSGetConstantBuffers(0, 1, &bound);
    D3D11_BUFFER_DESC boundDesc{}; if (bound) bound->GetDesc(&boundDesc);
    const uint32_t boundBytes = boundDesc.ByteWidth;
    if (bound) bound->Release();   // only compared
    ViewportReader* found = nullptr;
    for (ViewportReader& e : g_viewports) if (e.dev == dev && e.key == key && e.cbBytes == boundBytes) { found = &e; break; }
    if (!found) {
        if (g_viewports.size() >= 8) {   // the oldest goes, one that was refused or is still being read before one that is known good
            size_t victim = 0; for (size_t i = 0; i < g_viewports.size(); ++i) if (g_viewports[i].state != 2) { victim = i; break; }
            g_viewports[victim].Reset(); g_viewports.erase(g_viewports.begin() + victim);
        }
        g_viewports.push_back(ViewportReader{}); found = &g_viewports.back();
        found->dev = dev; found->key = key; found->cbBytes = boundBytes;
    }
    ViewportReader& r = *found;
    struct KeepState { ViewportReader& r; ~KeepState() { g_lastViewportState = r.state; } } keepState{ r };
    auto say = [&](const char* fmt, auto... args) { if (log) { char text[400]; snprintf(text, sizeof text, fmt, args...); log(text); } };
    if (r.state == 0) {
        if (++r.tries > 3) { r.state = 3; return false; }
        ID3D11Buffer* cb = nullptr; ctx->CSGetConstantBuffers(0, 1, &cb);
        if (!cb) { say("NIS pass on part of its output (%ux%u -> %ux%u, %ux%u groups): it has no constants bound; NIS stays", in.Width, in.Height, o.Width, o.Height, x, y); r.state = 3; return false; }
        D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
        if (d.ByteWidth < sizeof(NisConfigView)) { say("NIS pass on part of its output: its constants are %u bytes, too few for NIS's; NIS stays", d.ByteWidth); cb->Release(); r.state = 3; return false; }
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!r.staging && FAILED(dev->CreateBuffer(&sd, nullptr, &r.staging))) { cb->Release(); r.state = 3; return false; }
        ctx->CopyResource(r.staging, cb);
        cb->Release();
        r.state = 1;
        return false;
    }
    if (r.state == 1) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return false;   // not back yet
        memcpy(&r.cfg, m.pData, sizeof r.cfg);
        ctx->Unmap(r.staging, 0);
        const NisConfigView& c = r.cfg;
        const float scaleX = c.outW ? static_cast<float>(c.inW) / c.outW : 0.0f, scaleY = c.outH ? static_cast<float>(c.inH) / c.outH : 0.0f;
        const bool fits = c.inW && c.inH && c.outW && c.outH && c.inX + c.inW <= in.Width && c.inY + c.inH <= in.Height &&
                          c.outX + c.outW <= o.Width && c.outY + c.outH <= o.Height && x == (c.outW + 31) / 32 && y == (c.outH + 23) / 24 &&
                          c.outW + Slack(c.inW) >= c.inW && c.outH + Slack(c.inH) >= c.inH &&   // (an output a hair smaller than the input is taken as 1:1 with the edges trimmed: below)
                          std::abs(c.f[12] - scaleX) < 0.02f * scaleX + 1e-4f && std::abs(c.f[13] - scaleY) < 0.02f * scaleY + 1e-4f;   // kScaleX, kScaleY
        say("NIS pass on part of its output: frame %ux%u, output %ux%u, %ux%u groups; its constants: input viewport %u,%u %ux%u, output viewport %u,%u %ux%u, "
            "scale %.4f x %.4f -> %s", in.Width, in.Height, o.Width, o.Height, x, y, c.inX, c.inY, c.inW, c.inH, c.outX, c.outY, c.outW, c.outH, c.f[12], c.f[13],
            fits ? "the upscaler takes that part" : "they do not fit together; NIS stays");
        r.state = fits ? 2 : 3;
        if (!fits) return false;
    }
    if (r.state != 2) return false;
    const NisConfigView& c = r.cfg;
    pass.inX = c.inX; pass.inY = c.inY; pass.inW = c.inW; pass.inH = c.inH;
    pass.outX = c.outX; pass.outY = c.outY; pass.outW = c.outW; pass.outH = c.outH;
    // An output smaller than the input by a pixel or two: the input is trimmed evenly on both sides to the output's size, so that the upscaler runs 1:1 (DLAA) on the middle (the picture is
    // within a pixel of NIS's own at the very edges and exact in the middle).
    if (pass.outW < pass.inW) { const uint32_t cut = pass.inW - pass.outW; pass.inX += cut / 2; pass.inW = pass.outW; }
    if (pass.outH < pass.inH) { const uint32_t cut = pass.inH - pass.outH; pass.inY += cut / 2; pass.inH = pass.outH; }
    return true;
}

} // namespace

// ---- recognising the NIS pass

namespace { bool g_lastRefused = false; }   // the last FindNisPass was a NIS pass whose layout could not be followed (render thread only)

bool FindNisPass(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, NisPass& pass, const std::function<void(const char*)>& log) {
    pass = {};
    g_lastRefused = false;
    if (z != 1) return false;
    ID3D11ShaderResourceView* srvs[3] = {}; ID3D11UnorderedAccessView* uav = nullptr;
    ctx->CSGetShaderResources(0, 3, srvs); ctx->CSGetUnorderedAccessViews(0, 1, &uav);
    ID3D11Resource* res[3] = {}; ID3D11Resource* out = nullptr;
    for (int i = 0; i < 3; ++i) if (srvs[i]) srvs[i]->GetResource(&res[i]);
    if (uav) uav->GetResource(&out);
    D3D11_TEXTURE2D_DESC in{}, c1{}, c2{}, o{};
    auto coefficients = [](const D3D11_TEXTURE2D_DESC& d) { return d.Width == 2 && d.Height == 64 && d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT; };
    const bool nis = Texture2D(res[0], in) && Texture2D(res[1], c1) && Texture2D(res[2], c2) && Texture2D(out, o) && coefficients(c1) && coefficients(c2);
    const bool whole = nis && o.Width >= in.Width && o.Height >= in.Height &&   // 1:1 too (DLSS then runs as DLAA)
                       x == (o.Width + 31) / 32 && y == (o.Height + 23) / 24;
    for (auto*& v : srvs) SafeRelease(v);
    SafeRelease(uav); SafeRelease(res[1]); SafeRelease(res[2]);
    // NIS's bindings with a dispatch over less than the output: a window of another shape, scaled into part of the screen
    const bool part = nis && !whole && x <= (o.Width + 31) / 32 && y <= (o.Height + 23) / 24 && ResolveViewports(ctx, in, o, x, y, pass, log);
    g_lastRefused = nis && !whole && !part && g_lastViewportState == 3;   // this very pass (not a verdict kept from another window shape)
    if (!whole && !part) { SafeRelease(res[0]); SafeRelease(out); pass = {}; return false; }
    pass.in = res[0]; pass.out = out; pass.inFmt = in.Format; pass.outFmt = o.Format;
    if (whole) { pass.inW = in.Width; pass.inH = in.Height; pass.outW = o.Width; pass.outH = o.Height; }
    return true;
}

void ReleaseNisPass(NisPass& pass) { SafeRelease(pass.in); SafeRelease(pass.out); pass = {}; }

bool NisLayoutRefused() { return g_lastRefused; }


void ResetNisViewports() { for (ViewportReader& e : g_viewports) e.Reset(); g_viewports.clear(); }

} // namespace nr
