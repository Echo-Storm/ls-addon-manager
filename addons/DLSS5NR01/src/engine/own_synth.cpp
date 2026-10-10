#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/own_synth.h"
#include "engine/hdr_hlsl.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace nr {

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// See own_synth.h for what each pass does. All state lives in eleven structured buffers of uints (one a cell; floats as bits, vectors as two halves, in the
// frames' pixels). A cell is `scale` x `scale` pixels. HDR frames (scRGB, HDR10) are compared and blended in their SDR view (hdr_hlsl.h, as the upscalers do)
// and the frames made go back into their own encoding.
const char* const kHlsl = NR_HDR_HLSL R"(
Texture2D<float4> tA : register(t0);
Texture2D<float4> tC : register(t1);
Texture2D<float2> tV : register(t2);
RWStructuredBuffer<uint> bE0 : register(u0);    // the match error of each cell of c; later the moved pair's disagreement of hypothesis 1
RWStructuredBuffer<uint> bEa0 : register(u1);   // the match error of each cell of a (the reversed vectors); later hypothesis 2's disagreement
RWStructuredBuffer<uint> bK1 : register(u2);    // the splat keys: onto the time of the frame made, from c
RWStructuredBuffer<uint> bK2 : register(u3);    // onto a's cells (shared by every frame made from the pair)
RWStructuredBuffer<uint> bK3 : register(u4);    // onto the time of the frame made, from a's reversed vectors
RWStructuredBuffer<uint> bV1 : register(u5);    // the winners' vectors (also, at the end, each cell's vector F)
RWStructuredBuffer<uint> bV2 : register(u6);
RWStructuredBuffer<uint> bV3 : register(u7);
RWStructuredBuffer<uint> bB : register(u8);     // the 5x5 mean of c's match error (what the splats from c compete on)
RWStructuredBuffer<uint> bB2 : register(u9);    // the same for a's
RWStructuredBuffer<uint> bFL : register(u10);   // which hypothesis a cell took: 0 none, 1 from c, 2 from a, 3 filled from a neighbour
RWTexture2D<float4> uOut : register(u11);
SamplerState sLin : register(s0);
cbuffer C : register(b0) { uint2 size; uint2 fsize; uint mode; uint phase; float t; float k; uint encoding; float white; uint scale; uint pad; };

static const uint kEmpty = 0xFFFFFFFF;
float3 View(float3 c) { return ToSdr(c, encoding, white); }
float3 SmpA(float2 p) { return View(tA.SampleLevel(sLin, (p + 0.5) / float2(fsize), 0).rgb); }
float3 SmpC(float2 p) { return View(tC.SampleLevel(sLin, (p + 0.5) / float2(fsize), 0).rgb); }
uint Pack(float2 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }
float2 Unpack(uint u) { return float2(f16tof32(u & 0xFFFF), f16tof32(u >> 16)); }
uint Idx(int2 p) { return uint(p.y) * size.x + uint(p.x); }
bool Inside(int2 p) { return p.x >= 0 && p.y >= 0 && p.x < int(size.x) && p.y < int(size.y); }
float Lev(float3 d) { return (abs(d.x) + abs(d.y) + abs(d.z)) * 255.0; }
float2 Cen(int2 a) { return float2(a * int(scale)) + 0.5 * float(int(scale) - 1); }   // a cell's centre, in the frames' pixels
float2 VecAt(int2 a) {                                                                  // the estimate's vector for a cell: the mean over its pixels
    float2 s = float2(0, 0);
    [loop] for (uint y = 0; y < scale; ++y) [loop] for (uint x = 0; x < scale; ++x) s += tV[min(uint2(a) * scale + uint2(x, y), fsize - 1)];
    return s / float(scale * scale);
}
// the mean of a float buffer over (2r+1)^2 cells, edges clamped
#define BOX(buf, p, r, result) { float s_ = 0; [loop] for (int j_ = -(r); j_ <= (r); ++j_) [loop] for (int i_ = -(r); i_ <= (r); ++i_) \
    s_ += asfloat(buf[Idx(clamp((p) + int2(i_, j_), int2(0, 0), int2(size) - 1))]); result = s_ / float((2 * (r) + 1) * (2 * (r) + 1)); }
#define OOB if (id.x >= size.x || id.y >= size.y) return; const int2 p = int2(id.xy); const uint i = Idx(p);

// mode 0: K2 (the cells of a); mode 1: K1 and K3 (the frame made's), once for each frame made
[numthreads(8, 8, 1)] void Clear(uint3 id : SV_DispatchThreadID) {
    OOB
    if (mode == 0) bK2[i] = kEmpty; else { bK1[i] = kEmpty; bK3[i] = kEmpty; }
}

// the match error of every cell of c at its vector
[numthreads(8, 8, 1)] void Err0(uint3 id : SV_DispatchThreadID) {
    OOB
    const float2 c = Cen(p);
    bE0[i] = asuint(Lev(SmpC(c) - SmpA(c + VecAt(p))));
}

// the 5x5 mean of a match error (mode 0: c's into bB, 1: a's into bB2), once, for the splats
[numthreads(8, 8, 1)] void Box5(uint3 id : SV_DispatchThreadID) {
    OOB
    float e;
    if (mode == 0) { BOX(bE0, p, 2, e); bB[i] = asuint(e); } else { BOX(bEa0, p, 2, e); bB2[i] = asuint(e); }
}

// the match error of every cell of a that a vector landed on (the vector reversed)
[numthreads(8, 8, 1)] void ErrA(uint3 id : SV_DispatchThreadID) {
    OOB
    float e = 1e4;
    if (bK2[i] != kEmpty) { const float2 c = Cen(p); e = Lev(SmpA(c) - SmpC(c - Unpack(bV2[i]))); }
    bEa0[i] = asuint(e);
}

// the splat: mode 0 c's vectors to the frame made (k = 1 - t), 1 c's vectors onto a (k = 1; the vector kept), 2 a's reversed vectors to the frame made (k = t);
// phase 0 an atomic minimum on (match error, source), phase 1 the winner writes its vector
[numthreads(8, 8, 1)] void Splat(uint3 id : SV_DispatchThreadID) {
    OOB
    float2 vec; float err;
    if (mode == 2) { if (bK2[i] == kEmpty) return; vec = -Unpack(bV2[i]); err = asfloat(bB2[i]); }
    else { vec = VecAt(p); err = asfloat(bB[i]); }
    const int2 d = int2(round(float2(p) + k * vec / float(scale)));
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

// each hypothesis's moved pair's disagreement at every cell it reached
[numthreads(8, 8, 1)] void Hyp1(uint3 id : SV_DispatchThreadID) {
    OOB
    const float2 q = Cen(p);
    float dc = 1e4, da = 1e4;
    if (bK1[i] != kEmpty) { const float2 s = Unpack(bV1[i]); dc = Lev(SmpA(q + t * s) - SmpC(q - (1.0 - t) * s)); }
    if (bK3[i] != kEmpty) { const float2 s = Unpack(bV3[i]); da = Lev(SmpA(q - t * s) - SmpC(q + (1.0 - t) * s)); }
    bE0[i] = asuint(dc); bEa0[i] = asuint(da);
}

// per cell the hypothesis that agrees better over 3x3 (neighbours that did not reach do not count); the vector (as c's: c(x) ~ a(x + v)) goes to bV1,
// which hypothesis to bFL
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
    if (which == 2u) bV1[i] = Pack(-Unpack(bV3[i]));
    bFL[i] = which;
}

float Agree(int2 p, float2 u) {   // the moved pair's disagreement around a cell, nine samples a cell apart, one vector
    float s = 0;
    [unroll] for (int j = -2; j <= 2; j += 2) [unroll] for (int x = -2; x <= 2; x += 2) {
        const float2 q = Cen(p) + float2(x, j) * float(scale);
        s += Lev(SmpA(q + t * u) - SmpC(q - (1.0 - t) * u));
    }
    return s;
}

// a cell nothing reached: the nearest reached cell in each of four directions (up to 64 cells), or no motion, whichever agrees best
[numthreads(8, 8, 1)] void Holes(uint3 id : SV_DispatchThreadID) {
    OOB
    if (bFL[i] != 0) return;
    float2 cand[5]; cand[0] = float2(0, 0);
    const int2 dir[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll] for (int d = 0; d < 4; ++d) {
        float2 f = float2(0, 0);
        [loop] for (int s = 1; s <= 64; ++s) {
            const int2 n = p + dir[d] * s;
            if (!Inside(n)) break;
            const uint fl = bFL[Idx(n)];
            if (fl == 1u || fl == 2u) { f = Unpack(bV1[Idx(n)]); break; }
        }
        cand[d + 1] = f;
    }
    float2 F = float2(0, 0); float best = 1e30;
    [unroll] for (int c = 0; c < 5; ++c) {
        bool seen = false;
        [unroll] for (int b = 0; b < c; ++b) seen = seen || all(cand[b] == cand[c]);
        if (seen) continue;
        const float e = Agree(p, cand[c]);
        if (e < best) { best = e; F = cand[c]; }
    }
    bV1[i] = Pack(F); bFL[i] = 3u;
}

// the frame, at the frames' size: of the (up to) 2x2 nearest cells, the vector under which the moved pair agrees best here
[numthreads(8, 8, 1)] void Synth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= fsize.x || id.y >= fsize.y) return;
    const int2 p = int2(id.xy);
    const float2 q = float2(p);
    const int2 a0 = int2(floor((q - 0.5 * float(int(scale) - 1)) / float(scale)));
    const int n = scale == 1u ? 1 : 2;
    float2 F = float2(0, 0); float best = 1e30;
    [loop] for (int j = 0; j < n; ++j) [loop] for (int x = 0; x < n; ++x) {
        const int2 a = clamp(a0 + int2(x, j), int2(0, 0), int2(size) - 1);
        const float2 u = Unpack(bV1[Idx(a)]);
        const float e = Lev(SmpA(q + t * u) - SmpC(q - (1.0 - t) * u));
        if (e < best) { best = e; F = u; }
    }
    uOut[p] = float4(FromSdr((1.0 - t) * SmpA(q + t * F) + t * SmpC(q - (1.0 - t) * F), encoding, white), 1.0);
}
)";

struct Constants { uint32_t w, h, fw, fh, mode, phase; float t, k; uint32_t encoding; float white; uint32_t scale, pad; };

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
    static const char* const names[EntryCount] = { "Clear", "Err0", "Box5", "ErrA", "Splat", "Hyp1", "Hyp2", "Holes", "Synth" };
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

bool OwnSynth::ReadMs(int slot, double& ms) {
    if (!m_stampReadback || !m_freq) return false;
    const uint64_t* v = nullptr; D3D12_RANGE r{ static_cast<SIZE_T>(slot % kSlots) * 16, static_cast<SIZE_T>(slot % kSlots) * 16 + 16 };
    if (FAILED(m_stampReadback->Map(0, &r, reinterpret_cast<void**>(const_cast<uint64_t**>(&v))))) return false;
    const uint64_t a = v[(slot % kSlots) * 2], b = v[(slot % kSlots) * 2 + 1];
    D3D12_RANGE none{ 0, 0 }; m_stampReadback->Unmap(0, &none);
    if (b <= a) return false;
    ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(m_freq);
    return true;
}

void OwnSynth::Shutdown() {
    for (auto& b : m_buf) Rel(b);
    for (auto& p : m_pso) Rel(p);
    Rel(m_heap); Rel(m_root); Rel(m_stamps); Rel(m_stampReadback);
    m_w = m_h = m_aw = m_ah = 0; m_ready = false; m_dev = nullptr;
}

bool OwnSynth::Ensure(uint32_t w, uint32_t h) {
    if (!m_dev) return false;
    if (w == m_w && h == m_h && m_scale == m_wantScale && m_buf[0]) return true;
    for (auto& b : m_buf) Rel(b);
    m_scale = m_wantScale;
    m_aw = (w + m_scale - 1) / m_scale; m_ah = (h + m_scale - 1) / m_scale;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = static_cast<UINT64>(m_aw) * m_ah * 4; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto& b : m_buf) if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&b)))) { Log("own synthesis: buffers could not be made"); return false; }
    m_w = w; m_h = h;
    return true;
}

bool OwnSynth::Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* before, DXGI_FORMAT beforeFormat, ID3D12Resource* now, DXGI_FORMAT nowFormat,
                      ID3D12Resource* motion, DXGI_FORMAT outFormat, const Target* targets, int count, uint32_t encoding, float whiteNits) {
    if (!m_ready || !m_buf[0] || !before || !now || !motion || !targets || count < 1 || count > kMaxTargets) return false;
    for (int j = 0; j < count; ++j) if (!targets[j].out) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += static_cast<SIZE_T>(slot % kSlots) * kPerSlot * m_descriptorSize;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += static_cast<UINT64>(slot % kSlots) * kPerSlot * m_descriptorSize;
    auto srv = [&](int n, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = f; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += static_cast<SIZE_T>(n) * m_descriptorSize; m_dev->CreateShaderResourceView(r, &sv, h);
    };
    srv(0, before, beforeFormat); srv(1, now, nowFormat); srv(2, motion, DXGI_FORMAT_R16G16_FLOAT);
    // for each frame to make, a table of the eleven buffers and its own output (a shader cannot pick among outputs by an index)
    for (int j = 0; j < count; ++j) {
        const SIZE_T base = static_cast<SIZE_T>(kSrvs + j * kUavs);
        for (int b = 0; b < kBuffers; ++b) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uv.Buffer.NumElements = m_aw * m_ah; uv.Buffer.StructureByteStride = 4;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (base + b) * m_descriptorSize; m_dev->CreateUnorderedAccessView(m_buf[b], nullptr, &uv, h);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = outFormat; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu; h.ptr += (base + kBuffers) * m_descriptorSize; m_dev->CreateUnorderedAccessView(targets[j].out, nullptr, &uv, h);
    }
    if (m_freq && !m_stamps) {
        D3D12_QUERY_HEAP_DESC qd{}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = kSlots * 2;
        D3D12_HEAP_PROPERTIES rp{}; rp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kSlots * 16; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_stamps))) || FAILED(m_dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_stampReadback)))) { Rel(m_stamps); Rel(m_stampReadback); m_freq = 0; }
    }
    if (m_stamps) list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot % kSlots) * 2);
    {   // the buffers are shared by every frame in flight: the frame before's passes must be done with them (a UAV barrier orders the queue's work, lists included)
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = nullptr; list->ResourceBarrier(1, &b);
    }
    ID3D12DescriptorHeap* heaps[] = { m_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(m_root);
    list->SetComputeRootDescriptorTable(0, gpu);
    auto uavTable = [&](int j) { D3D12_GPU_DESCRIPTOR_HANDLE h = gpu; h.ptr += static_cast<UINT64>(kSrvs + j * kUavs) * m_descriptorSize; list->SetComputeRootDescriptorTable(1, h); };
    const UINT gx = (m_aw + 7) / 8, gy = (m_ah + 7) / 8, fx = (m_w + 7) / 8, fy = (m_h + 7) / 8;
    static const int stopAt = [] { char b[16]; return GetEnvironmentVariableA("NR_SYNTH_STOP", b, 16) ? atoi(b) : 999; }(); int passNo = 0;   // (a test aid: NR_SYNTH_STOP=N runs only the first N passes, to time them)
    float t = 0.5f;
    auto pass = [&](Entry e, uint32_t mode, uint32_t phase, float k, bool full = false) {
        if (++passNo > stopAt) return;
        const Constants c{ m_aw, m_ah, m_w, m_h, mode, phase, t, k, encoding, whiteNits > 0.0f ? whiteNits : 80.0f, static_cast<uint32_t>(m_scale), 0 };
        list->SetPipelineState(m_pso[e]);
        list->SetComputeRoot32BitConstants(2, sizeof(Constants) / 4, &c, 0);
        list->Dispatch(full ? fx : gx, full ? fy : gy, 1);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = nullptr;   // every pass reads what the one before wrote
        list->ResourceBarrier(1, &b);
    };
    // once for the pair: the errors and the vectors onto a's cells
    uavTable(0);
    pass(Clear, 0, 0, 0.0f);
    pass(Err0, 0, 0, 0.0f);
    pass(Box5, 0, 0, 0.0f);
    pass(Splat, 1, 0, 1.0f); pass(Splat, 1, 1, 1.0f);
    pass(ErrA, 0, 0, 0.0f);
    pass(Box5, 1, 0, 0.0f);
    // for each frame to make
    for (int j = 0; j < count; ++j) {
        t = targets[j].t;
        uavTable(j);
        pass(Clear, 1, 0, 0.0f);
        pass(Splat, 0, 0, 1.0f - t); pass(Splat, 0, 1, 1.0f - t);    // c's vectors to the frame made
        pass(Splat, 2, 0, t);        pass(Splat, 2, 1, t);           // a's reversed vectors to the frame made
        pass(Hyp1, 0, 0, 0.0f);
        pass(Hyp2, 0, 0, 0.0f);
        pass(Holes, 0, 0, 0.0f);
        pass(Synth, 0, 0, 0.0f, true);
    }
    if (m_stamps) {
        list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot % kSlots) * 2 + 1);
        list->ResolveQueryData(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot % kSlots) * 2, 2, m_stampReadback, static_cast<UINT64>(slot % kSlots) * 16);
    }
    return true;
}

} // namespace nr
