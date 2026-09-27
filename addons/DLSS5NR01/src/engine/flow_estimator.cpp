#include "engine/flow_estimator.h"
#include "engine/hdr_hlsl.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Every pass: t0-t3 in a table, u0-u1 in a table, u7 the statistics (a root view), twelve constants, a linear sampler.
const char* const kCommonHlsl = R"(
RWByteAddressBuffer uStats : register(u7);
SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 size; uint2 grid; uint2 coarse; uint radius; uint flags; float lambda; float bias; float stability; uint unused; };
)";

// the frame's brightness at its own size (and the statistics cleared for this frame). An HDR frame (radius: its encoding, lambda: the SDR
// white in nits) in its SDR view first, which the matching's thresholds are made for
const char* const kLumaHlsl = NR_HDR_HLSL R"(
Texture2D<float4> tFrame : register(t0);
RWTexture2D<float> uLuma : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0 && id.y == 0) { uStats.Store4(0, uint4(0, 0, 0, 0)); uStats.Store4(16, uint4(0, 0, 0, 0)); }
    if (id.x >= size.x || id.y >= size.y) return;
    uLuma[id.xy] = dot(ToSdr(tFrame.Load(int3(id.xy, 0)).rgb, radius, lambda), float3(0.299, 0.587, 0.114));
}
)";

// half size: the average of each 2x2 (grid holds the larger size)
const char* const kDownHlsl = R"(
Texture2D<float> tSrc : register(t0);
RWTexture2D<float> uDst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const int2 p = int2(id.xy) * 2, last = int2(grid) - 1;
    uDst[id.xy] = 0.25 * (tSrc.Load(int3(min(p, last), 0)) + tSrc.Load(int3(min(p + int2(1, 0), last), 0)) +
                          tSrc.Load(int3(min(p + int2(0, 1), last), 0)) + tSrc.Load(int3(min(p + int2(1, 1), last), 0)));
}
)";

// One size of the search: every 4x4 block, where its 8x8 surroundings were in the frame before. flags: 1 a coarser grid exists, 2 this is
// the finest size (a fraction of a pixel, and the statistics). Vectors in this size's pixels.
const char* const kSearchHlsl = R"(
Texture2D<float> tCur : register(t0);
Texture2D<float> tPrev : register(t1);
Texture2D<float2> tCoarse : register(t2);
RWTexture2D<float2> uGrid : register(u0);
static const float kExact = 0.0005, kShallow = 0.002;   // an exact match (brightness 0..1, per pixel); the least rise either side to refine on
static float cw[64];
static int2 origin;
float Sad(int2 v) {   // all 64 pixels of the 8x8 (the final answer and its fraction of a pixel)
    const int2 last = int2(size) - 1;
    float s = 0;
    [loop] for (int j = 0; j < 8; ++j) {
        [unroll] for (int i = 0; i < 8; ++i) s += abs(cw[j * 8 + i] - tPrev.Load(int3(clamp(origin + int2(i, j) + v, int2(0, 0), last), 0)));
    }
    return s * (1.0 / 64.0);
}
float SadHalf(int2 v) {   // half of them, as a checkerboard (the search: half the reads, with no stripe the pattern could hide in)
    const int2 last = int2(size) - 1;
    float s = 0;
    [loop] for (int j = 0; j < 8; ++j) {
        [unroll] for (int i = (j & 1); i < 8; i += 2) s += abs(cw[j * 8 + i] - tPrev.Load(int3(clamp(origin + int2(i, j) + v, int2(0, 0), last), 0)));
    }
    return s * (1.0 / 32.0);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= grid.x || id.y >= grid.y) return;
    const int2 last = int2(size) - 1;
    origin = int2(id.xy) * 4 - 2;
    [unroll] for (int j = 0; j < 8; ++j) [unroll] for (int i = 0; i < 8; ++i) cw[j * 8 + i] = tCur.Load(int3(clamp(origin + int2(i, j), int2(0, 0), last), 0));

    float2 predicted = float2(0, 0);
    int2 best = int2(0, 0);
    float bestCost = SadHalf(best);
    if (flags & 1) {   // the coarser size's answers for this block and its neighbours, doubled
        const int2 c = min(int2((id.xy * 2 + 1) >> 2), int2(coarse) - 1);
        predicted = tCoarse.Load(int3(c, 0)) * 2.0;
        bestCost += lambda * length(predicted);   // "not moving" strays from the guess too
        const int2 seeds[5] = { int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll] for (int s = 0; s < 5; ++s) {
            const int2 v = int2(round(tCoarse.Load(int3(clamp(c + seeds[s], int2(0, 0), int2(coarse) - 1), 0)) * 2.0));
            const float cost = SadHalf(v) + lambda * length(float2(v) - predicted);
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
    }
    const int2 center = best;
    const int r = int(radius);
    const float seedCost = bestCost - lambda * length(float2(best) - predicted);
    if (seedCost > kExact) {   // a guess that already matches exactly (a still area, mostly) needs no search around it
        [loop] for (int dy = -r; dy <= r; ++dy) {
            [loop] for (int dx = -r; dx <= r; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int2 v = center + int2(dx, dy);
                const float cost = SadHalf(v) + lambda * length(float2(v) - predicted);
                if (cost < bestCost) { bestCost = cost; best = v; }
            }
        }
    }
    float2 result = float2(best);
    if (flags & 2) {
        // A fraction of a pixel: the costs of summed differences rise in a V around the true position, so two lines of equal slope through
        // the costs either side (not a parabola, which pulls toward whole pixels). Not where the match is already exact (a still picture
        // must give exactly zero: a small wrong offset would slide DLSS's history every frame and blur text), nor where the V is too shallow.
        const float c0 = Sad(best), xm = Sad(best - int2(1, 0)), xp = Sad(best + int2(1, 0)), ym = Sad(best - int2(0, 1)), yp = Sad(best + int2(0, 1));
        const float sx = max(xm, xp) - c0, sy = max(ym, yp) - c0;
        if (c0 > kExact && sx > kShallow) result.x += clamp(0.5 * (xm - xp) / sx, -0.5, 0.5);
        if (c0 > kExact && sy > kShallow) result.y += clamp(0.5 * (ym - yp) / sy, -0.5, 0.5);
        uStats.InterlockedAdd(0, uint(c0 * 4096.0));
        uStats.InterlockedAdd(4, asuint(int(round(result.x * 32.0))));   // game pixels (twice this size's), 1/16 pixel
        uStats.InterlockedAdd(8, asuint(int(round(result.y * 32.0))));
        uStats.InterlockedAdd(12, 1u);
        uStats.InterlockedAdd(16, uint(length(result) * 32.0));
    }
    uGrid[id.xy] = result;
}
)";

// the 3x3 vector median: the neighbour closest (in sum) to all the others
const char* const kMedianHlsl = R"(
Texture2D<float2> tGrid : register(t0);
RWTexture2D<float2> uOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    float2 v[9];
    [unroll] for (int k = 0; k < 9; ++k) v[k] = tGrid.Load(int3(clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(size) - 1), 0));
    float bestSum = 1e30; float2 best = v[4];
    [unroll] for (int a = 0; a < 9; ++a) {
        float s = 0;
        [unroll] for (int b = 0; b < 9; ++b) s += abs(v[a].x - v[b].x) + abs(v[a].y - v[b].y);
        if (s < bestSum) { bestSum = s; best = v[a]; }
    }
    uOut[id.xy] = best;
}
)";

// Every pixel at the game's size: "not moving", its own block's vector and the three nearest blocks'; the one under which its 3x3 surroundings
// match best. "Not moving" wins a tie (a HUD or text that stays put must not slide), and another block's vector must beat the own block's by
// `bias`. flags: 1 there is a frame before (else zero). The distrust mask rises from 0 to 1 as even the best match's difference goes from
// kTrusted to kUntrusted (brightness 0..1, averaged over the 3x3).
// stability (0..1) moves that judgement from the pixel's 3x3 to the average brightness of its 5x5 surroundings (and widens the thresholds):
// shimmer (thin lines and leaves flickering from frame to frame) moves light around inside the surroundings and leaves their average about
// the same, while something newly uncovered changes it. So at 1 flicker is no longer reported, and the upscaler averages it out over its
// history instead of passing it through.
const char* const kPixelHlsl = R"(
Texture2D<float> tCur : register(t0);
Texture2D<float> tPrev : register(t1);
Texture2D<float2> tGrid : register(t2);
RWTexture2D<float2> uMotion : register(u0);
RWTexture2D<float> uDistrust : register(u1);
static const float kTrusted = 0.03, kUntrusted = 0.10;
float Pixel(uint2 id) {
    if (!(flags & 1)) { uMotion[id.xy] = float2(0, 0); uDistrust[id.xy] = 0; return 0; }
    const int2 p = int2(id.xy), last = int2(size) - 1, gl = int2(grid) - 1;
    const int2 cell = min(p >> 3, gl);
    const int2 q = int2((p.x & 7) < 4 ? -1 : 1, (p.y & 7) < 4 ? -1 : 1);
    float2 cand[5];
    cand[0] = float2(0, 0);
    cand[1] = tGrid.Load(int3(cell, 0)) * 2.0;
    cand[2] = tGrid.Load(int3(clamp(cell + int2(q.x, 0), int2(0, 0), gl), 0)) * 2.0;
    cand[3] = tGrid.Load(int3(clamp(cell + int2(0, q.y), int2(0, 0), gl), 0)) * 2.0;
    cand[4] = tGrid.Load(int3(clamp(cell + q, int2(0, 0), gl), 0)) * 2.0;
    float c[9];
    [unroll] for (int k = 0; k < 9; ++k) c[k] = tCur.Load(int3(clamp(p + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), last), 0));
    const float2 inv = 1.0 / float2(size);
    float bestCost = 1e30; float2 best = cand[0]; float bestPrev = 0;
    [unroll] for (int n = 0; n < 5; ++n) {
        // a vector already tried (neighbours moving alike, which is most of a moving picture; or the own block not moving) is not tried again
        if (n == 1 && all(abs(cand[1]) < 0.05)) continue;
        if (n >= 2 && (all(abs(cand[n] - cand[1]) < 0.05) || all(abs(cand[n] - cand[max(n - 1, 1)]) < 0.05) || all(abs(cand[n]) < 0.05))) continue;
        float s = n <= 1 ? 0.0 : bias, sp = 0;
        [unroll] for (int k = 0; k < 9; ++k) {
            const float prev = tPrev.SampleLevel(sLinear, (float2(p + int2(k % 3 - 1, k / 3 - 1)) + 0.5 + cand[n]) * inv, 0);
            s += abs(c[k] - prev) * (1.0 / 9.0); sp += prev;
        }
        if (s < bestCost) { bestCost = s; best = cand[n]; bestPrev = sp; }
    }
    uMotion[id.xy] = best;
    float cost = bestCost;
    if (stability > 0.001) {   // the 5x5 around: the 3x3 plus four bilinear taps, each the mean of a 2x2 in a corner
        float sc = 0;
        [unroll] for (int k = 0; k < 9; ++k) sc += c[k];
        float tc = 0, tp = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            const float2 at = float2(p) + 0.5 + float2(k & 1 ? 1.5 : -1.5, k & 2 ? 1.5 : -1.5);
            tc += tCur.SampleLevel(sLinear, at * inv, 0); tp += tPrev.SampleLevel(sLinear, (at + best) * inv, 0);
        }
        const float around = abs((sc + 4.0 * tc) - (bestPrev + 4.0 * tp)) * (1.0 / 25.0);
        cost = lerp(bestCost, around, stability) / (1.0 + stability);
    }
    float distrust = saturate((cost - kTrusted) / (kUntrusted - kTrusted));
    // A thin line that moves (a wire swaying, a branch): a ridge one pixel wide, brighter or darker than both its neighbours across it. Its
    // history is the line at other sub-pixel places, which only blurs it, so the upscaler leans on this frame there, whatever stability
    // says (test host: a swaying line 8.8 levels off the truth without stability, 10.2 with it, 2026-09-25)
    if (stability > 0.001 && dot(best, best) > 0.0625) {
        const float ridgeX = min(c[4] - max(c[3], c[5]), 1.0) , valleyX = min(min(c[3], c[5]) - c[4], 1.0);
        const float ridgeY = min(c[4] - max(c[1], c[7]), 1.0), valleyY = min(min(c[1], c[7]) - c[4], 1.0);
        const float thin = max(max(ridgeX, valleyX), max(ridgeY, valleyY));
        distrust = max(distrust, saturate((thin - 0.04) / 0.08));
    }
    uDistrust[id.xy] = distrust;
    return distrust;
}
groupshared uint gDistrust;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index == 0) gDistrust = 0;
    GroupMemoryBarrierWithGroupSync();
    const bool inside = id.x < size.x && id.y < size.y;
    float distrust = 0;
    if (inside) distrust = Pixel(id.xy);
    InterlockedAdd(gDistrust, uint(distrust * 100.0 + 0.5));
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && (flags & 1)) {
        uStats.InterlockedAdd(20, gDistrust);
        const uint w = min(8u, size.x - min(size.x, (id.x / 8) * 8)), h = min(8u, size.y - min(size.y, (id.y / 8) * 8));
        uStats.InterlockedAdd(24, w * h);
    }
}
)";

struct Constants { uint32_t w, h, gw, gh, cw, ch, radius, flags; float lambda, bias, stability; uint32_t unused; };
constexpr float kLambda = 0.003f;   // match cost per pixel of straying from the coarser size's guess
constexpr float kBias = 0.004f;     // a pixel's extra cost for another block's vector (or none) over its own

} // namespace

void FlowEstimator::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    m_log(text);
}

bool FlowEstimator::Init(ID3D12Device* dev, LogFn log) {
    m_log = std::move(log); m_dev = dev;
    D3D12_DESCRIPTOR_RANGE srv{}; srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv.NumDescriptors = 4;
    D3D12_DESCRIPTOR_RANGE uav{}; uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = { 1, &srv };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &uav };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; params[2].Descriptor.ShaderRegister = 7;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[3].Constants.Num32BitValues = sizeof(Constants) / 4;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    const D3D12_ROOT_SIGNATURE_DESC rootDesc{ 4, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(m_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root)))) {
        SafeRelease(blob); SafeRelease(error); Log("motion estimator: the root signature could not be made"); Shutdown(); return false;
    }
    SafeRelease(blob);
    const char* sources[PsoCount] = { kLumaHlsl, kDownHlsl, kSearchHlsl, kMedianHlsl, kPixelHlsl };
    const char* names[PsoCount] = { "flow_luma", "flow_down", "flow_search", "flow_median", "flow_pixel" };
    for (int i = 0; i < PsoCount; ++i) {
        const std::string text = std::string(kCommonHlsl) + sources[i];
        ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(text.c_str(), text.size(), names[i], nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            Log("motion estimator: %s: %s", names[i], err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); SafeRelease(err); Shutdown(); return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_root; pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        const HRESULT hr = m_dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso[i]));
        SafeRelease(code);
        if (FAILED(hr)) { Log("motion estimator: the %s pipeline 0x%08x", names[i], (unsigned)hr); Shutdown(); return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = kDescriptorsPerSlot * kSlots;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_dev->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)))) { Log("motion estimator: the descriptor heap could not be made"); Shutdown(); return false; }
    m_descriptorSize = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf{}; buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buf.Width = 32; buf.Height = 1; buf.DepthOrArraySize = 1; buf.MipLevels = 1;
    buf.SampleDesc.Count = 1; buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; buf.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(m_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_stats)))) {
        Log("motion estimator: the statistics buffer could not be made"); Shutdown(); return false;
    }
    buf.Width = 32 * kSlots; buf.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(m_dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_statsReadback)))) {
        Log("motion estimator: the statistics readback could not be made"); Shutdown(); return false;
    }
    // the time of each stage (diagnostics only: without them the estimate still runs)
    D3D12_QUERY_HEAP_DESC queries{}; queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = kStamps * kSlots;
    buf.Width = 8 * kStamps * kSlots;
    if (FAILED(m_dev->CreateQueryHeap(&queries, IID_PPV_ARGS(&m_stamps))) ||
        FAILED(m_dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_stampReadback)))) {
        SafeRelease(m_stamps); SafeRelease(m_stampReadback);
    }
    return true;
}

void FlowEstimator::Release(uint64_t retireAt) {
    auto drop = [&](ID3D12Resource*& t) {
        if (!t) return;
        if (retireAt) m_retired.push_back({ t, retireAt }); else t->Release();
        t = nullptr;
    };
    for (auto& set : m_luma) for (auto*& t : set) drop(t);
    for (auto*& t : m_grid) drop(t);
    drop(m_filtered);
    m_w = m_h = 0; m_levels = 0; m_havePrevious = false;
}

void FlowEstimator::Collect(uint64_t completed) {
    for (size_t i = 0; i < m_retired.size();)
        if (m_retired[i].at <= completed) { m_retired[i].texture->Release(); m_retired[i] = m_retired.back(); m_retired.pop_back(); } else ++i;
}

void FlowEstimator::Shutdown() {
    Release();
    for (const Retired& r : m_retired) r.texture->Release();   // the caller waited for the GPU
    m_retired.clear();
    for (auto*& p : m_pso) SafeRelease(p);
    SafeRelease(m_root); SafeRelease(m_heap); SafeRelease(m_stats); SafeRelease(m_statsReadback);
    SafeRelease(m_stamps); SafeRelease(m_stampReadback);
    for (bool& b : m_statsPending) b = false;
    for (bool& b : m_stampsPending) b = false;
    m_dev = nullptr;
}

bool FlowEstimator::Ensure(uint32_t w, uint32_t h, uint64_t retireAt) {
    if (!NeedsResize(w, h)) return true;
    Release(retireAt);
    // sizes: the frame's, then halved while the next is still at least 64x32 (at most kMaxLevels; at least down to half size)
    m_lw[0] = w; m_lh[0] = h; m_levels = 1;
    while (m_levels < kMaxLevels) {
        const uint32_t nw = (m_lw[m_levels - 1] + 1) / 2, nh = (m_lh[m_levels - 1] + 1) / 2;
        if (m_levels >= 2 && (nw < 64 || nh < 32)) break;
        m_lw[m_levels] = nw; m_lh[m_levels] = nh; ++m_levels;
    }
    for (int k = 0; k < m_levels; ++k) { m_gw[k] = (m_lw[k] + 3) / 4; m_gh[k] = (m_lh[k] + 3) / 4; }
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](uint32_t tw, uint32_t th, DXGI_FORMAT fmt, ID3D12Resource** out) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Format = fmt; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(out)));
    };
    bool ok = true;
    for (int s = 0; s < 2 && ok; ++s) for (int k = 0; k < m_levels && ok; ++k) ok = make(m_lw[k], m_lh[k], DXGI_FORMAT_R16_FLOAT, &m_luma[s][k]);
    for (int k = 1; k < m_levels && ok; ++k) ok = make(m_gw[k], m_gh[k], DXGI_FORMAT_R16G16_FLOAT, &m_grid[k]);
    if (ok) ok = make(m_gw[1], m_gh[1], DXGI_FORMAT_R16G16_FLOAT, &m_filtered);
    if (!ok) { Log("motion estimator: the textures for %ux%u could not be made", w, h); Release(); return false; }
    m_w = w; m_h = h; m_current = 0; m_havePrevious = false;
    Log("motion estimator: %ux%u, %d sizes down to %ux%u, searched in 4x4 blocks from %ux%u up to %ux%u", w, h, m_levels, m_lw[m_levels - 1], m_lh[m_levels - 1],
        m_lw[m_levels - 1], m_lh[m_levels - 1], m_lw[1], m_lh[1]);
    return true;
}

void FlowEstimator::Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    list->ResourceBarrier(1, &b);
}

FlowEstimator::Pass FlowEstimator::MakePass(int slot, int& index, ID3D12Resource* const srv[4], const DXGI_FORMAT srvFormat[4], ID3D12Resource* uav, DXGI_FORMAT uavFormat,
                                             ID3D12Resource* uav2, DXGI_FORMAT uav2Format) {
    const UINT base = static_cast<UINT>(slot * kDescriptorsPerSlot + index * kDescriptorsPerPass);
    ++index;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(base) * m_descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += static_cast<UINT64>(base) * m_descriptorSize;
    for (int i = 0; i < 4; ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = srvFormat[i] != DXGI_FORMAT_UNKNOWN ? srvFormat[i] : DXGI_FORMAT_R16_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(i) * m_descriptorSize;
        m_dev->CreateShaderResourceView(srv[i], &sv, h);   // a null resource makes a null view
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = uavFormat; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE hu = cpu; hu.ptr += 4 * static_cast<SIZE_T>(m_descriptorSize);
    m_dev->CreateUnorderedAccessView(uav, nullptr, &uv, hu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv2{}; uv2.Format = uav2Format; uv2.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    hu.ptr += m_descriptorSize;
    m_dev->CreateUnorderedAccessView(uav2, nullptr, &uv2, hu);   // a null resource makes a null view
    Pass p; p.srvs = gpu; p.uav = gpu; p.uav.ptr += 4 * static_cast<UINT64>(m_descriptorSize);
    return p;
}

void FlowEstimator::Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* frame, DXGI_FORMAT frameFormat, ID3D12Resource* motion, ID3D12Resource* distrust,
                           float stability, uint32_t encoding, float whiteNits) {
    const int cur = m_current, prev = 1 - m_current;
    int index = 0;
    auto stamp = [&](int i) { if (m_stamps) list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot * kStamps + i)); };
    stamp(0);
    ID3D12DescriptorHeap* heaps[] = { m_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root);
    list->SetComputeRootUnorderedAccessView(2, m_stats->GetGPUVirtualAddress());
    auto run = [&](Pso pso, ID3D12Resource* const srv[4], const DXGI_FORMAT fmt[4], ID3D12Resource* uav, DXGI_FORMAT uavFmt, const Constants& c, uint32_t tw, uint32_t th, bool restsReadable,
                   ID3D12Resource* uav2 = nullptr) {
        const Pass p = MakePass(slot, index, srv, fmt, uav, uavFmt, uav2);
        if (restsReadable) Barrier(list, uav, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetPipelineState(m_pso[pso]);
        list->SetComputeRootDescriptorTable(0, p.srvs);
        list->SetComputeRootDescriptorTable(1, p.uav);
        list->SetComputeRoot32BitConstants(3, sizeof(Constants) / 4, &c, 0);
        list->Dispatch((tw + 7) / 8, (th + 7) / 8, 1);
        if (restsReadable) Barrier(list, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = m_stats;   // the statistics, from pass to pass
        list->ResourceBarrier(1, &b);
    };
    const DXGI_FORMAT R16 = DXGI_FORMAT_R16_FLOAT, RG16 = DXGI_FORMAT_R16G16_FLOAT, NONE = DXGI_FORMAT_UNKNOWN;

    {   // this frame's pyramid
        ID3D12Resource* const srv[4] = { frame, nullptr, nullptr, nullptr }; const DXGI_FORMAT fmt[4] = { frameFormat, NONE, NONE, NONE };
        Constants c{ m_lw[0], m_lh[0] }; c.radius = encoding; c.lambda = whiteNits > 0.0f ? whiteNits : 80.0f;
        run(Luma, srv, fmt, m_luma[cur][0], R16, c, m_lw[0], m_lh[0], true);
    }
    for (int k = 1; k < m_levels; ++k) {
        ID3D12Resource* const srv[4] = { m_luma[cur][k - 1], nullptr, nullptr, nullptr }; const DXGI_FORMAT fmt[4] = { R16, NONE, NONE, NONE };
        run(Down, srv, fmt, m_luma[cur][k], R16, Constants{ m_lw[k], m_lh[k], m_lw[k - 1], m_lh[k - 1] }, m_lw[k], m_lh[k], true);
    }
    stamp(1);
    if (m_havePrevious) {
        for (int k = m_levels - 1; k >= 1; --k) {   // the search, coarse to fine
            const bool coarser = k + 1 < m_levels;
            ID3D12Resource* const srv[4] = { m_luma[cur][k], m_luma[prev][k], coarser ? m_grid[k + 1] : nullptr, nullptr };
            const DXGI_FORMAT fmt[4] = { R16, R16, RG16, NONE };
            Constants c{ m_lw[k], m_lh[k], m_gw[k], m_gh[k], coarser ? m_gw[k + 1] : 0u, coarser ? m_gh[k + 1] : 0u, coarser ? 2u : 4u,
                         (coarser ? 1u : 0u) | (k == 1 ? 2u : 0u), kLambda, kBias };
            run(Search, srv, fmt, m_grid[k], RG16, c, m_gw[k], m_gh[k], true);
        }
        stamp(2);
        {
            ID3D12Resource* const srv[4] = { m_grid[1], nullptr, nullptr, nullptr }; const DXGI_FORMAT fmt[4] = { RG16, NONE, NONE, NONE };
            run(Median, srv, fmt, m_filtered, RG16, Constants{ m_gw[1], m_gh[1] }, m_gw[1], m_gh[1], true);
        }
        stamp(3);
    }
    {   // every pixel (zero without a frame before)
        ID3D12Resource* const srv[4] = { m_luma[cur][0], m_luma[prev][0], m_filtered, nullptr }; const DXGI_FORMAT fmt[4] = { R16, R16, RG16, NONE };
        Constants c{ m_lw[0], m_lh[0], m_gw[1], m_gh[1], 0, 0, 0, m_havePrevious ? 1u : 0u, kLambda, kBias, std::clamp(stability, 0.0f, 1.0f) };
        run(Pixel, srv, fmt, motion, RG16, c, m_lw[0], m_lh[0], false, distrust);
    }
    if (m_havePrevious && m_stamps) {
        stamp(4);
        list->ResolveQueryData(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot * kStamps), kStamps, m_stampReadback, static_cast<UINT64>(slot) * kStamps * 8);
        m_stampsPending[slot] = true;
    }
    if (m_havePrevious) {   // the statistics, for ReadStats once this slot comes round again
        Barrier(list, m_stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(m_statsReadback, static_cast<UINT64>(slot) * 32, m_stats, 0, 32);
        Barrier(list, m_stats, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_statsPending[slot] = true;
    }
    m_current = prev; m_havePrevious = true;
}

void FlowEstimator::ReadStats(int slot) {
    if (slot >= 0 && slot < kSlots && m_stampsPending[slot] && m_stampReadback && m_timestampFreq) {
        m_stampsPending[slot] = false;
        const D3D12_RANGE range{ static_cast<SIZE_T>(slot) * kStamps * 8, static_cast<SIZE_T>(slot + 1) * kStamps * 8 };
        uint64_t* t = nullptr;
        if (SUCCEEDED(m_stampReadback->Map(0, &range, reinterpret_cast<void**>(&t)))) {
            const uint64_t* s = t + slot * kStamps;
            bool ordered = true; for (int i = 1; i < kStamps; ++i) ordered &= s[i] >= s[i - 1];
            if (ordered) { for (int i = 0; i < 4; ++i) m_stageSum[i] += (s[i + 1] - s[i]) * 1000.0 / m_timestampFreq; ++m_stageCount; }
            const D3D12_RANGE none{ 0, 0 };
            m_stampReadback->Unmap(0, &none);
        }
    }
    if (slot < 0 || slot >= kSlots || !m_statsPending[slot] || !m_statsReadback) return;
    m_statsPending[slot] = false;
    const D3D12_RANGE range{ static_cast<SIZE_T>(slot) * 32, static_cast<SIZE_T>(slot) * 32 + 32 };
    uint32_t* v = nullptr;
    if (FAILED(m_statsReadback->Map(0, &range, reinterpret_cast<void**>(&v)))) return;
    const uint32_t* s = v + slot * 8;
    m_sumCost += s[0] / 4096.0; m_sumX += static_cast<int32_t>(s[1]) / 16.0; m_sumY += static_cast<int32_t>(s[2]) / 16.0;
    m_blocks += s[3]; m_sumLength += s[4] / 16.0; m_sumDistrust += s[5] / 100.0; m_pixels += s[6]; ++m_frames;
    const D3D12_RANGE none{ 0, 0 };
    m_statsReadback->Unmap(0, &none);
}

bool FlowEstimator::TakeStageTimes(double ms[4]) {
    if (!m_stageCount) return false;
    for (int i = 0; i < 4; ++i) { ms[i] = m_stageSum[i] / m_stageCount; m_stageSum[i] = 0; }
    m_stageCount = 0;
    return true;
}

bool FlowEstimator::TakeAverages(double& x, double& y, double& length, double& cost, double& distrust, uint64_t& frames) {
    if (m_blocks <= 0) return false;
    x = m_sumX / m_blocks; y = m_sumY / m_blocks; length = m_sumLength / m_blocks; cost = m_sumCost / m_blocks; frames = m_frames;
    distrust = m_pixels > 0 ? m_sumDistrust / m_pixels : 0;
    m_sumCost = m_sumX = m_sumY = m_sumLength = m_blocks = m_sumDistrust = m_pixels = 0; m_frames = 0;
    return true;
}
