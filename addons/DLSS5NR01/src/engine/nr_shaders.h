// The engine's own compute passes around the model, compiled when the engine starts (cs_5_0). They share one root signature: t0..t3, u0..u1,
// twelve root constants in b0 and a linear clamp sampler in s0 (nr_engine.cpp). Per run: CSDown (the frame to the proxy), CSFlowToMvec (LSFG's
// flow to the model's motion vectors), the model, then CSDelta or CSDeltaSmooth (model minus proxy, into the shared delta). The delta is put
// on the presented frames by the D3D11 side (compose11.cpp).
#pragma once
#include "engine/hdr_hlsl.h"

static const char* kNrModelHlsl = NR_HDR_HLSL R"HLSL(
SamplerState sLinear : register(s0);
Texture2D<float4>   tFrame  : register(t0);   // CSDown: the frame, full size
Texture2D<float4>   tProxy  : register(t1);   // CSDelta: the frame as the model saw it
Texture2D<float4>   tModel  : register(t2);   // CSDelta: the model's output
Texture2D<float4>   tFlow   : register(t3);   // CSFlowToMvec: LSFG's flow (RGBA16F)
RWTexture2D<float4> uOut    : register(u0);
RWTexture2D<float2> uMotion : register(u1);   // CSFlowToMvec only

cbuffer Constants : register(b0) {
    uint2 outSize;    // the texture written
    uint2 inSize;     // the texture read
    uint  flags;      // 1: a flow texture is bound
    float flowScale;  // CSFlowToMvec: working-size pixels per flow unit
    float smoothAmount;
    uint  encoding;   // CSDown: the frame's encoding (0 SDR, 1 scRGB, 2 HDR10)
    float white;      // CSDown: the SDR white, in nits (HDR only)
    uint3 pad1;
};

// The frame shrunk to the working size, in its SDR view (an HDR frame tone-mapped, see hdr_hlsl.h), which is what the model works on. Four bilinear reads spread over each output pixel's footprint: one read aliases once the frame is more
// than twice the size, and aliasing in the model's input comes out of it as noise.
[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    const float2 texel = 1.0 / float2(outSize);
    const float2 uv = (float2(id.xy) + 0.5) * texel;
    const float2 q = texel * 0.25;
    float3 sum = 0;
    [unroll] for (int k = 0; k < 4; ++k)
        sum += ToSdr(tFrame.SampleLevel(sLinear, uv + float2(k & 1 ? q.x : -q.x, k & 2 ? q.y : -q.y), 0).rgb, encoding, white);
    uOut[id.xy] = float4(sum * 0.25, 1.0);
}

// LSFG's flow (xy: current -> previous frame) as motion vectors in working-size pixels; zero without flow.
[numthreads(8, 8, 1)]
void CSFlowToMvec(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(outSize);
    uMotion[id.xy] = (flags & 1u) ? tFlow.SampleLevel(sLinear, uv, 0).xy * flowScale : float2(0, 0);
}

// The delta: the model's output minus its input, signed (RGBA16F).
[numthreads(8, 8, 1)]
void CSDelta(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    uOut[id.xy] = float4(tModel[id.xy].rgb - tProxy[id.xy].rgb, 0);
}
)HLSL";

// The delta with smoothing: the new delta is blended with the last one, read where this pixel's content was a frame ago (the motion vectors
// point current -> previous), and written both to the shared delta and to the history for the next run. flags 2: the history is usable (not
// right after a reset or a new feature). Against trailing (a fast camera turn, motion the estimate got wrong): the last delta is first held
// within the range of this frame's own deltas around the pixel (3x3), as temporal anti-aliasing holds its history, and the smoothing fades
// out in fast motion (flowScale: from that many pixels a frame, gone at twice it; 0: never), as the upscalers lean on the frame there.
static const char* kNrSmoothHlsl = R"HLSL(
SamplerState sLinear : register(s0);
Texture2D<float4>   tHistory : register(t0);   // the last smoothed delta
Texture2D<float4>   tProxy   : register(t1);
Texture2D<float4>   tModel   : register(t2);
Texture2D<float4>   tMotion  : register(t3);
RWTexture2D<float4> uOut     : register(u0);   // the shared delta
RWTexture2D<float4> uHistory : register(u1);   // the history for the next run

cbuffer Constants : register(b0) {
    uint2 outSize; uint2 inSize;
    uint  flags; float flowScale; float smoothAmount; uint encoding; float white; float stableWeight; uint2 pad1;
};

[numthreads(8, 8, 1)]
void CSDeltaSmooth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    float3 delta = tModel[id.xy].rgb - tProxy[id.xy].rgb;
    if (flags & 2u) {
        const float2 mv = tMotion[id.xy].xy;
        const float2 before = (float2(id.xy) + 0.5 + mv) / float2(outSize);
        float3 lo = delta, hi = delta;
        const int2 last = int2(outSize) - 1;
        [unroll] for (int k = 0; k < 9; ++k) {
            if (k == 4) continue;
            const int2 q = clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), last);
            const float3 d = tModel[q].rgb - tProxy[q].rgb;
            lo = min(lo, d); hi = max(hi, d);
        }
        // Where the frame itself still matches there (its brightness now against the brightness kept with the history, moved along the
        // motion), the history is the same surface: kept whole, so the model's change stops flickering while the camera moves. Where it
        // does not (what came into view, motion the estimate got wrong), the history is held in this frame's range and fades in fast
        // motion, as before (against trailing).
        const float4 kept = tHistory.SampleLevel(sLinear, before, 0);
        const float lumaNow = dot(tProxy[id.xy].rgb, float3(0.299, 0.587, 0.114));
        const bool inside = all(before >= 0.0) && all(before <= 1.0);
        const float agree = inside ? saturate(1.0 - abs(lumaNow - kept.a) / 0.05) : 0.0;
        const float3 history = lerp(clamp(kept.rgb, lo, hi), kept.rgb, agree);
        const float fast = flowScale > 0.0 ? saturate((length(mv) - flowScale) / flowScale) : 0.0;
        // Where the frame itself matches (agree), any change in the model's output is its own noise, not new content, so the history is trusted more there
        // than the setting alone would (stableWeight: how far up it goes); the setting still rules where the frame changed.
        const float w = lerp(saturate(smoothAmount), max(saturate(smoothAmount), stableWeight), agree * agree);
        delta = lerp(delta, history, w * lerp(1.0 - fast, 1.0, agree));
    }
    uOut[id.xy] = float4(delta, 0);
    uHistory[id.xy] = float4(delta, dot(tProxy[id.xy].rgb, float3(0.299, 0.587, 0.114)));   // the frame's brightness, for the next run's check
}
)HLSL";
