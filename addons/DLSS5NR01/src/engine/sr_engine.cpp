#include "engine/sr_engine.h"
#include "engine/ngx_users.h"
#include "engine/ngx_paths.h"
#include "engine/hdr_hlsl.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"
#include "ffx_api/ffx_api_loader.h"
#if __has_include("xess/xess_d3d12.h")
#include "xess/xess_d3d12.h"
#endif
#include <type_traits>
#include "ffx_api/dx12/ffx_api_dx12.h"

struct SrEngine::FfxState { HMODULE module = nullptr; ffxFunctions fn{}; ffxContext context = nullptr; };

// Intel's XeSS runtime (libxess.dll from the addon's xess folder, loaded by its full path; its headers come with Intel's SDK, which
// tools\fetch_xess_sdk.ps1 puts in external\xess). Built without them, the XeSS backend only says so.
#if __has_include("xess/xess_d3d12.h")
#define NR_HAVE_XESS 1
struct SrEngine::XessState {
    HMODULE module = nullptr; xess_context_handle_t context = nullptr; bool initialised = false; xess_version_t version{};
    decltype(&xessGetVersion) GetVersion = nullptr; decltype(&xessD3D12CreateContext) CreateContext = nullptr;
    decltype(&xessGetOptimalInputResolution) OptimalInput = nullptr; decltype(&xessD3D12Init) Init = nullptr;
    decltype(&xessD3D12Execute) Execute = nullptr; decltype(&xessDestroyContext) DestroyContext = nullptr;
    decltype(&xessSetLoggingCallback) SetLogging = nullptr;
    decltype(&xessSetMaxResponsiveMaskValue) SetMaxResponsive = nullptr; float maxResponsive = -1.0f;
};
#else
struct SrEngine::XessState { HMODULE module = nullptr; bool initialised = false; };
#endif

namespace {

constexpr float kFastMotionShare = 0.001f;   // the motion (a share of the frame's width, a frame) from which the upscaler leans on the frame, at 1:1
// (DLAA): 0.5 % until 2026-09-28; at 0.1 % (about 4 px at 3840) DLSS E in World of Warcraft trails far less (coarse 45.70 -> 47.74 dB against
// the game's own frame, which at 1:1 still shows trailing though not the aliasing). The owner can move it ("Lean from", scalerLeanFrom).
// When upscaling, from much less: without the game's jitter the history adds no detail in motion, only trailing. nr_sreval (DLSS L, 1.5x),
// 2026-09-28: from 0.5 % to 0.05 % of the width, Silent Hill f turn coarse 43.18 -> 47.02 dB (a plain stretch 43.94), its slow start
// 47.02 -> 48.42, World of Warcraft walking 42.13 -> 43.18 (at 1:1 the score cannot tell, the frame itself being the reference: kept)
constexpr float kFastMotionShareUpscaling = 0.0005f;

constexpr unsigned long long kAppId = 0x24480452ull;   // the upscaler's NGX application id (Neural Rendering's is ...451)
constexpr DWORD kSlotWaitMs = 500, kIdleWaitMs = 5000;

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Frame generation's flow (xy: from this frame to the one before, in units of 1/flowUnit of a flow pixel) as DLSS motion vectors in pixels of
// the game's frame: where each pixel was in the presented frame before, a fraction of a real frame ago. Zero without flow.
const char* kMotionHlsl = R"(
Texture2D<float4> tFlow : register(t0);
RWTexture2D<float2> uMotion : register(u0);
SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 size; float scale; uint hasFlow; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    uMotion[id.xy] = hasFlow != 0 ? tFlow.SampleLevel(sLinear, uv, 0).xy * scale : float2(0, 0);
}
)";
struct MotionConstants { uint32_t w, h; float scale; uint32_t hasFlow; };

// Contrast-adaptive sharpening of the upscaler's picture (the AMD FidelityFX CAS formula, MIT; as Neural Rendering's compose uses it): the
// weight shrinks where a channel is already near 0 or 1, so detail is sharpened without clipping. `gain` above 1 amplifies what it changed,
// past CAS's own maximum (the upscalers' slider reaches 1.6 times it); the result stays within 0..1.
const char* kSharpenHlsl = R"(
Texture2D<float4> tIn : register(t0);
RWTexture2D<float4> uOut : register(u0);
cbuffer C : register(b0) { uint2 size; float amount; float gain; uint hdr; };
float3 View(float3 c) { return hdr != 0u ? LightToSdr(c) : c; }   // HDR: the picture is light; CAS decides in the SDR view
float3 Sharpen(float3 n, float3 w, float3 c, float3 e, float3 s, float a) {
    const float3 lo = min(min(min(w, c), min(e, n)), s);
    const float3 hi = max(max(max(w, c), max(e, n)), s);
    const float3 room = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 1e-4)));
    const float3 k = room * (-1.0 / lerp(8.0, 5.0, a));
    return saturate((n * k + w * k + e * k + s * k + c) / (1.0 + 4.0 * k));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const int2 p = int2(id.xy), last = int2(size) - 1;
    const float4 c = tIn[p];
    const float3 v = View(c.rgb);
    const float3 s = Sharpen(View(tIn[clamp(p + int2(0, -1), 0, last)].rgb), View(tIn[clamp(p + int2(-1, 0), 0, last)].rgb), v,
                             View(tIn[clamp(p + int2(1, 0), 0, last)].rgb), View(tIn[clamp(p + int2(0, 1), 0, last)].rgb), saturate(amount));
    const float3 r = saturate(v + (s - v) * gain);
    // HDR: only the change goes back into light, so what sharpening leaves alone (a highlight's flat middle) stays exactly as it was
    uOut[p] = float4(hdr != 0u ? c.rgb + (SdrToLight(r) - SdrToLight(v)) : r, c.a);
}
)";
struct SharpenConstants { uint32_t w, h; float amount, gain; uint32_t hdr; };

// The same sharpening, made to follow the picture's stability, in one pass. It keeps a running average of the picture in the SDR view (the previous
// average fetched where the motion says the pixel was, clamped to this frame's neighbourhood so a wrong motion cannot ghost, and trusted less the faster
// the pixel moves), sharpens that average and adds only its sharpening to this frame. The game's shimmer averages out of the average, so it is not amplified
// by the sharpening, while the detail stays. One 8x8 group loads a 12x12 tile of the picture once, works the average out for the 10x10 around its pixels
// and sharpens from that, and writes the average for the next frame (the second of two history textures, taken in turns). nr_sreval at 1:1
// (docs/frame-generation-research.md) measures flicker and detail; scored against the true picture when upscaling.
const char* kSteadySharpenHlsl = R"(
Texture2D<float4> tIn : register(t0);
Texture2D<float4> tHist : register(t1);
Texture2D<float2> tMotion : register(t2);
RWTexture2D<float4> uOut : register(u0);
RWTexture2D<float4> uHistOut : register(u1);
SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 size; uint2 inSize; float amount; float gain; uint hdr; float steady; float mvScale; uint histOk; float mvA; float mvB; };
groupshared float3 gIn[12][12];
groupshared float3 gStab[10][10];
float3 View(float3 c) { return hdr != 0u ? LightToSdr(c) : c; }
float3 Sharpen(float3 n, float3 w, float3 c, float3 e, float3 s, float a) {
    const float3 lo = min(min(min(w, c), min(e, n)), s);
    const float3 hi = max(max(max(w, c), max(e, n)), s);
    const float3 room = sqrt(saturate(min(lo, 1.0 - hi) / max(hi, 1e-4)));
    const float3 k = room * (-1.0 / lerp(8.0, 5.0, a));
    return saturate((n * k + w * k + e * k + s * k + c) / (1.0 + 4.0 * k));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int2 last = int2(size) - 1, origin = int2(gid.xy) * 8 - 2;
    for (uint i = gi; i < 144u; i += 64u) {
        const int2 t = int2(i % 12u, i / 12u);
        gIn[t.y][t.x] = View(tIn[clamp(origin + t, 0, last)].rgb);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < 100u; j += 64u) {
        const int2 s = int2(j % 10u, j / 10u), pix = origin + 1 + s;
        const float3 v = gIn[s.y + 1][s.x + 1];
        float3 stab = v;
        if (histOk != 0u) {
            const int2 q = clamp(int2((float2(clamp(pix, 0, last)) + 0.5) * float2(inSize) / float2(size)), 0, int2(inSize) - 1);
            const float2 mv = tMotion[q] * mvScale;                                  // in output pixels
            // the faster the pixel moves, the less the average is trusted (a small error in the motion moves detail the sharpening then adds in the wrong place);
            // where it is not trusted at all the history is not even read
            const float trust = 1.0 - saturate((length(mv) - mvA) / max(mvB - mvA, 1e-3));
            if (trust > 0.0) {
                float3 lo = v, hi = v;
                [unroll] for (int dy = 0; dy < 3; ++dy) [unroll] for (int dx = 0; dx < 3; ++dx) { const float3 n = gIn[s.y + dy][s.x + dx]; lo = min(lo, n); hi = max(hi, n); }
                const float3 h = clamp(tHist.SampleLevel(sLinear, (float2(pix) + 0.5 + mv) / float2(size), 0).rgb, lo, hi);
                stab = lerp(v, h, steady * trust);
            }
        }
        gStab[s.y][s.x] = stab;
        if (s.x >= 1 && s.x <= 8 && s.y >= 1 && s.y <= 8 && all(pix >= 0) && all(pix <= last)) uHistOut[pix] = float4(stab, 1.0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= size.x || id.y >= size.y) return;
    const int2 p = int2(id.xy);
    const float4 c = tIn[p];
    const float3 v = gIn[gt.y + 2][gt.x + 2];
    const float3 m = gStab[gt.y + 1][gt.x + 1];
    const float3 s = Sharpen(gStab[gt.y][gt.x + 1], gStab[gt.y + 1][gt.x], m, gStab[gt.y + 1][gt.x + 2], gStab[gt.y + 2][gt.x + 1], saturate(amount));
    const float3 r = saturate(v + (s - m) * gain);   // this frame, plus the average's sharpening
    uOut[p] = float4(hdr != 0u ? c.rgb + (SdrToLight(r) - SdrToLight(v)) : r, c.a);
}
)";
struct SteadyConstants { uint32_t w, h, inW, inH; float amount, gain; uint32_t hdr; float steady, mvScale; uint32_t histOk; float mvA, mvB; };

// The lean, after every upscaler: without the sub-pixel jitter a game gives it, an upscaler's history trails in a fast turn, and DLSS's
// transformer models (presets J, K, M: DLSS 4) take no mask to lean on the current frame (its bias-current-colour mask changes nothing). So
// the upscaler's picture is blended toward this frame upscaled plainly (Catmull-Rom), by the distrust mask (where the motion cannot be
// trusted, or is fast). nr_sreval, Silent Hill f, a 30-frame turn shrunk 1.5x, at a quarter of the size (a plain stretch 43.3 dB): DLSS
// 36.8 -> 44.2, FSR 3.1 41.7 -> 45.6 (on top of its own reactive mask), XeSS 40.2 -> 46.2; a slow pan unchanged.
const char* kLeanHlsl = R"(
Texture2D<float4> tUp : register(t0);
Texture2D<float4> tIn : register(t1);
Texture2D<float> tDistrust : register(t2);
RWTexture2D<float4> uOut : register(u0);
SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 size; uint2 inSize; float strength; uint mode; float rest; };   // rest: the least the frame is blended in, even at rest   // mode 0: Catmull-Rom, 1: EASU
float4 CatmullRom(float2 uv) {   // 16 loads, the input's size
    const float2 pos = uv * float2(inSize) - 0.5, base = floor(pos), f = pos - base;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    const float wx[4] = { w0.x, w1.x, w2.x, w3.x }, wy[4] = { w0.y, w1.y, w2.y, w3.y };
    const int2 last = int2(inSize) - 1;
    float4 sum = 0;
    [unroll] for (int j = 0; j < 4; ++j) [unroll] for (int i = 0; i < 4; ++i) sum += tIn.Load(int3(clamp(int2(base) + int2(i - 1, j - 1), int2(0, 0), last), 0)) * (wx[i] * wy[j]);
    return sum;
}
// Edge-adaptive spatial upscaling: AMD FidelityFX Super Resolution 1's EASU (from the FidelityFX SDK, ffx_fsr1.h, MIT licence, Copyright (C) Advanced
// Micro Devices, Inc.; NOTICE.md), ported to plain HLSL with direct loads in place of the gathers. A 12-tap kernel (b c / e f g h / i j k l / n o) whose
// direction and length follow the local luma gradient, so a diagonal edge is not stair-stepped and a soft one is not sharpened, and the result is
// held between the min and max of the 4 nearest.
float EasuLuma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void EasuSet(inout float2 dir, inout float len, float2 pp, bool biS, bool biT, bool biU, bool biV, float lA, float lB, float lC, float lD, float lE) {
    float weight = 0.0;
    if (biS) weight = (1.0 - pp.x) * (1.0 - pp.y);
    if (biT) weight = pp.x * (1.0 - pp.y);
    if (biU) weight = (1.0 - pp.x) * pp.y;
    if (biV) weight = pp.x * pp.y;
    const float lengthXBase = 1.0 / max(max(abs(lD - lC), abs(lC - lB)), 1e-8);
    const float directionX = lD - lB;
    dir.x += directionX * weight;
    float lengthX = saturate(abs(directionX) * lengthXBase); lengthX *= lengthX;
    len += lengthX * weight;
    const float lengthYBase = 1.0 / max(max(abs(lE - lC), abs(lC - lA)), 1e-8);
    const float directionY = lE - lA;
    dir.y += directionY * weight;
    float lengthY = saturate(abs(directionY) * lengthYBase); lengthY *= lengthY;
    len += lengthY * weight;
}
void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0, wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    const float w = wB * wA;
    aC += c * w; aW += w;
}
float4 Easu(float2 uv) {
    float2 pp = uv * float2(inSize) - 0.5;
    const float2 fp = floor(pp);
    pp -= fp;
    const int2 last = int2(inSize) - 1, f0 = int2(fp);
    #define T(dx, dy) tIn.Load(int3(clamp(f0 + int2(dx, dy), int2(0, 0), last), 0)).rgb
    const float3 cb = T(0, -1), cc = T(1, -1), ce = T(-1, 0), cf = T(0, 0), cg = T(1, 0), ch = T(2, 0), ci = T(-1, 1), cj = T(0, 1), ck = T(1, 1), cl = T(2, 1), cn = T(0, 2), co = T(1, 2);
    #undef T
    const float bL = EasuLuma(cb), cL = EasuLuma(cc), eL = EasuLuma(ce), fL = EasuLuma(cf), gL = EasuLuma(cg), hL = EasuLuma(ch);
    const float iL = EasuLuma(ci), jL = EasuLuma(cj), kL = EasuLuma(ck), lL = EasuLuma(cl), nL = EasuLuma(cn), oL = EasuLuma(co);
    float2 dir = 0; float len = 0;
    EasuSet(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    EasuSet(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);
    const float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zro = dirR < 1.0 / 32768.0;
    dirR = rsqrt(max(dirR, 1e-12));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5; len *= len;
    const float stretch = (dir.x * dir.x + dir.y * dir.y) / max(max(abs(dir.x), abs(dir.y)), 1e-8);
    const float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    const float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    const float clp = 1.0 / lob;
    const float3 min4 = min(min(cf, cg), min(cj, ck)), max4 = max(max(cf, cg), max(cj, ck));
    float3 aC = 0; float aW = 0;
    EasuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, cb);
    EasuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, cc);
    EasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, ci);
    EasuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, cj);
    EasuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, cf);
    EasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, ce);
    EasuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, ck);
    EasuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, cl);
    EasuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, ch);
    EasuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, cg);
    EasuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, co);
    EasuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, cn);
    return float4(min(max4, max(min4, aC / aW)), 1.0);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(size);
    const float4 up = tUp[id.xy];
    const float d = max(saturate(tDistrust.SampleLevel(sLinear, uv, 0) * strength), saturate(rest));
    if (d <= 0.001) { uOut[id.xy] = up; return; }
    const float4 plain = mode == 1u ? Easu(uv) : CatmullRom(uv);
    uOut[id.xy] = float4(lerp(up.rgb, max(plain.rgb, 0.0), d), up.a);   // (Catmull-Rom can overshoot below 0)
}
)";
struct LeanConstants { uint32_t w, h, inW, inH; float strength; uint32_t mode; float rest; };

// Edge smoothing of the upscaler's picture, for games without anti-aliasing of their own. Where the brightness steps sharply (an edge drawn
// without anti-aliasing: stair steps), it finds which way the edge runs and how far along it each way the step continues, which says where
// on the stair this pixel sits; then it blends the pixel with its neighbour across the edge by the fraction of a pixel the true edge would
// have covered. A lone pixel that stands out from all its neighbours (a thin line's broken bit) is softened too. strength 0..1 scales the
// blend. Our own formulation of the well-known idea (morphological / FXAA-style anti-aliasing). It runs after the upscaler, not before:
// both DLSS and FSR 3 rebuild edges their own way and gave the stair steps back (test host, 2026-09-25: a smoothed frame 7.2 levels off the
// ideal edge, after FSR 3 at 1:1 11.4, the same as from the hard frame).
const char* kEdgesHlsl = R"(
Texture2D<float4> tIn : register(t0);
RWTexture2D<float4> uOut : register(u0);
SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 size; float strength; uint hdr; };
float Luma(float3 c) { if (hdr != 0u) c = LightToSdr(c); return dot(c, float3(0.299, 0.587, 0.114)); }   // HDR: judged in the SDR view
float LumaAt(float2 uv) { return Luma(tIn.SampleLevel(sLinear, uv, 0).rgb); }
static const float kSteps[12] = { 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0 };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const int2 p = int2(id.xy), last = int2(size) - 1;
    const float2 inv = 1.0 / float2(size);
    const float2 uv = (float2(p) + 0.5) * inv;
    const float4 c = tIn[p];
    float l[9];   // the 3x3's brightness; k: x = k % 3 - 1, y = k / 3 - 1
    [unroll] for (int k = 0; k < 9; ++k) l[k] = Luma(tIn[clamp(p + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), last)].rgb);
    const float lc = l[4], ln = l[1], ls = l[7], lw = l[3], le = l[5];
    const float lo = min(lc, min(min(ln, ls), min(lw, le))), hi = max(lc, max(max(ln, ls), max(lw, le)));
    const float range = hi - lo;
    if (range < max(0.03, hi * 0.12)) { uOut[p] = c; return; }   // flat, or a step too faint to be an edge
    // which way the edge runs: the change down the columns (a horizontal edge) against the change along the rows (a vertical one)
    const float acrossRows = abs(l[0] - 2.0 * l[3] + l[6]) + 2.0 * abs(ln - 2.0 * lc + ls) + abs(l[2] - 2.0 * l[5] + l[8]);
    const float acrossCols = abs(l[0] - 2.0 * l[1] + l[2]) + 2.0 * abs(lw - 2.0 * lc + le) + abs(l[6] - 2.0 * l[7] + l[8]);
    const bool horizontal = acrossRows >= acrossCols;
    // the side the step is on: towards the neighbour across the edge that differs most
    const float before = horizontal ? ln : lw, after = horizontal ? ls : le;
    const bool towardAfter = abs(after - lc) > abs(before - lc);
    const float side = towardAfter ? after : before;
    const float2 across = horizontal ? float2(0.0, inv.y) : float2(inv.x, 0.0);
    const float2 along = horizontal ? float2(inv.x, 0.0) : float2(0.0, inv.y);
    const float dir = towardAfter ? 1.0 : -1.0;
    const float edgeLuma = 0.5 * (lc + side), gradient = 0.25 * abs(side - lc);
    // along the edge, halfway between this pixel and that neighbour, both ways, until the brightness leaves the edge's
    const float2 start = uv + across * (0.5 * dir);
    float distBack = 0.0, distFwd = 0.0, endBack = 0.0, endFwd = 0.0; bool doneBack = false, doneFwd = false;
    [unroll] for (int s = 0; s < 12; ++s) {
        if (!doneBack) { distBack += kSteps[s]; endBack = LumaAt(start - along * distBack) - edgeLuma; doneBack = abs(endBack) >= gradient; }
        if (!doneFwd) { distFwd += kSteps[s]; endFwd = LumaAt(start + along * distFwd) - edgeLuma; doneFwd = abs(endFwd) >= gradient; }
    }
    // the nearer end says where on the stair this pixel is: at the step's middle nothing moves, at its ends up to half a pixel. Only when
    // that end turns away from this pixel's side of the edge (otherwise the pixel is on the stair's flat part)
    const float nearEnd = distBack < distFwd ? endBack : endFwd;
    const bool centreBelow = lc - edgeLuma < 0.0;
    const float spanOffset = ((nearEnd < 0.0) != centreBelow) ? 0.5 - min(distBack, distFwd) / (distBack + distFwd) : 0.0;
    // a pixel unlike all its neighbours (a thin line's lone bit): softened towards them
    const float around = (2.0 * (ln + ls + lw + le) + l[0] + l[2] + l[6] + l[8]) / 12.0;
    float lone = saturate(abs(around - lc) / range);
    lone = (-2.0 * lone + 3.0) * lone * lone;
    const float offset = max(spanOffset, 0.75 * lone * lone) * saturate(strength);
    uOut[p] = float4(tIn.SampleLevel(sLinear, uv + across * (offset * dir), 0).rgb, c.a);
}
)";
struct EdgeConstants { uint32_t w, h; float strength; uint32_t hdr; };

// An HDR frame, which comes as light (1 = the SDR white) for the upscaler's HDR mode, in its SDR view: what the motion estimate measures
const char* kViewHlsl = R"(
Texture2D<float4> tIn : register(t0);
RWTexture2D<float4> uOut : register(u0);
cbuffer C : register(b0) { uint2 size; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float4 c = tIn[id.xy];
    uOut[id.xy] = float4(LightToSdr(c.rgb), c.a);
}
)";
struct ViewConstants { uint32_t w, h, unused[2]; };


const char* ResultName(NVSDK_NGX_Result r) {
    switch (r) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound (nvngx_dlss.dll missing?)";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate (update the NVIDIA driver)";
    default: return "error";
    }
}

// AMD's runtime reports problems through a callback without a user pointer: the engine that loaded it sets where they go.
std::function<void(const char*)> g_ffxLog;
void FfxMessage(uint32_t type, const wchar_t* message) {
    if (!g_ffxLog || !message) return;
    char text[512]; WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof text, nullptr, nullptr);
    char line[560]; snprintf(line, sizeof line, "FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
    g_ffxLog(line);
}

// NGX's log (the reasons it gives when DLSS does not start) into the addon's log: errors and warnings only, as Neural Rendering does.
std::function<void(const char*)> g_ngxLog;
void NVSDK_CONV NgxMessage(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!g_ngxLog || !message) return;
    std::string line(message);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.find("error") == std::string::npos && line.find("warning") == std::string::npos) return;
    if (line.find("NGXLoadConfig") != std::string::npos || line.find("NGXCore") != std::string::npos) return;   // noise on every start
    g_ngxLog(("[ngx] " + line).c_str());
}

} // namespace

bool SrEngine::HasFeature() const {
    return m_backend == Backend::Fsr ? (m_ffx && m_ffx->context) : m_backend == Backend::Xess ? (m_xess && m_xess->initialised) : m_feature != nullptr;
}

void SrEngine::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    m_log(text);
}

void SrEngine::Fail(const char* fmt, ...) {
    char text[256]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    { std::lock_guard<std::mutex> lock(m_errorMutex); m_error = text; }
    m_failed = true; m_ready = false;
    Log("%s upscaler FAILED: %s", Name(), text);
}

// ---- starting and stopping

bool SrEngine::Init(const LUID& card, const std::wstring& dataPath, const std::wstring& runtimeDir, LogFn log, Backend backend) {
    if (m_abandoned) { m_failed = true; return false; }   // its thread is still stuck in a runtime: only a restart of Lossless Scaling helps
    m_log = std::move(log); m_failed = false; { std::lock_guard<std::mutex> lock(m_errorMutex); m_error.clear(); } m_backend = backend;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { Fail("CreateDXGIFactory1"); return false; }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !adapter; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc; a->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == card.LowPart && desc.AdapterLuid.HighPart == card.HighPart) adapter = a; else a->Release();
    }
    factory->Release();
    if (!adapter) { Fail("no graphics card with LUID %08x:%08x", card.HighPart, card.LowPart); return false; }
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev));
    adapter->Release();
    if (FAILED(hr)) { Fail("D3D12CreateDevice 0x%08x", (unsigned)hr); return false; }

    // High priority: its picture is what goes on the screen, so it goes ahead of the game's own rendering on a busy GPU instead of finishing
    // after the next frame is due (in Fallout: New Vegas a normal queue left about one frame in six to repeat, 2026-09-24).
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(m_dev->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_queue)))) {
        queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(m_dev->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_queue)))) { Fail("CreateCommandQueue"); return false; }
        Log("%s upscaler: a high-priority queue was refused; a normal one runs", Name());
    }
    for (auto*& a : m_alloc) if (FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { Fail("CreateCommandAllocator"); return false; }
    if (FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0], nullptr, IID_PPV_ARGS(&m_list)))) { Fail("CreateCommandList"); return false; }
    m_list->Close();
    if (FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))) || !(m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr))) { Fail("CreateFence"); return false; }
    D3D12_QUERY_HEAP_DESC queries{}; queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = 4 * kSlots;   // start, motion done, upscaler done, end
    m_dev->CreateQueryHeap(&queries, IID_PPV_ARGS(&m_timestamps));
    D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 32 * kSlots; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    m_dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_timestampReadback));
    m_queue->GetTimestampFrequency(&m_timestampFreq);

    // the motion pass: t0 the flow, u0 the motion vectors, four constants, a linear sampler
    D3D12_DESCRIPTOR_RANGE srv{}; srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE uav{}; uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = { 1, &srv };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &uav };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants.Num32BitValues = 8;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    const D3D12_ROOT_SIGNATURE_DESC rootDesc{ 3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(m_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig)))) {
        SafeRelease(blob); SafeRelease(error); Fail("the motion pass's root signature"); return false;
    }
    SafeRelease(blob);
    auto build = [&](const char* source, const char* name, ID3D12PipelineState** out) {
        ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
        const std::string text = std::string(NR_HDR_HLSL) + source;   // the HDR curve (hdr_hlsl.h) in front of every pass
        if (FAILED(D3DCompile(text.c_str(), text.size(), name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            Log("%s: %s", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); SafeRelease(err); Fail("the %s shader did not compile", name); return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_rootSig; pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        const HRESULT h = m_dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(out));
        SafeRelease(code);
        if (FAILED(h)) { Fail("the %s pipeline 0x%08x", name, (unsigned)h); return false; }
        return true;
    };
    if (!build(kMotionHlsl, "sr_motion", &m_motionPso) || !build(kSharpenHlsl, "sr_sharpen", &m_sharpenPso) || !build(kEdgesHlsl, "sr_edges", &m_edgesPso) ||
        !build(kViewHlsl, "sr_view", &m_viewPso)) return false;
    if (!InitLean()) Log("%s upscaler: the lean pass could not start; DLSS runs without it", Name());
    static_assert(FlowEstimator::kSlots == kSlots, "the estimator reads its statistics back per engine slot");
    if (!m_estimator.Init(m_dev, [this](const char* m) { Log("%s", m); })) Log("%s upscaler: the motion estimator could not start; motion comes from frame generation only", Name());
    m_estimator.SetTimestampFrequency(m_timestampFreq);
    // per allocator slot: the flow's view, the motion vectors', the sharpening pass's input and output, the edge pass's input and output
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = kDescriptors * kSlots; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_dev->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)))) { Fail("CreateDescriptorHeap"); return false; }
    m_descriptorSize = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (m_backend == Backend::Fsr) {   // AMD's runtime from the addon's fsr folder (signed by AMD; loaded by its full path only)
        m_ffx = new FfxState;
        const std::wstring path = runtimeDir + L"\\amd_fidelityfx_dx12.dll";
        m_ffx->module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_ffx->module) { Fail("AMD's FSR runtime could not be loaded from %ls (error %lu)", path.c_str(), GetLastError()); return false; }
        ffxLoadFunctions(&m_ffx->fn, m_ffx->module);
        if (!m_ffx->fn.CreateContext || !m_ffx->fn.DestroyContext || !m_ffx->fn.Dispatch) { Fail("AMD's FSR runtime lacks the FidelityFX API"); return false; }
        g_ffxLog = [this](const char* m) { Log("%s", m); };
        StartWorker();
        m_ready = true;
        Log("FSR upscaler ready on its own D3D12 device (runtime from %ls)", runtimeDir.c_str());
        return true;
    }

    if (m_backend == Backend::Xess) {   // Intel's runtime from the addon's xess folder (signed by Intel; loaded by its full path only)
#if NR_HAVE_XESS
        m_xess = new XessState;
        const std::wstring path = runtimeDir + L"\\libxess.dll";
        m_xess->module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m_xess->module) { Fail("Intel's XeSS runtime could not be loaded from %ls (error %lu)", path.c_str(), GetLastError()); return false; }
        auto get = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m_xess->module, name)); return fn != nullptr; };
        if (!get(m_xess->GetVersion, "xessGetVersion") || !get(m_xess->CreateContext, "xessD3D12CreateContext") || !get(m_xess->OptimalInput, "xessGetOptimalInputResolution") ||
            !get(m_xess->Init, "xessD3D12Init") || !get(m_xess->Execute, "xessD3D12Execute") || !get(m_xess->DestroyContext, "xessDestroyContext") ||
            !get(m_xess->SetMaxResponsive, "xessSetMaxResponsiveMaskValue")) {
            Fail("Intel's XeSS runtime lacks the XeSS API"); return false;
        }
        get(m_xess->SetLogging, "xessSetLoggingCallback");
        m_xess->GetVersion(&m_xess->version);
        const xess_result_t rc = m_xess->CreateContext(m_dev, &m_xess->context);
        if (rc != XESS_RESULT_SUCCESS || !m_xess->context) { m_xess->context = nullptr; Fail("XeSS could not make its context (code %d)", static_cast<int>(rc)); return false; }
        {
            char v[32]; snprintf(v, sizeof v, "%u.%u.%u", m_xess->version.major, m_xess->version.minor, m_xess->version.patch);
            std::lock_guard<std::mutex> lock(m_providerMutex); m_provider = v;
        }
        StartWorker();
        m_ready = true;
        Log("XeSS upscaler ready on its own D3D12 device (XeSS %u.%u.%u, runtime from %ls)", m_xess->version.major, m_xess->version.minor, m_xess->version.patch, runtimeDir.c_str());
        return true;
#else
        Fail("this build has no XeSS (Intel's SDK was not there when it was built)"); return false;
#endif
    }

    // NGX, with NVIDIA's runtime from the addon's dlss folder
    m_ngxDataPath = dataPath; m_ngxRuntimeDir = runtimeDir; m_ngxRestarts = 0; m_ngxLost = false;
    // (the search list is the same in every addon of ours: NGX keeps the paths of its first Init in the process, see ngx_paths.h)
    nr::ngxpaths::PublishDlssRuntime(runtimeDir);
    const std::vector<std::wstring> searchList = nr::ngxpaths::SearchList({ runtimeDir });
    const std::vector<const wchar_t*> paths = nr::ngxpaths::AsArray(searchList);
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path = paths.data(); info.PathListInfo.Length = static_cast<unsigned>(paths.size());
    g_ngxLog = [this](const char* m) { Log("%s", m); };
    info.LoggingInfo.LoggingCallback = NgxMessage; info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = false;
    const std::wstring runtime = runtimeDir + L"\\nvngx_dlss.dll";
    if (GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES)
        Log("DLSS upscaler: NVIDIA's runtime is not at %ls (error %lu); NGX will look for one elsewhere", runtime.c_str(), GetLastError());
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(kAppId, dataPath.c_str(), m_dev, &info);
    if (NVSDK_NGX_FAILED(r)) { Fail("NGX Init: %s", ResultName(r)); return false; }
    nr::ngxusers::Joined(); m_ngxJoined = true;
    NVSDK_NGX_Parameter* p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&p)) || !p) { Fail("NGX parameters"); return false; }
    m_params = p;
    // Available, or why not: NGX says whether the driver is too old (and which it needs) and what went wrong when it set the feature up
    // (the runtime not found or not accepted, for example); both go into the message, so a report says more than "not available".
    int available = 0, needsDriver = 0, initResult = 0;
    unsigned minMajor = 0, minMinor = 0;
    NVSDK_NGX_Parameter* caps = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps) {
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        NVSDK_NGX_D3D12_DestroyParameters(caps);
    }
    if (!available) {
        if (needsDriver) Fail("DLSS needs a newer NVIDIA driver (%u.%u or later)", minMajor, minMinor);
        else Fail("DLSS Super Resolution did not start: %s, 0x%08x (NVIDIA's runtime: %ls)", ResultName(static_cast<NVSDK_NGX_Result>(initResult)), static_cast<unsigned>(initResult), runtime.c_str());
        return false;
    }
    StartWorker();
    m_ready = true;
    Log("DLSS upscaler ready on its own D3D12 device (runtime from %ls)", runtimeDir.c_str());
    return true;
}

bool SrEngine::Shutdown() {
    m_ready = false;
    if (!StopWorker(kIdleWaitMs)) {   // its thread is inside a runtime and does not come back: nothing it may still use is torn down
        m_abandoned = true; m_failed = true;
        { std::lock_guard<std::mutex> lock(m_errorMutex); m_error = std::string(Name()) + " stopped responding inside its runtime; restart Lossless Scaling to use it again"; }
        Log("%s upscaler: the engine's thread did not stop within %lu ms (stuck in the runtime); the engine is left as it is until Lossless Scaling closes", Name(), kIdleWaitMs);
        return false;
    }
    if (m_queue && !WaitIdle()) {
        m_ready = false; m_failed = true;
        { std::lock_guard<std::mutex> lock(m_errorMutex); m_error = "the GPU did not finish the upscaler's work; restart Lossless Scaling to use it again"; }
        Log("%s upscaler: the GPU did not finish within %lu ms; the engine is left as it is until Lossless Scaling closes", Name(), kIdleWaitMs);
        return false;
    }
    if (m_ffx) {
        if (m_ffx->context) m_ffx->fn.DestroyContext(&m_ffx->context, nullptr);
        if (m_ffx->module) FreeLibrary(m_ffx->module);
        delete m_ffx; m_ffx = nullptr;
        g_ffxLog = nullptr;
    }
    if (m_xess) {
#if NR_HAVE_XESS
        if (m_xess->context) m_xess->DestroyContext(m_xess->context);
#endif
        if (m_xess->module) FreeLibrary(m_xess->module);
        delete m_xess; m_xess = nullptr;
    }
    g_ngxLog = nullptr;
    if (m_feature) { NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_feature)); m_feature = nullptr; }
    if (m_params) { NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(m_params)); m_params = nullptr; }
    if (m_ngxJoined) { m_ngxJoined = false; if (nr::ngxusers::Leaving() && m_dev) NVSDK_NGX_D3D12_Shutdown1(m_dev); else Log("DLSS upscaler: NGX left running (another addon still uses it)"); }
    m_estimator.Shutdown(); m_estimatedLast = false; m_estimates = 0;
    SafeRelease(m_motion); SafeRelease(m_distrust); SafeRelease(m_depth); SafeRelease(m_depthUpload);
    SafeRelease(m_leanPso); SafeRelease(m_leanRoot); SafeRelease(m_leaned); m_leanedW = m_leanedH = 0;
    SafeRelease(m_steadyPso); SafeRelease(m_steadyRoot); for (auto*& t : m_sharpHist) SafeRelease(t); m_sharpHistW = m_sharpHistH = 0; m_sharpHistValid = false;
    SafeRelease(m_motionPso); SafeRelease(m_sharpenPso); SafeRelease(m_edgesPso); SafeRelease(m_viewPso); SafeRelease(m_view); m_viewW = m_viewH = 0; SafeRelease(m_smoothed); m_smoothedW = m_smoothedH = 0; SafeRelease(m_unsharpened); m_unsharpenedW = m_unsharpenedH = 0; SafeRelease(m_rootSig); SafeRelease(m_heap);
    SafeRelease(m_timestamps); SafeRelease(m_timestampReadback);
    SafeRelease(m_list); for (auto*& a : m_alloc) SafeRelease(a);
    SafeRelease(m_fence); if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    SafeRelease(m_queue); SafeRelease(m_dev);
    for (auto& v : m_slotDone) v = 0;
    m_fenceValue = 0; m_nextSlot = 0; m_inW = m_inH = m_outW = m_outH = 0; m_preset = ~0u; m_ready = false;
    return true;
}

ID3D12Resource* SrEngine::OpenSharedTexture(HANDLE h) {
    ID3D12Resource* r = nullptr;
    if (FAILED(m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&r)))) Log("%s upscaler: a shared texture could not be opened", Name());
    return r;
}
ID3D12Fence* SrEngine::OpenSharedFence(HANDLE h) {
    ID3D12Fence* f = nullptr;
    if (FAILED(m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&f)))) Log("%s upscaler: a shared fence could not be opened", Name());
    return f;
}

bool SrEngine::WaitIdle() {
    if (!m_queue || !m_fence) return true;
    m_queue->Signal(m_fence, ++m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) { m_fence->SetEventOnCompletion(m_fenceValue, m_event); WaitForSingleObject(m_event, kIdleWaitMs); }
    return m_fence->GetCompletedValue() >= m_fenceValue;
}
// The engine's thread and its queue idle (before shared textures or fences go away). Not while its thread is stuck in a runtime: then the
// queue is not touched from here (it is that thread's), and whatever is released may never be used again anyway.
void SrEngine::Drain() {
    if (!WaitWorkerIdle(kIdleWaitMs)) { Log("%s upscaler: the engine's thread is still busy; not waiting for it", Name()); return; }
    WaitIdle();
}

void SrEngine::StartWorker() {
    if (m_worker.joinable()) return;
    { std::lock_guard<std::mutex> lock(m_jobMutex); m_stop = false; m_busy = false; m_jobs.clear(); }
    m_submitted = 0; m_busySince = 0; m_stuck = false;
    for (auto& v : m_okRing) v = 0;
    if (!m_workerExited) m_workerExited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(m_workerExited);
    m_worker = std::thread([this] { WorkerLoop(); });
}

bool SrEngine::StopWorker(DWORD ms) {
    if (!m_worker.joinable()) return true;
    { std::lock_guard<std::mutex> lock(m_jobMutex); m_stop = true; }
    m_jobCv.notify_all();
    if (WaitForSingleObject(m_workerExited, ms) != WAIT_OBJECT_0) { m_worker.detach(); return false; }
    m_worker.join();
    return true;
}

bool SrEngine::WaitWorkerIdle(DWORD ms) {
    if (m_stuck) return false;
    std::unique_lock<std::mutex> lock(m_jobMutex);
    return m_jobCv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return m_jobs.empty() && !m_busy; });
}

bool SrEngine::WaitSubmitted(uint64_t doneValue, DWORD ms) {
    std::unique_lock<std::mutex> lock(m_jobMutex);
    return m_jobCv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return m_submitted.load() >= doneValue || m_stuck.load(); }) && !m_stuck;
}

void SrEngine::Submit(const Job& j) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_worker.joinable() && !m_stop && !m_stuck) { m_jobs.push_back(j); m_jobCv.notify_all(); return; }
    }
    if (m_queue && j.done && !m_stuck) m_queue->Signal(j.done, j.doneValue);   // no thread to run it: "done" still moves on (nothing ran)
}

void SrEngine::WorkerLoop() {
    // on the frame path now (a frame waits for this thread to record it), in short bursts: ahead of a busy game's threads, so the scheduler
    // does not hold it up by milliseconds when every core is loaded
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"LS Addon engine");
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
            if (m_stop) {   // the jobs left are marked done, so nothing waits for them
                for (const Job& left : m_jobs) if (m_queue && left.done) m_queue->Signal(left.done, left.doneValue);
                m_jobs.clear();
                break;
            }
            j = m_jobs.front(); m_jobs.pop_front(); m_busy = true;
        }
        m_busySince = GetTickCount64();
        // marked as going through before it is submitted (the GPU may finish it before this thread gets back), taken back if it did not
        m_okRing[j.doneValue % kOkRing].store(j.doneValue, std::memory_order_release);
        const bool ok = Run(j.in, j.inW, j.inH, j.inFormat, j.out, j.outW, j.outH, j.outFormat, j.flow, j.flowW, j.flowH, j.flowUnit, j.motionFraction,
                            j.estimate, j.preset, j.sharpen, j.reset, j.hdr, j.copied, j.copiedValue, j.done, j.doneValue);
        m_busySince = 0;
        m_okRing[j.doneValue % kOkRing].store(ok ? j.doneValue : 0, std::memory_order_release);
        m_submitted.store(j.doneValue, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(m_jobMutex); m_busy = false; }
        m_jobCv.notify_all();
    }
    SetEvent(m_workerExited);
}

bool SrEngine::CheckStuck() {
    if (m_stuck) return true;
    const ULONGLONG since = m_busySince.load();
    if (!since || GetTickCount64() - since < kStuckMs) return false;
    m_stuck = true; m_ready = false; m_failed = true;
    const std::string provider = Provider();
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = std::string(m_backend == Backend::Fsr ? "AMD's FSR runtime" : m_backend == Backend::Xess ? "Intel's XeSS runtime" : "NVIDIA's DLSS runtime") + (provider.empty() ? "" : " (" + provider + ")") +
                  " stopped responding on this frame format; choose another runtime in the Runtimes list, then restart Lossless Scaling";
    }
    m_jobCv.notify_all();
    Log("%s upscaler: a frame has been in the runtime for %llu ms: the engine stopped responding; NIS runs instead", Name(), (unsigned long long)(GetTickCount64() - since));
    return true;
}

void SrEngine::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    m_list->ResourceBarrier(1, &b);
}

int SrEngine::TakeSlot() {
    const int slot = m_nextSlot;
    if (m_slotDone[slot] && m_fence->GetCompletedValue() < m_slotDone[slot]) {
        m_fence->SetEventOnCompletion(m_slotDone[slot], m_event);
        WaitForSingleObject(m_event, kSlotWaitMs);
        if (m_fence->GetCompletedValue() < m_slotDone[slot]) return -1;   // still on the GPU: this frame is left to NIS
    }
    m_nextSlot = (slot + 1) % kSlots;
    m_alloc[slot]->Reset();
    return slot;
}

void SrEngine::ReadTime(int slot) {
    if (!m_slotDone[slot] || !m_timestampReadback) return;
    const D3D12_RANGE range{ static_cast<SIZE_T>(slot) * 32, static_cast<SIZE_T>(slot) * 32 + 32 };
    uint64_t* t = nullptr;
    if (FAILED(m_timestampReadback->Map(0, &range, reinterpret_cast<void**>(&t)))) return;
    const uint64_t t0 = t[slot * 4], t1 = t[slot * 4 + 1], t2 = t[slot * 4 + 2], t3 = t[slot * 4 + 3];
    const D3D12_RANGE none{ 0, 0 };
    m_timestampReadback->Unmap(0, &none);
    auto smooth = [](double& avg, double ms) { avg = avg == 0 ? ms : avg * 0.9 + ms * 0.1; };
    if (!m_timestampFreq || !(t0 <= t1 && t1 <= t2 && t2 <= t3) || t3 == t0) return;
    const double ms = 1000.0 / m_timestampFreq;
    smooth(m_gpuMs, (t3 - t0) * ms); smooth(m_motionMs, (t1 - t0) * ms); smooth(m_afterMs, (t3 - t2) * ms);
}

// ---- the feature and its inputs (on the caller's thread; our own device only)

bool SrEngine::EnsureInputs(uint32_t w, uint32_t h) {
    if (m_motion && m_inW == w && m_inH == h) return true;
    SafeRelease(m_motion); SafeRelease(m_distrust); SafeRelease(m_depth); SafeRelease(m_depthUpload);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_motion)))) return false;
    d.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_distrust)))) return false;
    d.Format = DXGI_FORMAT_R32_FLOAT; d.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_depth)))) return false;
    // flat depth (DLSS reads it; Lossless Scaling has none), uploaded once
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    m_dev->GetCopyableFootprints(&d, 0, 1, 0, &layout, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES upload{}; upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf{}; buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buf.Width = total; buf.Height = 1; buf.DepthOrArraySize = 1; buf.MipLevels = 1;
    buf.SampleDesc.Count = 1; buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_dev->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_depthUpload)))) return false;
    uint8_t* mapped = nullptr;
    m_depthUpload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    const std::vector<float> row(w, 0.5f);
    for (UINT y = 0; y < rows; ++y) memcpy(mapped + layout.Offset + y * layout.Footprint.RowPitch, row.data(), w * 4);
    m_depthUpload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = m_depth; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = m_depthUpload; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = layout;
    m_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Transition(m_depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

bool SrEngine::InitLean() {   // its own root signature: three pictures in, one out, five constants, a linear sampler
    D3D12_DESCRIPTOR_RANGE srv{}; srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv.NumDescriptors = 3;
    D3D12_DESCRIPTOR_RANGE uav{}; uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = { 1, &srv };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &uav };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants.Num32BitValues = 12;   // the lean's seven, the steady sharpening's ten
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    const D3D12_ROOT_SIGNATURE_DESC rootDesc{ 3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    const bool rootOk = SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) &&
                        SUCCEEDED(m_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_leanRoot)));
    SafeRelease(blob); SafeRelease(error);
    if (!rootOk) return false;
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kLeanHlsl, strlen(kLeanHlsl), "sr_lean", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        Log("sr_lean: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); SafeRelease(err); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_leanRoot; pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    const HRESULT hr = m_dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_leanPso)); SafeRelease(code);
    if (FAILED(hr)) return false;
    // steady sharpening (an option: without it the sharpening pass is the plain one): its own root signature, three pictures in and two out
    {
        D3D12_DESCRIPTOR_RANGE srv3{}; srv3.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv3.NumDescriptors = 3;
        D3D12_DESCRIPTOR_RANGE uav2{}; uav2.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav2.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER sp[3]{};
        sp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; sp[0].DescriptorTable = { 1, &srv3 };
        sp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; sp[1].DescriptorTable = { 1, &uav2 };
        sp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; sp[2].Constants.Num32BitValues = sizeof(SteadyConstants) / 4;
        for (auto& q : sp) q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        const D3D12_ROOT_SIGNATURE_DESC sd{ 3, sp, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ID3DBlob* sb = nullptr; ID3DBlob* se = nullptr;
        const bool ok = SUCCEEDED(D3D12SerializeRootSignature(&sd, D3D_ROOT_SIGNATURE_VERSION_1, &sb, &se)) &&
                        SUCCEEDED(m_dev->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&m_steadyRoot)));
        SafeRelease(sb); SafeRelease(se);
        const std::string text = std::string(NR_HDR_HLSL) + kSteadySharpenHlsl;
        if (ok && SUCCEEDED(D3DCompile(text.c_str(), text.size(), "sr_steady_sharpen", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC sdesc{}; sdesc.pRootSignature = m_steadyRoot; sdesc.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            if (FAILED(m_dev->CreateComputePipelineState(&sdesc, IID_PPV_ARGS(&m_steadyPso)))) m_steadyPso = nullptr;
            SafeRelease(code);
        } else if (ok) {
            Log("sr_steady_sharpen: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); SafeRelease(err);
        }
    }
    return true;
}

// The previous frame's sharpening input (output size and format), at rest readable.
// The format of the steady sharpening's running average: R10G10B10A2 where the card can sample it and store to it, else RGBA16F (always possible).
static DXGI_FORMAT outFormatOf(ID3D12Device* dev) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT f{}; f.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f)) && (f.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (f.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) &&
        (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) return DXGI_FORMAT_R10G10B10A2_UNORM;
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool SrEngine::EnsureSharpHist(uint32_t w, uint32_t h, DXGI_FORMAT /*outFormat*/) {
    if (m_sharpHist[0] && m_sharpHistW == w && m_sharpHistH == h) return true;
    if (m_sharpHist[0]) { WaitIdle(); for (auto*& t : m_sharpHist) SafeRelease(t); }
    m_sharpHistValid = false; m_sharpHistCur = 0;
    // The average is kept in the SDR view (0..1), so 10 bits a channel in 4 bytes are enough (more precise than the 8 bits of an SDR picture, half the traffic of an
    // HDR one); a card that cannot store to that format keeps it in the picture's own.
    m_sharpHistFmt = outFormatOf(m_dev);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Format = m_sharpHistFmt; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // taken in turns: the one just written rests readable, the other rests writable
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_sharpHist[0])))) return false;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_sharpHist[1])))) { SafeRelease(m_sharpHist[0]); return false; }
    m_sharpHistW = w; m_sharpHistH = h;
    return true;
}

// The leaned picture, when edge smoothing or sharpening follows (output size and format), left in UNORDERED_ACCESS between runs.
bool SrEngine::EnsureLeanTarget(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (m_leaned && m_leanedW == w && m_leanedH == h && m_leanedFmt == fmt) return true;
    if (m_leaned) { WaitIdle(); SafeRelease(m_leaned); }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Format = fmt; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_leaned)))) return false;
    m_leanedW = w; m_leanedH = h; m_leanedFmt = fmt;
    return true;
}

// The edge-smoothed picture, when the sharpening pass follows (output size and format), left in UNORDERED_ACCESS between runs.
bool SrEngine::EnsureSmoothTarget(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (m_smoothed && m_smoothedW == w && m_smoothedH == h && m_smoothedFmt == fmt) return true;
    if (m_smoothed) { WaitIdle(); SafeRelease(m_smoothed); }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Format = fmt; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_smoothed)))) return false;
    m_smoothedW = w; m_smoothedH = h; m_smoothedFmt = fmt;
    return true;
}

bool SrEngine::EnsureSharpenTarget(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (m_unsharpened && m_unsharpenedW == w && m_unsharpenedH == h && m_unsharpenedFmt == fmt) return true;
    if (m_unsharpened) { WaitIdle(); SafeRelease(m_unsharpened); }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Format = fmt; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_unsharpened)))) return false;
    m_unsharpenedW = w; m_unsharpenedH = h; m_unsharpenedFmt = fmt;
    return true;
}

// An HDR frame's SDR view, the motion estimate's input (the game's size), left in UNORDERED_ACCESS between runs.
bool SrEngine::EnsureViewInput(uint32_t w, uint32_t h) {
    if (m_view && m_viewW == w && m_viewH == h) return true;
    if (m_view) { WaitIdle(); SafeRelease(m_view); }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_view)))) return false;
    m_viewW = w; m_viewH = h;
    return true;
}

// NGX again on this device, after its feature was lost under it. NGX is shared by everything in Lossless Scaling's process: when another
// user of it shuts down (the Neural Rendering addon, switched off while the DLSS Upscaler ran, 2026-09-27), this engine's feature was gone
// at its next evaluate ("FeatureNotFound") and the upscaler switched itself off. Now it starts NGX again and makes the feature anew.
bool SrEngine::RestartNgx() {
    WaitIdle();
    if (m_feature) { NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_feature)); m_feature = nullptr; }
    if (m_params) { NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(m_params)); m_params = nullptr; }
    if (nr::ngxusers::Users() <= 1) NVSDK_NGX_D3D12_Shutdown1(m_dev);   // (not while another of our engines uses NGX)
    nr::ngxpaths::PublishDlssRuntime(m_ngxRuntimeDir);
    const std::vector<std::wstring> searchList = nr::ngxpaths::SearchList({ m_ngxRuntimeDir });
    const std::vector<const wchar_t*> paths = nr::ngxpaths::AsArray(searchList);
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path = paths.data(); info.PathListInfo.Length = static_cast<unsigned>(paths.size());
    info.LoggingInfo.LoggingCallback = NgxMessage; info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON; info.LoggingInfo.DisableOtherLoggingSinks = false;
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(kAppId, m_ngxDataPath.c_str(), m_dev, &info);
    if (NVSDK_NGX_FAILED(r)) { Log("DLSS upscaler: NGX did not start again: %s", ResultName(r)); return false; }
    NVSDK_NGX_Parameter* p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&p)) || !p) { Log("DLSS upscaler: NGX gave no parameters after starting again"); return false; }
    m_params = p;
    m_inW = m_inH = m_outW = m_outH = 0;   // the feature is made anew at the next run
    Log("DLSS upscaler: NGX started again on its device (its feature had been lost: another NGX user in the process shut down?)");
    return true;
}

bool SrEngine::EnsureFeature(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, unsigned preset, bool hdr) {
    if (m_backend != Backend::Dlss) preset = 0;   // only DLSS has models to choose
    if (HasFeature() && inW == m_inW && inH == m_inH && outW == m_outW && outH == m_outH && preset == m_preset && hdr == m_hdr) return true;
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    if (!WaitIdle()) { Fail("the GPU did not finish the earlier work"); return false; }
    for (auto& v : m_slotDone) v = 0;
    if (m_feature) { NVSDK_NGX_D3D12_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(m_feature)); m_feature = nullptr; }
    if (m_ffx && m_ffx->context) { m_ffx->fn.DestroyContext(&m_ffx->context, nullptr); m_ffx->context = nullptr; }
    m_alloc[0]->Reset();
    m_list->Reset(m_alloc[0], nullptr);
    if (!EnsureInputs(inW, inH)) { m_list->Close(); Fail("the motion-vector and depth textures could not be made"); return false; }
    const float ratio = std::max(static_cast<float>(outW) / inW, static_cast<float>(outH) / inH);
#if NR_HAVE_XESS
    if (m_backend == Backend::Xess) {   // the depth upload runs first; XeSS is set up on its context, without a command list
        m_list->Close();
        ID3D12CommandList* upload[] = { m_list };
        m_queue->ExecuteCommandLists(1, upload);
        WaitIdle();
        SafeRelease(m_depthUpload);
        m_xess->initialised = false;
        // XeSS's quality settings are fixed ratios, each with a range of input sizes it takes: the first (finest) whose range holds the game's size
        const xess_2d_t outRes{ outW, outH };
        static const xess_quality_settings_t kQualities[] = { XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, XESS_QUALITY_SETTING_ULTRA_QUALITY,
            XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE, XESS_QUALITY_SETTING_ULTRA_PERFORMANCE };
        xess_quality_settings_t quality = XESS_QUALITY_SETTING_ULTRA_PERFORMANCE; bool fits = false;
        for (const xess_quality_settings_t q : kQualities) {
            xess_2d_t optimal{}, lo{}, hi{};
            if (m_xess->OptimalInput(m_xess->context, &outRes, q, &optimal, &lo, &hi) != XESS_RESULT_SUCCESS) continue;
            if (inW >= lo.x && inW <= hi.x && inH >= lo.y && inH <= hi.y) { quality = q; fits = true; break; }
        }
        if (!fits) { Fail("XeSS takes no input of %ux%u for an output of %ux%u", inW, inH, outW, outH); return false; }
        xess_d3d12_init_params_t ip{}; ip.outputResolution = outRes; ip.qualitySetting = quality;
        // motion at the game's size with the (flat) depth; SDR frames as they are shown, HDR frames as light (1 = the SDR white)
        ip.initFlags = (hdr ? XESS_INIT_FLAG_NONE : XESS_INIT_FLAG_LDR_INPUT_COLOR) | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;   // the distrust mask, as FSR's reactive one
        ip.creationNodeMask = 1; ip.visibleNodeMask = 1;
        const xess_result_t rc = m_xess->Init(m_xess->context, &ip);
        QueryPerformanceCounter(&b);
        if (rc != XESS_RESULT_SUCCESS) { Fail("XeSS could not be set up (code %d)", static_cast<int>(rc)); return false; }
        m_xess->initialised = true;
        m_inW = inW; m_inH = inH; m_outW = outW; m_outH = outH; m_preset = preset; m_hdr = hdr;
        m_buildMs = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        static const char* const kNames[] = { "ultra performance", "performance", "balanced", "quality", "ultra quality", "ultra quality plus", "anti-aliasing" };
        Log("XeSS upscaler: %ux%u -> %ux%u (x%.2f), %s%s, made in %.0f ms", inW, inH, outW, outH, ratio, kNames[quality - XESS_QUALITY_SETTING_ULTRA_PERFORMANCE],
            hdr ? ", HDR" : "", m_buildMs);
        return true;
    }
#endif
    if (m_backend == Backend::Fsr) {   // the depth upload runs first; FSR's context needs no command list
        m_list->Close();
        ID3D12CommandList* upload[] = { m_list };
        m_queue->ExecuteCommandLists(1, upload);
        WaitIdle();
        SafeRelease(m_depthUpload);
        ffxCreateBackendDX12Desc backendDesc{}; backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backendDesc.device = m_dev;
        // SDK 2.x's runtimes (FSR 4) ask for the API version the program was built against; an older runtime passes over it. Its type and
        // value are FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION and FFX_UPSCALER_VERSION 4.1.1, from AMD's ffx_upscale.h of SDK 2.3.
        struct VersionDesc { ffxCreateContextDescHeader header; uint32_t version; } versionDesc{};
        versionDesc.header.type = 0x0001000bu; versionDesc.version = (4u << 22) | (1u << 12) | 1u; versionDesc.header.pNext = &backendDesc.header;
        ffxCreateContextDescUpscale desc{}; desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; desc.header.pNext = &versionDesc.header;
        // gamma-encoded colour (the frame as the game shows it: AMD asks for this flag with the dispatch's sRGB flag, FSR 4 above all);
        // motion at the game's size; no jitter, no inverted or infinite depth. Auto exposure and AMD's own tuning of OptiScaler's were tried
        // (test host, 2026-09-26): no better, and OptiScaler's values put a swaying wire 10.4 levels off against our 8.8.
        // HDR frames: light (1 = the SDR white) in FSR's HDR mode, which keeps highlights far above the SDR white as they were
        desc.flags = hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        desc.maxRenderSize = { inW, inH }; desc.maxUpscaleSize = { outW, outH }; desc.fpMessage = FfxMessage;
        const ffxReturnCode_t rc = m_ffx->fn.CreateContext(&m_ffx->context, &desc.header, nullptr);
        QueryPerformanceCounter(&b);
        if (rc != FFX_API_RETURN_OK || !m_ffx->context) { m_ffx->context = nullptr; Fail("FSR could not make its upscaling context (code %u)", rc); return false; }
        m_inW = inW; m_inH = inH; m_outW = outW; m_outH = outH; m_preset = preset; m_hdr = hdr;
        m_ffxStability = -1.0f;   // a new context has AMD's defaults
        m_buildMs = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        Log("FSR upscaler: %ux%u -> %ux%u (x%.2f)%s, made in %.0f ms", inW, inH, outW, outH, ratio, hdr ? ", HDR" : "", m_buildMs);
        // which upscaler the runtime chose (a newer runtime can hold FSR 4 as well as FSR 3), and what else it holds
        {
            ffxQueryGetProviderVersion used{}; used.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            const bool known = m_ffx->fn.Query && m_ffx->fn.Query(&m_ffx->context, &used.header) == FFX_API_RETURN_OK && used.versionName;
            std::string offered;
            if (m_ffx->fn.Query) {
                uint64_t count = 8; uint64_t ids[8] = {}; const char* names[8] = {};
                ffxQueryDescGetVersions all{}; all.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS; all.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                all.device = m_dev; all.outputCount = &count; all.versionIds = ids; all.versionNames = names;
                if (m_ffx->fn.Query(nullptr, &all.header) == FFX_API_RETURN_OK)
                    for (uint64_t i = 0; i < count && i < 8; ++i) if (names[i]) offered += (offered.empty() ? "" : ", ") + std::string(names[i]);
            }
            { std::lock_guard<std::mutex> lock(m_providerMutex); m_provider = known ? used.versionName : ""; }
            Log("FSR upscaler: the runtime runs %s%s%s%s", known ? used.versionName : "(it does not say which version)", offered.empty() ? "" : " (it holds: ",
                offered.c_str(), offered.empty() ? "" : ")");
        }
        return true;
    }
    auto* p = static_cast<NVSDK_NGX_Parameter*>(m_params);
    const NVSDK_NGX_PerfQuality_Value quality = ratio <= 1.01f ? NVSDK_NGX_PerfQuality_Value_DLAA   // the game at the screen's size: anti-aliasing only
                                              : ratio <= 1.55f ? NVSDK_NGX_PerfQuality_Value_MaxQuality : ratio <= 1.75f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                              : ratio <= 2.2f ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u); p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, inW); p->Set(NVSDK_NGX_Parameter_Height, inH);
    p->Set(NVSDK_NGX_Parameter_OutWidth, outW); p->Set(NVSDK_NGX_Parameter_OutHeight, outH);
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(quality));
    // motion at the game's size; HDR frames as light (1 = the SDR white), in DLSS's HDR mode
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | (hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0)));
    p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    for (const char* key : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality })
        p->Set(key, preset);
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_CreateFeature(m_list, NVSDK_NGX_Feature_SuperSampling, p, &handle);
    m_list->Close();
    ID3D12CommandList* lists[] = { m_list };
    m_queue->ExecuteCommandLists(1, lists);
    WaitIdle();
    SafeRelease(m_depthUpload);
    QueryPerformanceCounter(&b);
    if (NVSDK_NGX_FAILED(r) || !handle) { Fail("CreateFeature(DLSS): %s", ResultName(r)); return false; }
    m_feature = handle; m_inW = inW; m_inH = inH; m_outW = outW; m_outH = outH; m_preset = preset; m_hdr = hdr;
    m_buildMs = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    Log("%s upscaler: %ux%u -> %ux%u (x%.2f), preset %u%s, made in %.0f ms", Name(), inW, inH, outW, outH, ratio, preset, hdr ? ", HDR" : "", m_buildMs);
    return true;
}

// ---- one frame

// FSR 3.1's tuning keys (AMD's defaults at 0): at 1 it keeps more history where the reactive mask or a disocclusion would cut it, lets a
// thin object that keeps uncovering its background build up history sooner, reacts less to small changes of shading and to motion.
void SrEngine::ConfigureFsrStability(float s) {
    if (!m_ffx || !m_ffx->context || !m_ffx->fn.Configure || s == m_ffxStability) return;
    const struct { uint64_t key; float value; const char* name; } keys[] = {
        { FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, 1.0f - 0.5f * s, "velocity factor" },
        { FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE, 1.0f - 0.75f * s, "shading change" },
        { FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME, 0.333f + 0.3f * s, "accumulation per frame" },
        { FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION, -0.333f + 0.6f * s, "disocclusion accumulation" },
    };
    bool ok = true;
    // (FSR 4 takes none of these: it keeps its history its own way. The same scenes scored the same at any stability, 2026-09-26.)
    for (const auto& k : keys) {
        float value = k.value;
        ffxConfigureDescUpscaleKeyValue d{}; d.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE; d.key = k.key; d.ptr = &value;
        if (m_ffx->fn.Configure(&m_ffx->context, &d.header) != FFX_API_RETURN_OK) ok = false;
    }
    m_ffxStability = s;
    if (std::abs(s - m_loggedStability) >= 0.05f || (s == 0.0f) != (m_loggedStability == 0.0f)) {   // not every step of a slider being dragged
        m_loggedStability = s;
        Log("FSR upscaler: stability %.2f: velocity factor %.2f, shading change %.2f, accumulation per frame %.3f, disocclusion accumulation %.3f%s", s,
            keys[0].value, keys[1].value, keys[2].value, keys[3].value, ok ? "" : " (FSR refused some of them)");
    }
}

bool SrEngine::Run(ID3D12Resource* in, uint32_t inW, uint32_t inH, DXGI_FORMAT inFormat, ID3D12Resource* out, uint32_t outW, uint32_t outH, DXGI_FORMAT outFormat,
                   ID3D12Resource* flow, uint32_t flowW, uint32_t flowH, float flowUnit, float motionFraction, bool estimate, unsigned preset, float sharpen, bool reset,
                   bool hdr, ID3D12Fence* copied, uint64_t copiedValue, ID3D12Fence* done, uint64_t doneValue) {
    if (!m_ready) { if (m_queue && done) m_queue->Signal(done, doneValue); return false; }
    // A frame that cannot run is still marked done, on this queue after the frames before it, so "done" only ever moves forward.
    const auto skip = [&] { if (m_queue && done) m_queue->Signal(done, doneValue); return false; };
    const bool fsr = m_backend == Backend::Fsr, dlss = m_backend == Backend::Dlss;
    if (dlss && m_ngxLost) {   // its feature was lost at the last evaluate: NGX again (at most three times, then the upscaler stops)
        m_ngxLost = false;
        if (++m_ngxRestarts > 3 || !RestartNgx()) { Fail("DLSS lost its feature and NGX could not start again"); return skip(); }
    }
    if (preset == kPresetAuto) preset = inW == outW && inH == outH ? 5u : 12u;   // E at 1:1 (L pulses every fourth frame there), L when upscaling
    const bool fresh =!HasFeature() || inW != m_inW || inH != m_inH || outW != m_outW || outH != m_outH || (dlss && preset != m_preset) || hdr != m_hdr;
    if (!EnsureFeature(inW, inH, outW, outH, preset, hdr)) return skip();
    if (hdr && !EnsureViewInput(inW, inH)) { Fail("the HDR frame's SDR view could not be made"); return skip(); }
    ID3D12Resource* const color = in;   // what the upscaler reads (HDR: light)
    // Sharpening (strength 0..1.6, see kScalerSharpenScale): DLSS has none of its own, so the upscaler writes into a texture of ours and the
    // sharpening pass goes from there into out. FSR sharpens by itself (its RCAS) up to 1; beyond that our pass adds the rest on top.
    const bool ownSharp = fsr && m_fsrOwnSharpen.load();   // FSR: our sharpening pass does all of it (CAS, and Steady sharpening), not AMD's RCAS below 1
    const bool sharpening = (fsr && !ownSharp ? sharpen > 1.001f : sharpen > 0.001f) && EnsureSharpenTarget(outW, outH, outFormat);
    // edge smoothing of the upscaler's picture: from m_unsharpened into the output, or into m_smoothed when sharpening follows
    const float edges = m_edges.load();
    const bool smoothing = edges > 0.001f && EnsureSharpenTarget(outW, outH, outFormat) && (!sharpening || EnsureSmoothTarget(outW, outH, outFormat));
    LARGE_INTEGER qpcNow, qpcFreq; QueryPerformanceCounter(&qpcNow); QueryPerformanceFrequency(&qpcFreq);
    const float frameMs = m_lastRunQpc ? std::clamp(static_cast<float>((qpcNow.QuadPart - m_lastRunQpc) * 1000.0 / qpcFreq.QuadPart), 1.0f, 100.0f) : 16.7f;
    m_lastRunQpc = qpcNow.QuadPart;
    const float stability = m_stability.load();
    if (fsr) ConfigureFsrStability(stability);
    bool estimating = estimate && m_estimator.IsReady();
    if (estimating && m_estimator.NeedsResize(inW, inH)) { WaitIdle(); estimating = m_estimator.Ensure(inW, inH); }
    if (estimating && !m_estimatedLast) m_estimator.Forget();   // its frame before is not the one before this
    m_estimatedLast = estimating;
    // the lean (every upscaler): its picture into m_unsharpened, the leaned one into the output, or into m_leaned when edges or sharpening follow
    const bool leaning = estimating && !m_noMask && (m_fastMotion.load() != 0.0f || m_leanRest.load() > 0.001f) && m_leanPso && EnsureSharpenTarget(outW, outH, outFormat) &&
                         (!(sharpening || smoothing) || EnsureLeanTarget(outW, outH, outFormat));
    ID3D12Resource* const upscaled = (sharpening || smoothing || leaning) ? m_unsharpened : out;   // where the upscaler writes
    ID3D12Resource* const post = leaning ? m_leaned : m_unsharpened;                               // what edges and sharpening read
    // sharpening that follows the stability: needs the measured motion and the previous frame's picture; a frame without either starts the history again
    const float steadyShare = m_steadySharp.load();
    const bool steadySharp = sharpening && estimating && steadyShare > 0.001f && m_steadyPso && m_steadyRoot && EnsureSharpHist(outW, outH, outFormat);
    const bool histOk = steadySharp && m_sharpHistValid && !reset;
    if (!steadySharp) m_sharpHistValid = false;
    const int slot = TakeSlot();
    if (slot < 0) return skip();
    ReadTime(slot);
    m_estimator.ReadStats(slot);

    // descriptors: the flow (or a null view) and the motion vectors
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(slot) * kDescriptors * m_descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += static_cast<UINT64>(slot) * kDescriptors * m_descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    m_dev->CreateShaderResourceView(flow, &sv, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuUav = cpu; cpuUav.ptr += m_descriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_R16G16_FLOAT; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_dev->CreateUnorderedAccessView(m_motion, nullptr, &uv, cpuUav);
    if (smoothing) {   // the edge pass: the upscaler's picture in; the output, or m_smoothed for the sharpening pass, out
        D3D12_CPU_DESCRIPTOR_HANDLE cpuIn = cpu; cpuIn.ptr += 4 * m_descriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC si{}; si.Format = outFormat; si.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        si.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; si.Texture2D.MipLevels = 1;
        m_dev->CreateShaderResourceView(post, &si, cpuIn);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuOut = cpu; cpuOut.ptr += 5 * m_descriptorSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uo{}; uo.Format = outFormat; uo.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(sharpening ? m_smoothed : out, nullptr, &uo, cpuOut);
    }
    if (sharpening) {   // the sharpening pass: the upscaler's (or the edge pass's) picture in, the shared output out
        D3D12_CPU_DESCRIPTOR_HANDLE cpuIn = cpu; cpuIn.ptr += 2 * m_descriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC si{}; si.Format = outFormat; si.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        si.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; si.Texture2D.MipLevels = 1;
        m_dev->CreateShaderResourceView(smoothing ? m_smoothed : post, &si, cpuIn);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuOut = cpu; cpuOut.ptr += 3 * m_descriptorSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uo{}; uo.Format = outFormat; uo.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(out, nullptr, &uo, cpuOut);
    }
    if (steadySharp) {   // the steady sharpening: the picture, the previous average, the motion in; the output and the new average out
        D3D12_SHADER_RESOURCE_VIEW_DESC si{}; si.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; si.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; si.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uo{}; uo.Format = outFormat; uo.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        si.Format = outFormat;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += 12 * m_descriptorSize;
        m_dev->CreateShaderResourceView(smoothing ? m_smoothed : post, &si, h); h.ptr += m_descriptorSize;
        { D3D12_SHADER_RESOURCE_VIEW_DESC sh = si; sh.Format = m_sharpHistFmt; m_dev->CreateShaderResourceView(m_sharpHist[m_sharpHistCur], &sh, h); h.ptr += m_descriptorSize; }
        si.Format = DXGI_FORMAT_R16G16_FLOAT; m_dev->CreateShaderResourceView(m_motion, &si, h); h.ptr += m_descriptorSize;
        m_dev->CreateUnorderedAccessView(out, nullptr, &uo, h); h.ptr += m_descriptorSize;
        { D3D12_UNORDERED_ACCESS_VIEW_DESC uh = uo; uh.Format = m_sharpHistFmt; m_dev->CreateUnorderedAccessView(m_sharpHist[1 - m_sharpHistCur], nullptr, &uh, h); }
    }
    if (leaning) {   // the lean pass: DLSS's picture, the frame, the distrust mask in; the output (or m_leaned) out
        D3D12_SHADER_RESOURCE_VIEW_DESC si{}; si.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; si.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; si.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += 8 * m_descriptorSize;
        si.Format = outFormat; m_dev->CreateShaderResourceView(m_unsharpened, &si, h); h.ptr += m_descriptorSize;
        si.Format = inFormat == DXGI_FORMAT_UNKNOWN ? (hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM) : inFormat;
        m_dev->CreateShaderResourceView(color, &si, h); h.ptr += m_descriptorSize;
        si.Format = DXGI_FORMAT_R8_UNORM; m_dev->CreateShaderResourceView(m_distrust, &si, h); h.ptr += m_descriptorSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uo{}; uo.Format = outFormat; uo.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView((sharpening || smoothing) ? m_leaned : out, nullptr, &uo, h);
    }
    if (hdr) {   // the view pass: the frame (light) in, its SDR view out
        D3D12_CPU_DESCRIPTOR_HANDLE cpuIn = cpu; cpuIn.ptr += 6 * m_descriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC si{}; si.Format = inFormat == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R16G16B16A16_FLOAT : inFormat;
        si.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; si.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; si.Texture2D.MipLevels = 1;
        m_dev->CreateShaderResourceView(in, &si, cpuIn);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuOut = cpu; cpuOut.ptr += 7 * m_descriptorSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uo{}; uo.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; uo.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_dev->CreateUnorderedAccessView(m_view, nullptr, &uo, cpuOut);
    }

    m_list->Reset(m_alloc[slot], nullptr);
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * 4);
    // the shared textures come in COMMON
    Transition(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (flow) Transition(flow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (hdr) {   // the frame's SDR view, for the motion estimate
        { ID3D12DescriptorHeap* viewHeaps[] = { m_heap }; m_list->SetDescriptorHeaps(1, viewHeaps); }
        m_list->SetComputeRootSignature(m_rootSig);
        m_list->SetPipelineState(m_viewPso);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuIn = gpu; gpuIn.ptr += 6 * m_descriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuOut = gpu; gpuOut.ptr += 7 * m_descriptorSize;
        m_list->SetComputeRootDescriptorTable(0, gpuIn);
        m_list->SetComputeRootDescriptorTable(1, gpuOut);
        const ViewConstants lc{ inW, inH, { 0, 0 } };
        m_list->SetComputeRoot32BitConstants(2, 2, &lc, 0);
        m_list->Dispatch((inW + 7) / 8, (inH + 7) / 8, 1);
        Transition(m_view, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // 1. motion vectors: measured from the frames, or frame generation's flow (or none)
    Transition(m_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = { m_heap };
    if (estimating) {
        // In fast motion the upscaler leans on this frame (the distrust mask rises from 0.5 % of the frame's width a frame, fully at twice it):
        // Lossless Scaling's frames come without the jitter a game gives its upscaler, so history adds little there, and in a fast turn it trailed
        // (nr_sreval, Silent Hill f, a turn shrunk 1.5x: FSR 3.1 39.4 -> 41.8 dB at a quarter of the size, the leaves' doubled edges gone).
        { const float fast = m_fastMotion.load(); m_estimator.SetFastMotion(fast >= 0.0f ? fast : (m_fastShare.load() > 0.0f ? m_fastShare.load() : inW == outW && inH == outH ? kFastMotionShare : kFastMotionShareUpscaling) * static_cast<float>(inW)); }
        Transition(m_distrust, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_estimator.Record(m_list, slot, hdr ? m_view : in, hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : inFormat == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : inFormat,
                           m_motion, m_distrust, stability);
        Transition(m_distrust, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else {
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_rootSig);
        m_list->SetPipelineState(m_motionPso);
        m_list->SetComputeRootDescriptorTable(0, gpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuUav = gpu; gpuUav.ptr += m_descriptorSize;
        m_list->SetComputeRootDescriptorTable(1, gpuUav);
        const float unit = flowUnit > 0.1f ? flowUnit : 2.0f;
        const MotionConstants c{ inW, inH, flow && flowW ? static_cast<float>(inW) / (unit * flowW) * motionFraction : 0.0f, flow && flowW && flowH ? 1u : 0u };
        m_list->SetComputeRoot32BitConstants(2, 4, &c, 0);
        m_list->Dispatch((inW + 7) / 8, (inH + 7) / 8, 1);
    }
    Transition(m_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 1);

    // 2. the upscaler: FSR 3, or DLSS
    bool evaluated = true; char evalError[96] = {};
    if (fsr) {
        ffxDispatchDescUpscale d{}; d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = m_list;
        d.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(m_depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = ffxApiGetResourceDX12(m_motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.reactive = ffxApiGetResourceDX12(estimating && !m_noMask ? m_distrust : nullptr, FFX_API_RESOURCE_STATE_COMPUTE_READ);   // where the motion cannot be trusted
        d.output = ffxApiGetResourceDX12(upscaled, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = { 0.0f, 0.0f }; d.motionVectorScale = { m_motionScale, m_motionScale };   // vectors in the game's pixels
        d.renderSize = { inW, inH }; d.upscaleSize = { outW, outH };
        d.enableSharpening = !ownSharp && sharpen > 0.001f; d.sharpness = std::clamp(sharpen, 0.0f, 1.0f);
        d.frameTimeDelta = frameMs; d.preExposure = 1.0f; d.reset = reset || fresh;
        d.cameraNear = 0.1f; d.cameraFar = 1000.0f; d.cameraFovAngleVertical = 1.0f; d.viewSpaceToMetersFactor = 1.0f;   // the depth is flat anyway
        d.flags = hdr ? 0u : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;   // the frame as the game shows it (gamma-encoded), or light in HDR
        const ffxReturnCode_t rc = m_ffx->fn.Dispatch(&m_ffx->context, &d.header);
        if (rc != FFX_API_RETURN_OK) { evaluated = false; snprintf(evalError, sizeof evalError, "FSR dispatch failed (code %u)", rc); }
    }
    auto* p = static_cast<NVSDK_NGX_Parameter*>(m_params);
#if NR_HAVE_XESS
    if (m_backend == Backend::Xess) {
        xess_d3d12_execute_params_t x{};
        x.pColorTexture = color; x.pVelocityTexture = m_motion; x.pDepthTexture = m_depth; x.pOutputTexture = upscaled;
        // the distrust mask as XeSS's responsive one (where the motion cannot be trusted, or is fast); clipped to nothing without it (frame
        // generation's flow: the mask is then not written)
        x.pResponsivePixelMaskTexture = m_distrust;
        if (const float maxValue = estimating && !m_noMask ? 1.0f : 0.0f; maxValue != m_xess->maxResponsive) { m_xess->SetMaxResponsive(m_xess->context, maxValue); m_xess->maxResponsive = maxValue; }
        x.jitterOffsetX = 0.0f; x.jitterOffsetY = 0.0f; x.exposureScale = 1.0f; x.resetHistory = (reset || fresh) ? 1u : 0u;
        x.inputWidth = inW; x.inputHeight = inH;
        const xess_result_t rc = m_xess->Execute(m_xess->context, m_list, &x);
        if (rc != XESS_RESULT_SUCCESS) { evaluated = false; snprintf(evalError, sizeof evalError, "XeSS execute failed (code %d)", static_cast<int>(rc)); }
    }
#endif
    if (dlss) {
    p->Set(NVSDK_NGX_Parameter_Color, color);
    p->Set(NVSDK_NGX_Parameter_Output, upscaled);
    p->Set(NVSDK_NGX_Parameter_Depth, m_depth);
    p->Set(NVSDK_NGX_Parameter_MotionVectors, m_motion);
    // where the measured motion cannot be trusted, DLSS leans on this frame instead of its history (none with frame generation's flow)
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, estimating && !m_noMask ? m_distrust : static_cast<ID3D12Resource*>(nullptr));
    p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X, 0u); p->Set(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y, 0u);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f); p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, m_motionScale); p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, m_motionScale);
    p->Set(NVSDK_NGX_Parameter_Reset, (reset || fresh) ? 1 : 0);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, inW);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, inH);
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_EvaluateFeature_C(m_list, static_cast<NVSDK_NGX_Handle*>(m_feature), p, nullptr);
    if (r == NVSDK_NGX_Result_FAIL_FeatureNotFound || r == NVSDK_NGX_Result_FAIL_NotInitialized) {
        // the feature (or NGX itself) went away under us: another NGX user in the process shut down. This frame is left out; the next
        // run starts NGX again (RestartNgx) instead of switching the upscaler off.
        m_ngxLost = true;
        Log("DLSS upscaler: EvaluateFeature: %s; NGX starts again at the next frame", ResultName(r));
    } else if (NVSDK_NGX_FAILED(r)) { evaluated = false; snprintf(evalError, sizeof evalError, "EvaluateFeature(DLSS): %s", ResultName(r)); }
    else m_ngxRestarts = 0;
    }

    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 2);
    // 2b. the lean: the upscaler's picture toward this frame upscaled plainly, where the distrust mask says (the upscaler leaves its own heap bound)
    if (leaning) {
        Transition(m_unsharpened, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_leanRoot);
        m_list->SetPipelineState(m_leanPso);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuIn = gpu; gpuIn.ptr += 8 * m_descriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuOut = gpu; gpuOut.ptr += 11 * m_descriptorSize;
        m_list->SetComputeRootDescriptorTable(0, gpuIn);
        m_list->SetComputeRootDescriptorTable(1, gpuOut);
        const LeanConstants lc{ outW, outH, inW, inH, m_fastMotion.load() != 0.0f ? 1.0f : 0.0f, m_leanMode.load(), m_leanRest.load() };   // (strength 0: only the floor, when "Steady in fast motion" is off)
        m_list->SetComputeRoot32BitConstants(2, sizeof(LeanConstants) / 4, &lc, 0);
        m_list->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
        Transition(m_unsharpened, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (sharpening || smoothing) { D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = m_leaned; m_list->ResourceBarrier(1, &b); }
    }
    // 3. edge smoothing, from the upscaler's (or the lean's) picture (the upscaler leaves its own heap and root signature bound)
    if (smoothing) {
        Transition(post, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_rootSig);
        m_list->SetPipelineState(m_edgesPso);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuIn = gpu; gpuIn.ptr += 4 * m_descriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuOut = gpu; gpuOut.ptr += 5 * m_descriptorSize;
        m_list->SetComputeRootDescriptorTable(0, gpuIn);
        m_list->SetComputeRootDescriptorTable(1, gpuOut);
        const EdgeConstants ec{ outW, outH, std::clamp(edges, 0.0f, 1.0f), hdr ? 1u : 0u };
        m_list->SetComputeRoot32BitConstants(2, 4, &ec, 0);
        m_list->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
        Transition(post, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // 4. sharpening, into the shared output
    if (sharpening) {
        ID3D12Resource* const source = smoothing ? m_smoothed : post;
        Transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->SetDescriptorHeaps(1, heaps);
        // DLSS: CAS up to its maximum, amplified above it. FSR: RCAS did up to 1; this adds what is above it (CAS at full strength, scaled)
        const SharpenConstants sc = fsr && !ownSharp ? SharpenConstants{ outW, outH, 1.0f, sharpen - 1.0f, hdr ? 1u : 0u }
                                     : SharpenConstants{ outW, outH, std::min(sharpen, 1.0f), std::max(sharpen, 1.0f), hdr ? 1u : 0u };
        if (steadySharp) {
            // the motion is in the game's pixels, toward where the pixel was (or the other way: m_steadySign); the picture is outW wide
            const SteadyConstants st{ outW, outH, inW, inH, sc.amount, sc.gain, sc.hdr, steadyShare, m_steadySign.load() * static_cast<float>(outW) / static_cast<float>(inW), histOk ? 1u : 0u, m_steadyMvA.load(), m_steadyMvB.load() };
            m_list->SetComputeRootSignature(m_steadyRoot);
            m_list->SetPipelineState(m_steadyPso);
            D3D12_GPU_DESCRIPTOR_HANDLE g = gpu; g.ptr += 12 * m_descriptorSize;
            D3D12_GPU_DESCRIPTOR_HANDLE gUav = gpu; gUav.ptr += 15 * m_descriptorSize;
            m_list->SetComputeRootDescriptorTable(0, g);
            m_list->SetComputeRootDescriptorTable(1, gUav);
            m_list->SetComputeRoot32BitConstants(2, sizeof st / 4, &st, 0);
            m_list->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
            // the average just written becomes the one read next frame; the one read is written next
            Transition(m_sharpHist[m_sharpHistCur], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(m_sharpHist[1 - m_sharpHistCur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_sharpHistCur = 1 - m_sharpHistCur;
            Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_sharpHistValid = true;
        } else {
            m_list->SetComputeRootSignature(m_rootSig);
            m_list->SetPipelineState(m_sharpenPso);
            D3D12_GPU_DESCRIPTOR_HANDLE gpuIn = gpu; gpuIn.ptr += 2 * m_descriptorSize;
            D3D12_GPU_DESCRIPTOR_HANDLE gpuOut = gpu; gpuOut.ptr += 3 * m_descriptorSize;
            m_list->SetComputeRootDescriptorTable(0, gpuIn);
            m_list->SetComputeRootDescriptorTable(1, gpuOut);
            m_list->SetComputeRoot32BitConstants(2, 5, &sc, 0);
            m_list->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
            Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    Transition(in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (hdr) Transition(m_view, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (flow) Transition(flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 3);
    m_list->ResolveQueryData(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, slot * 4, 4, m_timestampReadback, static_cast<UINT64>(slot) * 32);
    m_list->Close();

    // submitted even when DLSS failed, so "done" is always signalled for a queued run
    m_queue->Wait(copied, copiedValue);
    ID3D12CommandList* lists[] = { m_list };
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(done, doneValue);
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_slotDone[slot] = m_fenceValue;
    if (m_ngxLost) return false;   // (NIS's picture this frame; NGX starts again at the next)
    if (!evaluated) { Fail("%s", evalError); return false; }
    ++m_runs;
    if (estimating && (++m_estimates == 60 || m_estimates % 1200 == 0)) {   // what the estimate found (a check that it follows the picture)
        double x = 0, y = 0, length = 0, cost = 0, distrust = 0; uint64_t frames = 0;
        if (m_estimator.TakeAverages(x, y, length, cost, distrust, frames))
            Log("motion estimator: over %llu frames, average vector (%.2f, %.2f) px, average length %.2f px, match cost %.4f; motion %.2f ms of %.2f ms; "
                "the upscaler told to lean on the current frame over %.1f%% of the picture", (unsigned long long)frames, x, y, length, cost, m_motionMs, m_gpuMs, distrust * 100.0);
        if (smoothing || sharpening)
            Log("%s upscaler: after the upscaler %.2f ms (edge smoothing %.2f, sharpening %.2f)", Name(), m_afterMs, smoothing ? edges : 0.0f, sharpen);
        double stage[4];
        if (m_estimator.TakeStageTimes(stage))
            Log("motion estimator: stages %.3f ms pyramid, %.3f search, %.3f median, %.3f every pixel", stage[0], stage[1], stage[2], stage[3]);
    }
    return true;
}
