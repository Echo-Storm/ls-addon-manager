#include "addon/compose11.h"
#include "addon/bridge.h"
#include "engine/hdr_hlsl.h"
#include <d3dcompiler.h>
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

const char* const kComposeHlsl = NR_HDR_HLSL R"HLSL(
SamplerState sLinear : register(s0);
Texture2D<float4>   tFrame : register(t0);   // a copy of the buffer being presented
Texture2D<float4>   tDelta : register(t1);   // the delta of real frame d, at the working size
Texture2D<float4>   tFlow  : register(t2);   // LSFG's flow: xy = current -> previous, zw = previous -> current
Texture2D<float2>   tMotion : register(t3);  // or the model's motion for frame d, in working-size pixels, d -> the frame before
RWTexture2D<float4> uOut   : register(u0);

cbuffer Constants : register(b0) {
    uint2  size;
    float  offset;        // the presented frame minus d, in real frames
    float  intensity;
    float  maxDelta;
    float  hiProtect;
    uint   debugView;     // 0 result, 1 original, 2 delta x4, 3 frame role, 4 flow, 5 ghost guard weight
    uint   flags;         // 1 flow bound, 2 a generated frame, 4 show the protected areas, 8 the model's motion bound
    float2 uvPerUnit;     // one flow unit, in uv
    float  ghostGuard;    // 0 off .. 1 full
    float  flowPxPerUnit; // flow-texture pixels in one flow unit
    float  sharpen;
    uint   compare;       // 0 enhanced, 1 split, 2 original only
    float  splitPos;
    uint   marker;        // 0 none, 1 green, 2 red, 3 amber, 4 blue, 5 purple
    float  saturation;
    float  vibrance;
    float  brightness;
    float  contrast;
    float  gamma;
    float  shadows;
    float  highlights;
    float  grain;
    uint   grainSeed;
    float  grainSize;
    uint   hudCount;
    float  hudFeather;
    float4 hud[6];        // left, top, right, bottom, 0..1
    uint   encoding;      // the frame's: 0 SDR, 1 scRGB, 2 HDR10 (hdr_hlsl.h)
    float  white;         // the SDR white, in nits (HDR only)
};
static const float3 kLuma = float3(0.299, 0.587, 0.114);
static const float3 kHudGreen = float3(0.49, 0.70, 0.26);

// How far inside the protected areas uv is: 1 well inside, 0 outside, a soft ramp across the edge.
float HudInside(float2 uv) {
    float inside = 0.0;
    for (uint i = 0u; i < hudCount && i < 6u; ++i) {
        const float2 fromEdge = min(uv - hud[i].xy, hud[i].zw - uv);
        inside = max(inside, saturate(min(fromEdge.x, fromEdge.y) / max(hudFeather, 1e-5) + 0.5));
    }
    return inside;
}

// Ghost guard. The delta comes from an earlier (or later) frame and is moved here along LSFG's flow, so where that flow is wrong it lands in the
// wrong place, which shows as a faint copy of another frame. The two fields LSFG gives are each other's reverse on a steady move: where their
// sum is large (an object's edge, something just uncovered) the delta is faded out, and a delta more than a frame old a little more.
float GhostWeight(float4 flow) {
    if (ghostGuard <= 0.001 || (flags & 1u) == 0u) return 1.0;
    const float disagreement = length(flow.xy + flow.zw) * flowPxPerUnit;   // in flow-texture pixels
    const float consistent = 1.0 - smoothstep(0.3, 1.5, disagreement);
    const float fresh = rcp(1.0 + 0.35 * max(0.0, offset - 1.0));
    return lerp(1.0, consistent * fresh, saturate(ghostGuard));
}

// Contrast-adaptive sharpening (the AMD FidelityFX CAS formula, MIT): the weight shrinks where a channel is already near 0 or 1, so detail is
// sharpened without clipping. c is the centre, n/w/e/s its neighbours.
float3 Sharpen(float3 n, float3 w, float3 c, float3 e, float3 s, float amount) {
    const float3 lo = min(min(min(w, c), min(e, n)), s);
    const float3 hi = max(max(max(w, c), max(e, n)), s);
    const float3 room = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 1e-4)));
    const float3 k = room * (-1.0 / lerp(8.0, 5.0, amount));
    return saturate((n * k + w * k + e * k + s * k + c) / (1.0 + 4.0 * k));
}

// The frame's SDR view (the identity for an SDR frame): everything below works on it, as the model did.
float3 Sdr(float3 c) { return ToSdr(c, encoding, white); }

// Tone on the encoded (display) values, as a monitor's controls work: contrast around mid-grey, brightness, then the gamma curve (above 1
// brightens the mid-tones and leaves black and white alone).
float3 Tone(float3 c) {
    c = saturate((c - 0.5) * contrast + 0.5 + brightness);
    return pow(max(c, 1e-5), 1.0 / max(gamma, 0.05));
}

// Shadows act on the dark tones and fade out by 0.55 luma, highlights on the bright ones from 0.45; both add the same to every channel, so
// colours keep their differences.
float3 TonalRanges(float3 c) {
    const float l = dot(c, kLuma);
    return saturate(c + (shadows * (1.0 - smoothstep(0.0, 0.55, l)) + highlights * smoothstep(0.45, 1.0, l)) * 0.25);
}

// Saturation scales each colour's distance from the pixel's luma; vibrance adds to that only where the pixel is muted (a small spread between
// its channels), so vivid colours and skin are not pushed further.
float3 Colour(float3 c) {
    const float l = dot(c, kLuma);
    const float spread = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    return saturate(l + (c - l) * (saturation + vibrance * (1.0 - saturate(spread))));
}

// Monochrome grain, strongest in the mid-tones and new at every present (grainSeed).
float3 Grain(float3 c, uint2 pixel) {
    const uint2 cell = pixel / max(1u, (uint)grainSize);
    uint h = cell.x * 1973u + cell.y * 9277u + grainSeed * 26699u + 1u;
    h = (h ^ 61u) ^ (h >> 16); h *= 9u; h ^= h >> 4; h *= 0x27d4eb2du; h ^= h >> 15;
    const float noise = (float)(h & 0xFFFFu) / 32767.5 - 1.0;
    const float l = dot(c, kLuma);
    return saturate(c + noise * grain * 0.10 * (0.35 + 0.65 * 4.0 * l * (1.0 - l)));
}

float3 MarkerColour() {
    return marker == 1 ? float3(0.10, 0.85, 0.20) : marker == 2 ? float3(0.90, 0.15, 0.15) : marker == 3 ? float3(0.95, 0.70, 0.10)
         : marker == 4 ? float3(0.15, 0.45, 0.95) : float3(0.65, 0.30, 0.90);
}

[numthreads(8, 8, 1)]
void CSCompose(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float4 frame = tFrame[id.xy];
    const float3 fs = Sdr(frame.rgb);
    bool replaced = false;   // a debug view or an overlay: written as it is, not as a change to the frame
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    const float hudInside = HudInside(uv);
    const bool plain = debugView == 0 || debugView == 3;   // the views that show the picture as it will be seen
    const bool original = compare == 2 || (compare == 1 && uv.x < splitPos) || (hudInside > 0.999 && plain);
    float3 c = fs;
    if (!original) {
        const float4 flow = (flags & 1u) ? tFlow.SampleLevel(sLinear, uv, 0) : float4(0, 0, 0, 0);
        // where this pixel's content was in frame d
        float2 uvInD = offset > 0.0 ? uv - offset * flow.zw * uvPerUnit : uv + offset * flow.xy * uvPerUnit;
        if (flags & 8u) {   // frame d's own motion stands in for the frames since: where this pixel was, `offset` frames back at that speed
            uint mw, mh; tMotion.GetDimensions(mw, mh);
            uvInD = uv + offset * tMotion.SampleLevel(sLinear, uv, 0) / float2(mw, mh);
        }
        const float ghost = GhostWeight(flow);
        const float highlightFade = hiProtect < 0.999 ? 1.0 - smoothstep(hiProtect, 1.0, dot(fs, kLuma)) : 1.0;
        const float3 d = clamp(tDelta.SampleLevel(sLinear, uvInD, 0).rgb * ghost * intensity, -maxDelta, maxDelta) * highlightFade;
        c = saturate(fs + d);
        if (plain) {
            if (sharpen > 0.001) {   // the delta is smooth, so the neighbours take the centre's delta rather than more flow and delta reads
                const int2 p = int2(id.xy), last = int2(size) - 1;
                c = Sharpen(saturate(Sdr(tFrame[clamp(p + int2(0, -1), 0, last)].rgb) + d), saturate(Sdr(tFrame[clamp(p + int2(-1, 0), 0, last)].rgb) + d), c,
                            saturate(Sdr(tFrame[clamp(p + int2(1, 0), 0, last)].rgb) + d), saturate(Sdr(tFrame[clamp(p + int2(0, 1), 0, last)].rgb) + d), saturate(sharpen));
            }
            if (abs(brightness) > 0.0005 || abs(contrast - 1.0) > 0.002 || abs(gamma - 1.0) > 0.002) c = Tone(c);
            if (abs(shadows) > 0.005 || abs(highlights) > 0.005) c = TonalRanges(c);
            if (abs(saturation - 1.0) > 0.002 || vibrance > 0.002) c = Colour(c);
            if (grain > 0.002) c = Grain(c, id.xy);
            if (hudInside > 0.0) c = lerp(c, fs, hudInside);
        }
        replaced = debugView >= 2u;
        if      (debugView == 1) c = fs;
        else if (debugView == 2) c = saturate(0.5 + d * 4.0);
        else if (debugView == 3) c = saturate(c + ((flags & 2u) ? float3(0.15, 0, 0) : float3(0, 0.15, 0)));
        else if (debugView == 4) c = saturate(float3(0.5 + flow.xy / 16.0, 0.5));
        else if (debugView == 5) c = ghost.xxx;   // white: the delta lands in full; dark: faded out
    }
    if (flags & 4u) {   // the protected areas shown while editing them: a green tint and outline
        float inside = 0.0, edge = 0.0;
        const float edgeWidth = 1.5 / (float)size.x;
        for (uint i = 0u; i < hudCount && i < 6u; ++i) {
            const float2 fromEdge = min(uv - hud[i].xy, hud[i].zw - uv);
            const float nearest = min(fromEdge.x, fromEdge.y);
            if (nearest > 0.0) inside = 1.0;
            if (abs(nearest) < edgeWidth) edge = 1.0;
        }
        c = edge > 0.5 ? kHudGreen : lerp(c, kHudGreen, inside * 0.22);
        replaced = replaced || edge > 0.5 || inside > 0.0;
    }
    if (compare == 1 && abs((float)id.x + 0.5 - splitPos * (float)size.x) < max(1.0, (float)size.x / 2000.0)) { c = float3(0.9, 0.9, 0.9); replaced = true; }   // the split
    if (marker != 0u) {
        const uint side = max(12u, size.x / 150u);
        if (id.x < side && id.y < side) { c = MarkerColour(); replaced = true; }
    }
    float3 result = c;
    if (encoding != 0u) {
        if (replaced) result = FromSdr(c, encoding, white);
        else {
            // Only the change goes back, so what was not changed (highlights brighter than the SDR view holds, colours outside Rec.709) stays
            // as it was; and the change fades out over the top of the rolled-off range, where a step in the SDR view is a large one in light.
            // In light, not in the SDR view's values: from 0.8 to 1.35 times the SDR white (what 0.90 to 0.99 were on the curve until 0.9.8).
            const float3 light = Expand(SrgbToLinear(saturate(fs)));
            const float keep = 1.0 - smoothstep(0.8, 1.35, max(light.r, max(light.g, light.b)));
            // And the change never lifts a channel above 1.35 times the SDR white (unless the frame was brighter there already): the
            // logarithmic top of the curve (since 0.9.9) turns a step from 0.7 to 0.95 in the SDR view into about 1900 nits in light,
            // so a colour the model brightened in one channel came back as neon orange or cyan (issue #3; 0.9.8's curve gave about 90).
            const float3 cap = max(light, 1.35);
            const float3 changed = LightToSdr(min(SdrToLight(lerp(fs, c, keep)), cap));
            result = frame.rgb + (FromSdr(changed, encoding, white) - FromSdr(fs, encoding, white));
        }
    }
    uOut[id.xy] = float4(result, frame.a);
}
)HLSL";

// The constant buffer as the shader lays it out: 16-byte rows, hud[] from byte 112, the whole a multiple of 16 (D3D11 refuses any other size).
struct Constants {
    uint32_t w, h; float offset, intensity, maxDelta, hiProtect; uint32_t debugView, flags;
    float uvPerUnitX, uvPerUnitY, ghostGuard, flowPxPerUnit;
    float sharpen; uint32_t compare; float splitPos; uint32_t marker;
    float saturation, vibrance, brightness, contrast;
    float gamma, shadows, highlights, grain;
    uint32_t grainSeed; float grainSize; uint32_t hudCount; float hudFeather;
    float hud[6][4];
    uint32_t encoding; float white; uint32_t pad[2];
};
static_assert(offsetof(Constants, gamma) == 80 && offsetof(Constants, hud) == 112 && offsetof(Constants, encoding) == 208 && sizeof(Constants) == 224,
              "Constants must match the shader's cbuffer");

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Lossless Scaling's compute state, taken before the pass and put back after it: the pass runs in the middle of its frame.
struct SavedComputeState {
    ID3D11DeviceContext* ctx;
    ID3D11ComputeShader* shader = nullptr; ID3D11ShaderResourceView* srvs[4] = {}; ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Buffer* constants = nullptr; ID3D11SamplerState* sampler = nullptr;
    explicit SavedComputeState(ID3D11DeviceContext* c) : ctx(c) {
        ctx->CSGetShader(&shader, nullptr, nullptr); ctx->CSGetShaderResources(0, 4, srvs); ctx->CSGetUnorderedAccessViews(0, 1, &uav);
        ctx->CSGetConstantBuffers(0, 1, &constants); ctx->CSGetSamplers(0, 1, &sampler);
    }
    ~SavedComputeState() {
        ctx->CSSetShader(shader, nullptr, 0); ctx->CSSetShaderResources(0, 4, srvs); ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &constants); ctx->CSSetSamplers(0, 1, &sampler);
        SafeRelease(shader); for (auto*& s : srvs) SafeRelease(s); SafeRelease(uav); SafeRelease(constants); SafeRelease(sampler);
    }
};

} // namespace

void Compose11::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    m_log(text);
}

bool Compose11::Init(ID3D11Device* dev, LogFn log) {
    Shutdown();
    m_log = std::move(log); m_dev = dev; m_dev->AddRef();
    ID3DBlob* code = nullptr, * error = nullptr;
    if (FAILED(D3DCompile(kComposeHlsl, strlen(kComposeHlsl), "compose11", nullptr, nullptr, "CSCompose", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
        Log("Compose11: HLSL: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?"); SafeRelease(error); Shutdown(); return false;
    }
    const HRESULT hr = m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_shader);
    code->Release();
    if (FAILED(hr)) { Log("Compose11: CreateComputeShader 0x%08x", (unsigned)hr); Shutdown(); return false; }
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(Constants); cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_dev->CreateBuffer(&cb, nullptr, &m_constants))) { Log("Compose11: the constant buffer could not be made"); Shutdown(); return false; }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_dev->CreateSamplerState(&sampler, &m_sampler))) { Log("Compose11: the sampler could not be made"); Shutdown(); return false; }
    m_cpuMs = 0; m_runs = 0;
    Log("Compose11: ready");
    return true;
}

void Compose11::DropCopy() {
    SafeRelease(m_copyView); SafeRelease(m_copy); SafeRelease(m_outView); SafeRelease(m_out);
    m_w = m_h = 0; m_fmt = DXGI_FORMAT_UNKNOWN;
}

void Compose11::Shutdown() {
    DropCopy();
    SafeRelease(m_flowView); SafeRelease(m_flow);
    SafeRelease(m_shader); SafeRelease(m_constants); SafeRelease(m_sampler); SafeRelease(m_dev);
}

// The copy of the buffer the pass reads, for this size and format (and m_out, made by TargetView when a buffer cannot be written directly).
bool Compose11::FitCopy(const D3D11_TEXTURE2D_DESC& target, DXGI_FORMAT view) {
    if (m_copy && target.Width == m_w && target.Height == m_h && view == m_fmt) return true;
    DropCopy();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = target.Width; desc.Height = target.Height; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = target.Format; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = view; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, &m_copy);
    if (SUCCEEDED(hr)) hr = m_dev->CreateShaderResourceView(m_copy, &srv, &m_copyView);
    if (FAILED(hr)) { Log("Compose11: a %ux%u copy (format %d) could not be made: 0x%08x", target.Width, target.Height, (int)target.Format, (unsigned)hr); DropCopy(); return false; }
    m_w = target.Width; m_h = target.Height; m_fmt = view;
    snprintf(m_targetInfo, sizeof m_targetInfo, "%ux%u fmt %d%s", target.Width, target.Height, (int)target.Format,
             (target.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? " (UAV, in place)" : " (copy back)");
    Log("Compose11: target %s", m_targetInfo);
    return true;
}

// A view of the buffer itself when a compute pass may write it (made at every present and released after it: a view kept would hold the
// swap chain buffer, and DXGI refuses to resize a swap chain while anything holds one), otherwise m_out.
ID3D11UnorderedAccessView* Compose11::TargetView(ID3D11Texture2D* target, DXGI_FORMAT view) {
    D3D11_TEXTURE2D_DESC desc; target->GetDesc(&desc);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = view; uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    ID3D11UnorderedAccessView* direct = nullptr;
    if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) && SUCCEEDED(m_dev->CreateUnorderedAccessView(target, &uav, &direct))) return direct;
    // not writable by a compute pass (or the view failed): write m_out and copy it over the buffer
    if (!m_out) {
        desc.MipLevels = 1; desc.ArraySize = 1; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.CPUAccessFlags = 0; desc.MiscFlags = 0;
        if (FAILED(m_dev->CreateTexture2D(&desc, nullptr, &m_out)) || FAILED(m_dev->CreateUnorderedAccessView(m_out, &uav, &m_outView))) {
            Log("Compose11: the result texture could not be made"); SafeRelease(m_out); return nullptr;
        }
    }
    return m_outView;
}

ID3D11ShaderResourceView* Compose11::FlowView(ID3D11Resource* flow) {
    if (!flow) return nullptr;
    if (flow == m_flow && m_flowView) return m_flowView;
    SafeRelease(m_flowView); SafeRelease(m_flow);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1;
    if (FAILED(m_dev->CreateShaderResourceView(flow, &srv, &m_flowView))) return nullptr;
    m_flow = flow; m_flow->AddRef();
    return m_flowView;
}

bool Compose11::Run(ID3D11DeviceContext* ctx, const Args& a) {
    if (!m_shader || !ctx || !a.target || !a.delta) return false;
    LARGE_INTEGER freq, start; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    D3D11_TEXTURE2D_DESC desc; a.target->GetDesc(&desc);
    const DXGI_FORMAT view = Bridge::ViewFormat(desc.Format);
    if (view == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1) {   // 8-bit, 10-bit and half-float buffers work: the pass changes their SDR view
        const uint64_t key = 0xC000000000000000ull | (uint32_t)desc.Format;
        if (key != m_lastFailure) { m_lastFailure = key; Log("Compose11: presented frames of format %d (or multisampled) cannot be composed", (int)desc.Format); }
        return false;
    }
    if (!FitCopy(desc, view)) return false;
    ID3D11UnorderedAccessView* out = TargetView(a.target, view);
    if (!out) return false;
    const bool inPlace = out != m_outView;   // then `out` is ours to release

    ctx->CopyResource(m_copy, a.target);
    ID3D11ShaderResourceView* const flowView = FlowView(a.flow);
    const float unit = a.flowUnit > 0.1f ? a.flowUnit : 2.0f;
    Constants c{};
    c.w = desc.Width; c.h = desc.Height; c.offset = a.offset; c.intensity = a.intensity; c.maxDelta = a.maxDelta; c.hiProtect = a.hiProtect;
    const bool moved = !flowView && a.motion;
    c.debugView = a.debugView; c.flags = (flowView ? 1u : 0u) | (a.isGen ? 2u : 0u) | (a.hudShow ? 4u : 0u) | (moved ? 8u : 0u);
    c.uvPerUnitX = flowView && a.flowW ? 1.0f / (unit * a.flowW) : 0.0f; c.uvPerUnitY = flowView && a.flowH ? 1.0f / (unit * a.flowH) : 0.0f;
    c.ghostGuard = flowView ? std::clamp(a.ghostGuard, 0.0f, 1.0f) : 0.0f; c.flowPxPerUnit = 1.0f / unit;
    c.sharpen = a.sharpen; c.compare = a.compare; c.splitPos = a.splitPos; c.marker = a.marker;
    c.saturation = a.saturation; c.vibrance = a.vibrance; c.brightness = a.brightness; c.contrast = a.contrast; c.gamma = a.gamma;
    c.shadows = a.shadows; c.highlights = a.highlights; c.grain = a.grain; c.grainSeed = a.grainSeed; c.grainSize = std::clamp(a.grainSize, 1.0f, 4.0f);
    c.hudCount = std::min(a.hudCount, 6u); c.hudFeather = a.hudFeather; memcpy(c.hud, a.hud, sizeof c.hud);
    c.encoding = a.encoding; c.white = a.whiteNits > 1.0f ? a.whiteNits : 200.0f;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { memcpy(mapped.pData, &c, sizeof c); ctx->Unmap(m_constants, 0); }

    {
        const SavedComputeState saved(ctx);
        ID3D11ShaderResourceView* const srvs[4] = { m_copyView, a.delta, flowView, moved ? a.motion : nullptr };
        ctx->CSSetShader(m_shader, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &m_constants);
        ctx->CSSetSamplers(0, 1, &m_sampler);
        ctx->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
        ID3D11ShaderResourceView* const noSrvs[4] = {}; ID3D11UnorderedAccessView* const noUav = nullptr;
        ctx->CSSetShaderResources(0, 4, noSrvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    }
    if (inPlace) out->Release(); else ctx->CopyResource(a.target, m_out);

    ++m_runs;
    LARGE_INTEGER end; QueryPerformanceCounter(&end);
    const double ms = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    m_cpuMs = m_cpuMs == 0 ? ms : m_cpuMs * 0.9 + ms * 0.1;
    return true;
}
