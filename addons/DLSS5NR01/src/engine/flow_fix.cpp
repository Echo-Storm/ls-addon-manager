#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/flow_fix.h"
#include "engine/hdr_hlsl.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace nr {

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// See flow_fix.h for what each pass does.
const char* const kHlsl = NR_HDR_HLSL R"(
Texture2D<float4> tA : register(t0);
Texture2D<float4> tC : register(t1);
Texture2D<float2> tMotion : register(t2);
Texture2D<float> tDistrust : register(t3);
Texture2D<float> tLumaA : register(t4);
Texture2D<float> tLumaC : register(t5);
Texture2D<float4> tModes : register(t6);
Texture2D<float4> tCells : register(t7);
RWTexture2D<float> uLumaA : register(u0);
RWTexture2D<float> uLumaC : register(u1);
RWTexture2D<float4> uModes : register(u2);
RWTexture2D<float4> uCells : register(u3);
RWTexture2D<float2> uOut : register(u4);
SamplerState sLin : register(s0);
cbuffer C : register(b0) { uint2 fsize; uint2 csize; uint2 tiles; uint tile; uint reach; uint encoding; float white; uint cs; uint pad1; };   // cs: a cell is cs x cs pixels (2, or 4 at 1920 wide and up)

float3 View(float3 c) { return ToSdr(c, encoding, white); }
float Luma255(float3 c) { return dot(View(c), float3(0.299, 0.587, 0.114)) * 255.0; }

// 1. brightness at half size (the frames' own values in their SDR view, 0..255)
[numthreads(8, 8, 1)] void Luma(uint3 id : SV_DispatchThreadID) {
    if (id.x >= csize.x || id.y >= csize.y) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(csize);
    if (cs <= 2u) {
        uLumaA[id.xy] = Luma255(tA.SampleLevel(sLin, uv, 0).rgb);
        uLumaC[id.xy] = Luma255(tC.SampleLevel(sLin, uv, 0).rgb);
    } else {   // four taps, each the mean of 2 x 2 pixels: the cell's 4 x 4
        const float2 q = 0.5 / float2(fsize) * float(cs) * 0.5;
        float a = 0, c = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            const float2 o = float2((k & 1) ? q.x : -q.x, (k & 2) ? q.y : -q.y);
            a += Luma255(tA.SampleLevel(sLin, uv + o, 0).rgb); c += Luma255(tC.SampleLevel(sLin, uv + o, 0).rgb);
        }
        uLumaA[id.xy] = a * 0.25; uLumaC[id.xy] = c * 0.25;
    }
}

// 2. per tile: the dominant movement of the trusted, moving pixels in the window of (2 * reach + 1)^2 tiles around it (every second pixel): the 2 px bin with the most votes
// in its 3 x 3 neighbourhood of bins, then the mean of the vectors within 3 px of that bin's middle; with the share of the window's votes that agree
groupshared uint gHist[4096];
groupshared uint gBest, gTotal;
groupshared int gSumX, gSumY, gCount;
bool Vote(int2 p, out float2 v) {
    v = tMotion.Load(int3(p, 0));
    return tDistrust.Load(int3(p, 0)) < 0.3 && dot(v, v) > 1.0 && all(abs(v) < 63.9);
}
[numthreads(256, 1, 1)] void TileMode(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    const int2 t = int2(gid.xy);
    const int x0 = max(0, (t.x - int(reach)) * int(tile)), x1 = min(int(fsize.x), (t.x + int(reach) + 1) * int(tile));
    const int y0 = max(0, (t.y - int(reach)) * int(tile)), y1 = min(int(fsize.y), (t.y + int(reach) + 1) * int(tile));
    const int st = int(cs);   // every cs-th pixel votes (cs: 2 or 4)
    const int nx = max(1, (x1 - x0 + st - 1) / st), ny = max(1, (y1 - y0 + st - 1) / st);
    for (uint b = gi; b < 4096u; b += 256u) gHist[b] = 0u;
    if (gi == 0) { gBest = 0u; gTotal = 0u; gSumX = 0; gSumY = 0; gCount = 0; }
    GroupMemoryBarrierWithGroupSync();
    for (int idx = int(gi); idx < nx * ny; idx += 256) {
        const int2 p = int2(x0 + st * (idx % nx), y0 + st * (idx / nx));
        float2 v;
        if (Vote(p, v)) {
            const int2 bin = clamp(int2(floor(v * 0.5)) + 32, int2(0, 0), int2(63, 63));
            uint old; InterlockedAdd(gHist[bin.y * 64 + bin.x], 1u, old);
            InterlockedAdd(gTotal, 1u, old);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint b = gi; b < 4096u; b += 256u) {
        const int2 c = int2(b & 63u, b >> 6);
        uint score = 0;
        [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) {
            const int2 n = clamp(c + int2(i, j), int2(0, 0), int2(63, 63));
            score += gHist[n.y * 64 + n.x];
        }
        uint old; InterlockedMax(gBest, (score << 12) | b, old);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint peak = gBest & 4095u;
    const float2 centre = (float2(int2(peak & 63u, peak >> 6)) - 32.0) * 2.0 + 1.0;
    for (int idx2 = int(gi); idx2 < nx * ny; idx2 += 256) {
        const int2 p = int2(x0 + st * (idx2 % nx), y0 + st * (idx2 / nx));
        float2 v;
        if (Vote(p, v) && all(abs(v - centre) <= 3.0)) {
            int old;
            InterlockedAdd(gSumX, int(round(v.x * 16.0)), old); InterlockedAdd(gSumY, int(round(v.y * 16.0)), old); InterlockedAdd(gCount, 1, old);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        float4 r = float4(0, 0, 0, 0);
        if (gTotal >= 60u && gCount > 0) r = float4(float(gSumX) / (16.0 * gCount), float(gSumY) / (16.0 * gCount), float(gCount) / float(gTotal), 1.0);
        uModes[t] = r;
    }
}

// 3. per half-size cell: take the tile's dominant vector, or keep the cell's own
float PatchCost(int2 c, float2 v) {   // the 3 x 3 cells around c: how unlike their place in the earlier frame (moved by v, in the frames' pixels) they are
    float s = 0;
    [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) {
        const int2 n = clamp(c + int2(i, j), int2(0, 0), int2(csize) - 1);
        s += abs(tLumaC.Load(int3(n, 0)) - tLumaA.SampleLevel(sLin, (float2(n) + 0.5 + v / float(cs)) / float2(csize), 0));
    }
    return s * (1.0 / 9.0);
}
float Contrast(int2 c) {
    float s = 0, a[25];
    [unroll] for (int j = -2; j <= 2; ++j) [unroll] for (int i = -2; i <= 2; ++i) { const float l = tLumaC.Load(int3(clamp(c + int2(i, j), int2(0, 0), int2(csize) - 1), 0)); a[(j + 2) * 5 + (i + 2)] = l; s += l; }
    const float mean = s * (1.0 / 25.0);
    float d = 0;
    [unroll] for (int k = 0; k < 25; ++k) d += abs(a[k] - mean);
    return d * (1.0 / 25.0);
}
[numthreads(8, 8, 1)] void Decide(uint3 id : SV_DispatchThreadID) {
    if (id.x >= csize.x || id.y >= csize.y) return;
    const int2 c = int2(id.xy);
    const int2 px = min(c * int(cs), int2(fsize) - 1);
    const float2 own = tMotion.Load(int3(px, 0));
    const float4 mode = tModes.Load(int3(min(px / int(tile), int2(tiles) - 1), 0));
    float2 outv = own; float take = 0.0;
    if (mode.w > 0.5 && mode.z > 0.3 && length(mode.xy - own) > 1.0) {
        const float2 L = mode.xy;
        const float ownc = PatchCost(c, own), lc = PatchCost(c, L);
        bool amb = false;
        if (ownc < 8.0 && lc <= ownc * 1.1 + 1.5 && length(own) <= length(L) + 2.0) {
            const float contrast = Contrast(c);
            amb = contrast < 2.5;
            if (!amb) {   // repeating: matched about as well 6, 10 or 14 pixels along or across
                [unroll] for (int k = 0; k < 2; ++k) {
                    const float d = 8.0 + 6.0 * k;
                    amb = amb || PatchCost(c, own + float2(d, 0)) <= ownc + 0.15 * contrast + 0.5 || PatchCost(c, own - float2(d, 0)) <= ownc + 0.15 * contrast + 0.5
                              || PatchCost(c, own + float2(0, d)) <= ownc + 0.15 * contrast + 0.5 || PatchCost(c, own - float2(0, d)) <= ownc + 0.15 * contrast + 0.5;
                }
            }
        }
        if (amb || lc + 8.0 < ownc) { outv = L; take = 1.0; }
    }
    uCells[c] = float4(outv, take, 0.0);
}

// 4. the result at the frames' size
[numthreads(8, 8, 1)] void Apply(uint3 id : SV_DispatchThreadID) {
    if (id.x >= fsize.x || id.y >= fsize.y) return;
    const float4 cell = tCells.Load(int3(min(int2(id.xy) / int(cs), int2(csize) - 1), 0));
    uOut[id.xy] = cell.z > 0.5 ? cell.xy : tMotion.Load(int3(id.xy, 0));
}
)";

struct Constants { uint32_t fw, fh, cw, ch, tx, ty, tile, reach, encoding; float white; uint32_t cs, pad1; };

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    list->ResourceBarrier(1, &br);
}

} // namespace

bool FlowFix::Init(ID3D12Device* dev, LogFn log) {
    Shutdown();
    m_dev = dev; m_log = std::move(log);
    D3D12_DESCRIPTOR_RANGE srv{}; srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv.NumDescriptors = kSrvs;
    D3D12_DESCRIPTOR_RANGE uav{}; uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav.NumDescriptors = kUavs;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = { 1, &srv };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &uav };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants.Num32BitValues = sizeof(Constants) / 4;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC smp{}; smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = D3D12_FLOAT32_MAX; smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC rootDesc{ 3, params, 1, &smp, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    const bool rootOk = SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) &&
                        SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root)));
    Rel(blob); Rel(error);
    if (!rootOk) { Log("flow fix: the root signature could not be made"); Shutdown(); return false; }
    static const char* const names[EntryCount] = { "Luma", "TileMode", "Decide", "Apply" };
    for (int e = 0; e < EntryCount; ++e) {
        ID3DBlob* code = nullptr;
        if (FAILED(D3DCompile(kHlsl, strlen(kHlsl), "flow_fix", nullptr, nullptr, names[e], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
            std::string m = std::string("flow fix: ") + names[e] + ": " + (error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            Log(m.c_str()); Rel(error); Shutdown(); return false;
        }
        Rel(error);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_root; pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        const bool ok = SUCCEEDED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso[e])));
        Rel(code);
        if (!ok) { Log("flow fix: a pipeline could not be made"); Shutdown(); return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kPerSlot * kSlots; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) { Shutdown(); return false; }
    m_descriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_ready = true;
    return true;
}

bool FlowFix::ReadMs(int slot, double& ms) {
    if (!m_stampReadback || !m_freq) return false;
    const uint64_t* v = nullptr; D3D12_RANGE r{ static_cast<SIZE_T>(slot % kSlots) * 16, static_cast<SIZE_T>(slot % kSlots) * 16 + 16 };
    if (FAILED(m_stampReadback->Map(0, &r, reinterpret_cast<void**>(const_cast<uint64_t**>(&v))))) return false;
    const uint64_t a = v[(slot % kSlots) * 2], b = v[(slot % kSlots) * 2 + 1];
    D3D12_RANGE none{ 0, 0 }; m_stampReadback->Unmap(0, &none);
    if (b <= a) return false;
    ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(m_freq);
    return true;
}

void FlowFix::Release() { Rel(m_lumaA); Rel(m_lumaC); Rel(m_modes); Rel(m_cells); m_w = m_h = 0; }

void FlowFix::Shutdown() {
    Release();
    for (auto& p : m_pso) Rel(p);
    Rel(m_heap); Rel(m_root); Rel(m_stamps); Rel(m_stampReadback);
    m_ready = false; m_dev = nullptr;
}

bool FlowFix::Ensure(uint32_t w, uint32_t h) {
    if (!m_dev) return false;
    if (w == m_w && h == m_h && m_lumaA) return true;
    Release();
    m_cs = w >= 1920 ? 4u : 2u;
    m_cw = (w + m_cs - 1) / m_cs; m_ch = (h + m_cs - 1) / m_cs;
    m_tile = w >= 1920 ? 64u : 32u;
    m_tx = (w + m_tile - 1) / m_tile; m_ty = (h + m_tile - 1) / m_tile;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](ID3D12Resource*& r, uint32_t tw, uint32_t th, DXGI_FORMAT f) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r)));
    };
    if (!make(m_lumaA, m_cw, m_ch, DXGI_FORMAT_R32_FLOAT) || !make(m_lumaC, m_cw, m_ch, DXGI_FORMAT_R32_FLOAT) || !make(m_modes, m_tx, m_ty, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !make(m_cells, m_cw, m_ch, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
        Log("flow fix: textures could not be made"); Release(); return false;
    }
    m_w = w; m_h = h;
    return true;
}

bool FlowFix::Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* before, DXGI_FORMAT beforeFormat, ID3D12Resource* now, DXGI_FORMAT nowFormat,
                     ID3D12Resource* motion, ID3D12Resource* distrust, ID3D12Resource* out, uint32_t encoding, float whiteNits) {
    if (!m_ready || !m_lumaA || !before || !now || !motion || !distrust || !out) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(slot % kSlots) * kPerSlot * m_descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += static_cast<UINT64>(slot % kSlots) * kPerSlot * m_descriptorSize;
    auto srv = [&](int n, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = f; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(n) * m_descriptorSize; m_dev->CreateShaderResourceView(r, &sv, h);
    };
    auto uav = [&](int n, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = f; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(kSrvs + n) * m_descriptorSize; m_dev->CreateUnorderedAccessView(r, nullptr, &uv, h);
    };
    srv(0, before, beforeFormat); srv(1, now, nowFormat); srv(2, motion, DXGI_FORMAT_R16G16_FLOAT); srv(3, distrust, DXGI_FORMAT_R8_UNORM);
    srv(4, m_lumaA, DXGI_FORMAT_R32_FLOAT); srv(5, m_lumaC, DXGI_FORMAT_R32_FLOAT); srv(6, m_modes, DXGI_FORMAT_R16G16B16A16_FLOAT); srv(7, m_cells, DXGI_FORMAT_R16G16B16A16_FLOAT);
    uav(0, m_lumaA, DXGI_FORMAT_R32_FLOAT); uav(1, m_lumaC, DXGI_FORMAT_R32_FLOAT); uav(2, m_modes, DXGI_FORMAT_R16G16B16A16_FLOAT); uav(3, m_cells, DXGI_FORMAT_R16G16B16A16_FLOAT);
    uav(4, out, DXGI_FORMAT_R16G16_FLOAT);
    if (m_freq && !m_stamps) {
        D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = kSlots * 2;
        D3D12_HEAP_PROPERTIES rp{}; rp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kSlots * 16; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_stamps))) || FAILED(m_dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_stampReadback)))) { Rel(m_stamps); Rel(m_stampReadback); m_freq = 0; }
    }
    if (m_stamps) list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot % kSlots) * 2);
    ID3D12DescriptorHeap* heaps[] = { m_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root);
    list->SetComputeRootDescriptorTable(0, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE ugpu = gpu; ugpu.ptr += static_cast<UINT64>(kSrvs) * m_descriptorSize;
    list->SetComputeRootDescriptorTable(1, ugpu);
    const Constants c{ m_w, m_h, m_cw, m_ch, m_tx, m_ty, m_tile, m_w >= 1920u ? 2u : 3u, encoding, whiteNits > 0.0f ? whiteNits : 80.0f, m_cs, 0 };
    list->SetComputeRoot32BitConstants(2, sizeof(Constants) / 4, &c, 0);
    const D3D12_RESOURCE_STATES uavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto uavBarrier = [&]() { D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = nullptr; list->ResourceBarrier(1, &b); };
    const UINT gx = (m_cw + 7) / 8, gy = (m_ch + 7) / 8;
    // 1. half-size brightness
    list->SetPipelineState(m_pso[Luma]); list->Dispatch(gx, gy, 1);
    Transition(list, m_lumaA, uavState, srvState); Transition(list, m_lumaC, uavState, srvState);
    // 2. each tile's dominant vector
    list->SetPipelineState(m_pso[TileMode]); list->Dispatch(m_tx, m_ty, 1);
    Transition(list, m_modes, uavState, srvState);
    // 3. each cell's decision
    list->SetPipelineState(m_pso[Decide]); list->Dispatch(gx, gy, 1);
    Transition(list, m_cells, uavState, srvState);
    // 4. the result
    list->SetPipelineState(m_pso[Apply]); list->Dispatch((m_w + 7) / 8, (m_h + 7) / 8, 1);
    uavBarrier();
    Transition(list, m_lumaA, srvState, uavState); Transition(list, m_lumaC, srvState, uavState); Transition(list, m_modes, srvState, uavState); Transition(list, m_cells, srvState, uavState);
    if (m_stamps) {
        list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot % kSlots) * 2 + 1);
        list->ResolveQueryData(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot % kSlots) * 2, 2, m_stampReadback, static_cast<UINT64>(slot % kSlots) * 16);
    }
    return true;
}

} // namespace nr
