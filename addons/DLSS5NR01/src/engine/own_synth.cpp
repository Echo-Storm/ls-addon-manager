#define NOMINMAX
#include "engine/own_synth.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace nr {

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// See own_synth.h for what each pass does. All state lives in eight structured buffers of uints (floats as bits, vectors as two halves).
const char* const kHlsl = R"(
Texture2D<float4> tA : register(t0);
Texture2D<float4> tC : register(t1);
Texture2D<float2> tV : register(t2);
RWStructuredBuffer<uint> bE0 : register(u0);    // the match error of each pixel of c; later the moved pair's disagreement of hypothesis 1
RWStructuredBuffer<uint> bEa0 : register(u1);   // the match error of each pixel of a (the reversed vectors); later hypothesis 2's disagreement
RWStructuredBuffer<uint> bK1 : register(u2);    // the splat keys: onto the time of the frame made, from c
RWStructuredBuffer<uint> bK2 : register(u3);    // onto a's pixels (also, at the end, the pixel's hypothesis: 0 none, 1 from c, 2 from a)
RWStructuredBuffer<uint> bK3 : register(u4);    // onto the time of the frame made, from a's reversed vectors
RWStructuredBuffer<uint> bV1 : register(u5);    // the winners' vectors (also, at the end, each pixel's vector F)
RWStructuredBuffer<uint> bV2 : register(u6);
RWStructuredBuffer<uint> bV3 : register(u7);
RWTexture2D<float4> uOut : register(u8);
SamplerState sLin : register(s0);
cbuffer C : register(b0) { uint2 size; uint mode; uint phase; float t; float k; uint pad0; uint pad1; };

static const uint kEmpty = 0xFFFFFFFF;
float3 SmpA(float2 p) { return tA.SampleLevel(sLin, (p + 0.5) / float2(size), 0).rgb; }
float3 SmpC(float2 p) { return tC.SampleLevel(sLin, (p + 0.5) / float2(size), 0).rgb; }
uint Pack(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 Unpack(uint u) { return float2(f16tof32(u & 0xFFFF), f16tof32(u >> 16)); }
uint Idx(int2 p) { return uint(p.y) * size.x + uint(p.x); }
bool Inside(int2 p) { return p.x >= 0 && p.y >= 0 && p.x < int(size.x) && p.y < int(size.y); }
float Lev(float3 d) { return (abs(d.x) + abs(d.y) + abs(d.z)) * 255.0; }
// the mean of a float buffer over (2r+1)^2, edges clamped
#define BOX(buf, p, r, result) { float s_ = 0; [loop] for (int j_ = -(r); j_ <= (r); ++j_) [loop] for (int i_ = -(r); i_ <= (r); ++i_) \
    s_ += asfloat(buf[Idx(clamp((p) + int2(i_, j_), int2(0, 0), int2(size) - 1))]); result = s_ / float((2 * (r) + 1) * (2 * (r) + 1)); }
#define OOB if (id.x >= size.x || id.y >= size.y) return; const int2 p = int2(id.xy); const uint i = Idx(p);

[numthreads(8, 8, 1)] void Clear(uint3 id : SV_DispatchThreadID) {
    OOB
    bK1[i] = kEmpty; bK2[i] = kEmpty; bK3[i] = kEmpty;
}

// the match error of every pixel of c at its vector
[numthreads(8, 8, 1)] void Err0(uint3 id : SV_DispatchThreadID) {
    OOB
    bE0[i] = asuint(Lev(tC[p].rgb - SmpA(float2(p) + tV[p])));
}

// the match error of every pixel of a that a vector landed on (the vector reversed)
[numthreads(8, 8, 1)] void ErrA(uint3 id : SV_DispatchThreadID) {
    OOB
    float e = 1e4;
    if (bK2[i] != kEmpty) e = Lev(tA[p].rgb - SmpC(float2(p) - Unpack(bV2[i])));
    bEa0[i] = asuint(e);
}

// the splat: mode 0 c's vectors to the frame made (k = 1 - t), 1 c's vectors onto a (k = 1; the vector kept), 2 a's reversed vectors to the frame made (k = t);
// phase 0 an atomic minimum on (match error, source), phase 1 the winner writes its vector
[numthreads(8, 8, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
    OOB
    float2 vec; float err;
    if (mode == 2) { if (bK2[i] == kEmpty) return; vec = -Unpack(bV2[i]); BOX(bEa0, p, 2, err); }
    else { vec = tV[p]; BOX(bE0, p, 2, err); }
    const int2 d = int2(round(float2(p) + k * vec));
    if (!Inside(d)) return;
    const uint di = Idx(d);
    const uint key = (min(uint(err * 16.0), 65535u) << 16) | (i & 0xFFFF);
    if (phase == 0) {
        if (mode == 0) InterlockedMin(bK1[di], key); else if (mode == 1) InterlockedMin(bK2[di], key); else InterlockedMin(bK3[di], key);
    } else {
        if (mode == 0) { if (bK1[di] == key) bV1[di] = Pack(vec); }
        else if (mode == 1) { if (bK2[di] == key) bV2[di] = Pack(vec); }
        else { if (bK3[di] == key) bV3[di] = Pack(vec); }
    }
}

// each hypothesis's moved pair's disagreement at every pixel it reached
[numthreads(8, 8, 1)] void Hyp1(uint3 id : SV_DispatchThreadID) {
    OOB
    const float2 q = float2(p);
    float dc = 1e4, da = 1e4;
    if (bK1[i] != kEmpty) { const float2 s = Unpack(bV1[i]); dc = Lev(SmpA(q + t * s) - SmpC(q - (1.0 - t) * s)); }
    if (bK3[i] != kEmpty) { const float2 s = Unpack(bV3[i]); da = Lev(SmpA(q - t * s) - SmpC(q + (1.0 - t) * s)); }
    bE0[i] = asuint(dc); bEa0[i] = asuint(da);
}

// per pixel the hypothesis that agrees better over 3x3 (neighbours that did not reach do not count); the vector (as c's: c(x) ~ a(x + v)) goes to bV1,
// which hypothesis to bK2 (0 none, 1 from c, 2 from a)
[numthreads(8, 8, 1)] void Hyp2(uint3 id : SV_DispatchThreadID) {
    OOB
    const bool h1 = bK1[i] != kEmpty, h3 = bK3[i] != kEmpty;
    float dc = 0, da = 0, nc = 0, na = 0;
    [loop] for (int j = -1; j <= 1; ++j) [loop] for (int x = -1; x <= 1; ++x) {
        const uint n = Idx(clamp(p + int2(x, j), int2(0, 0), int2(size) - 1));
        const float ec = asfloat(bE0[n]), ea = asfloat(bEa0[n]);
        if (ec < 9999.0) { dc += ec; nc += 1.0; }
        if (ea < 9999.0) { da += ea; na += 1.0; }
    }
    dc = nc > 0 ? dc / nc : 1e4; da = na > 0 ? da / na : 1e4;
    uint which = 0;
    if (h1 && h3) which = dc <= da ? 1u : 2u; else if (h1) which = 1u; else if (h3) which = 2u;
    if (which == 1u) { /* the vector stays in bV1 */ }
    else if (which == 2u) bV1[i] = Pack(-Unpack(bV3[i]));
    bK2[i] = which;
}

float Agree(int2 p, float2 u) {   // the moved pair's disagreement over 5x5 around p, one vector
    float s = 0;
    [loop] for (int j = -2; j <= 2; ++j) [loop] for (int x = -2; x <= 2; ++x) {
        const float2 q = float2(p + int2(x, j));
        s += Lev(SmpA(q + t * u) - SmpC(q - (1.0 - t) * u));
    }
    return s;
}

[numthreads(8, 8, 1)] void Synth(uint3 id : SV_DispatchThreadID) {
    OOB
    float2 F = float2(0, 0);
    if (bK2[i] != 0) F = Unpack(bV1[i]);
    else {
        // nothing reached here: the nearest reached pixel in each of four directions (up to 64 pixels), or no motion, whichever agrees best
        float2 cand[5]; cand[0] = float2(0, 0);
        const int2 dir[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        [unroll] for (int d = 0; d < 4; ++d) {
            float2 f = float2(0, 0);
            [loop] for (int s = 1; s <= 64; ++s) {
                const int2 n = p + dir[d] * s;
                if (!Inside(n)) break;
                if (bK2[Idx(n)] != 0) { f = Unpack(bV1[Idx(n)]); break; }
            }
            cand[d + 1] = f;
        }
        float best = 1e30;
        [unroll] for (int c = 0; c < 5; ++c) {
            const float e = Agree(p, cand[c]);
            if (e < best) { best = e; F = cand[c]; }
        }
    }
    const float2 q = float2(p);
    uOut[p] = float4((1.0 - t) * SmpA(q + t * F) + t * SmpC(q - (1.0 - t) * F), 1.0);
}
)";

struct Constants { uint32_t w, h, mode, phase; float t, k; uint32_t pad0, pad1; };

} // namespace

bool OwnSynth::Init(ID3D12Device* dev, LogFn log) {
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
    if (!rootOk) { Log("own synthesis: the root signature could not be made"); Shutdown(); return false; }
    static const char* const names[EntryCount] = { "Clear", "Err0", "ErrA", "Splat", "Hyp1", "Hyp2", "Synth" };
    for (int e = 0; e < EntryCount; ++e) {
        ID3DBlob* code = nullptr;
        if (FAILED(D3DCompile(kHlsl, strlen(kHlsl), "own_synth", nullptr, nullptr, names[e], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
            std::string m = std::string("own synthesis: ") + names[e] + ": " + (error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
            Log(m.c_str()); Rel(error); Shutdown(); return false;
        }
        Rel(error);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_root; pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        const bool ok = SUCCEEDED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso[e])));
        Rel(code);
        if (!ok) { Log("own synthesis: a pipeline could not be made"); Shutdown(); return false; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kPerSlot * kSlots; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) { Shutdown(); return false; }
    m_descriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_ready = true;
    return true;
}

void OwnSynth::Shutdown() {
    for (auto& b : m_buf) Rel(b);
    for (auto& p : m_pso) Rel(p);
    Rel(m_heap); Rel(m_root);
    m_w = m_h = 0; m_ready = false; m_dev = nullptr;
}

bool OwnSynth::Ensure(uint32_t w, uint32_t h) {
    if (!m_dev) return false;
    if (w == m_w && h == m_h && m_buf[0]) return true;
    for (auto& b : m_buf) Rel(b);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = static_cast<UINT64>(w) * h * 4; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto& b : m_buf) if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&b)))) { Log("own synthesis: buffers could not be made"); return false; }
    m_w = w; m_h = h;
    return true;
}

bool OwnSynth::Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* before, DXGI_FORMAT beforeFormat, ID3D12Resource* now, DXGI_FORMAT nowFormat,
                      ID3D12Resource* motion, ID3D12Resource* out, DXGI_FORMAT outFormat, float t) {
    if (!m_ready || !m_buf[0] || !before || !now || !motion || !out) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(slot % kSlots) * kPerSlot * m_descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += static_cast<UINT64>(slot % kSlots) * kPerSlot * m_descriptorSize;
    auto srv = [&](int n, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = f; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(n) * m_descriptorSize; m_dev->CreateShaderResourceView(r, &sv, h);
    };
    srv(0, before, beforeFormat); srv(1, now, nowFormat); srv(2, motion, DXGI_FORMAT_R16G16_FLOAT);
    for (int b = 0; b < kBuffers; ++b) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = m_w * m_h; uv.Buffer.StructureByteStride = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(kSrvs + b) * m_descriptorSize; m_dev->CreateUnorderedAccessView(m_buf[b], nullptr, &uv, h);
    }
    {   D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = outFormat; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(kSrvs + kBuffers) * m_descriptorSize; m_dev->CreateUnorderedAccessView(out, nullptr, &uv, h); }
    ID3D12DescriptorHeap* heaps[] = { m_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root);
    list->SetComputeRootDescriptorTable(0, gpu);
    D3D12_GPU_DESCRIPTOR_HANDLE uavs = gpu; uavs.ptr += static_cast<UINT64>(kSrvs) * m_descriptorSize;
    list->SetComputeRootDescriptorTable(1, uavs);
    const UINT gx = (m_w + 7) / 8, gy = (m_h + 7) / 8;
    auto pass = [&](Entry e, uint32_t mode, uint32_t phase, float k) {
        const Constants c{ m_w, m_h, mode, phase, t, k, 0, 0 };
        list->SetPipelineState(m_pso[e]);
        list->SetComputeRoot32BitConstants(2, sizeof(Constants) / 4, &c, 0);
        list->Dispatch(gx, gy, 1);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = nullptr;   // every pass reads what the one before wrote
        list->ResourceBarrier(1, &b);
    };
    pass(Clear, 0, 0, 0.0f);
    pass(Err0, 0, 0, 0.0f);
    pass(Splat, 0, 0, 1.0f - t); pass(Splat, 0, 1, 1.0f - t);    // c's vectors to the frame made
    pass(Splat, 1, 0, 1.0f);     pass(Splat, 1, 1, 1.0f);        // c's vectors onto a's pixels
    pass(ErrA, 0, 0, 0.0f);
    pass(Splat, 2, 0, t);        pass(Splat, 2, 1, t);           // a's reversed vectors to the frame made
    pass(Hyp1, 0, 0, 0.0f);
    pass(Hyp2, 0, 0, 0.0f);
    pass(Synth, 0, 0, 0.0f);
    return true;
}

} // namespace nr
