#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "addon/source_cadence.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <map>

namespace nr::cadence {

void Detector::Reset() { m_stays.clear(); m_run = 0; m_frames = 0; }

// Frames were lost between two reads: the stay in progress is not known; repeats are ignored until the next new picture starts a stay that is whole.
void Detector::Break() { m_run = -1; }

void Detector::Push(bool repeatOfBefore) {
    ++m_frames;
    if (m_run < 0) { if (!repeatOfBefore) m_run = 1; return; }
    if (repeatOfBefore && m_run > 0) { m_run = std::min(m_run + 1, 100000); return; }
    if (m_run > 0) { m_stays.push_back(std::min(m_run, 1000)); while (static_cast<int>(m_stays.size()) > kKeep) m_stays.pop_front(); }
    m_run = 1;
}

Result Detector::Verdict() const {
    Result r;
    if (static_cast<int>(m_stays.size()) < kNeed) return r;
    int count[kMaxRepeat + 1] = {};
    for (int s : m_stays) if (s <= kMaxRepeat) ++count[s];
    int best = 1; for (int l = 2; l <= kMaxRepeat; ++l) if (count[l] > count[best]) best = l;
    if (best < 2) return r;
    if (count[best] * 100 < static_cast<int>(m_stays.size()) * 85) return r;
    r.repeats = best; r.stays = static_cast<int>(m_stays.size());
    return r;
}

namespace {
const char* kSampleHlsl = R"(
Texture2D<float4> src : register(t0);
RWStructuredBuffer<float> curv : register(u0);
RWStructuredBuffer<float> prevv : register(u1);
RWStructuredBuffer<float> res : register(u2);
cbuffer C : register(b0) { uint2 sz; uint2 pad; };
groupshared float part[256];
[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupIndex) {
    float acc = 0;
    for (uint i = tid; i < 2304; i += 256) {
        uint gx = i % 64, gy = i / 64;
        uint2 p = min(sz - 1, uint2((gx * 2 + 1) * sz.x / 128, (gy * 2 + 1) * sz.y / 72));
        float3 c = src.Load(int3(p, 0)).rgb;
        float l = dot(c, float3(0.2126, 0.7152, 0.0722));
        if (!isfinite(l)) l = 0;
        l = max(l, 0);
        l = l / (1 + l);
        curv[i] = l;
        acc += abs(l - prevv[i]);
    }
    part[tid] = acc;
    GroupMemoryBarrierWithGroupSync();
    for (uint st = 128; st > 0; st >>= 1) {
        if (tid < st) part[tid] += part[tid + st];
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0) res[0] = part[0] / 2304.0;
}
)";

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}
}

bool Probe::Init(ID3D11Device* dev) {
    if (m_failed) return false;
    if (m_dev == dev && m_cs) return true;
    Shutdown();
    ID3DBlob* code = nullptr; ID3DBlob* error = nullptr;
    if (FAILED(D3DCompile(kSampleHlsl, strlen(kSampleHlsl), "source_cadence", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) { Rel(error); m_failed = true; return false; }
    Rel(error);
    const HRESULT hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_cs);
    code->Release();
    if (FAILED(hr)) { m_failed = true; return false; }
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC ob{}; ob.ByteWidth = kPoints * 4; ob.Usage = D3D11_USAGE_DEFAULT; ob.BindFlags = D3D11_BIND_UNORDERED_ACCESS; ob.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; ob.StructureByteStride = 4;
    D3D11_BUFFER_DESC rb = ob; rb.ByteWidth = 4;
    D3D11_BUFFER_DESC sb{}; sb.ByteWidth = 4; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = kPoints;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ru = uv; ru.Buffer.NumElements = 1;
    bool ok = SUCCEEDED(dev->CreateBuffer(&cb, nullptr, &m_cb)) && SUCCEEDED(dev->CreateBuffer(&rb, nullptr, &m_result)) && SUCCEEDED(dev->CreateUnorderedAccessView(m_result, &ru, &m_resultUav));
    for (int i = 0; ok && i < 2; ++i) ok = SUCCEEDED(dev->CreateBuffer(&ob, nullptr, &m_samples[i])) && SUCCEEDED(dev->CreateUnorderedAccessView(m_samples[i], &uv, &m_sampleUav[i]));
    for (int i = 0; ok && i < kRing; ++i) ok = SUCCEEDED(dev->CreateBuffer(&sb, nullptr, &m_stage[i]));
    if (!ok) { Shutdown(); m_failed = true; return false; }
    m_dev = dev; m_written = m_read = 0; m_flip = 0;
    return true;
}

void Probe::Shutdown() {
    for (View& v : m_views) { Rel(v.srv); Rel(v.tex); v.w = v.h = 0; }
    for (ID3D11Buffer*& b : m_stage) Rel(b);
    for (int i = 0; i < 2; ++i) { Rel(m_sampleUav[i]); Rel(m_samples[i]); }
    Rel(m_resultUav); Rel(m_result); Rel(m_cb); Rel(m_cs);
    m_dev = nullptr; m_written = m_read = 0; m_flip = 0;
}

ID3D11ShaderResourceView* Probe::ViewOf(ID3D11Texture2D* frame, uint32_t* w, uint32_t* h) {
    for (View& v : m_views) if (v.tex == frame) { *w = v.w; *h = v.h; return v.srv; }
    D3D11_TEXTURE2D_DESC d; frame->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || d.SampleDesc.Count != 1) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = ViewFormat(d.Format); sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(m_dev->CreateShaderResourceView(frame, &sv, &srv))) return nullptr;
    View& slot = m_views[m_nextView]; m_nextView = (m_nextView + 1) % 8;
    Rel(slot.srv); Rel(slot.tex);
    frame->AddRef(); slot.tex = frame; slot.srv = srv; slot.w = d.Width; slot.h = d.Height;
    *w = d.Width; *h = d.Height;
    return srv;
}

int Probe::Observe(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, Detector& det) {
    if (!dev || !ctx || !frame || !Init(dev)) return 0;
    uint32_t w = 0, h = 0;
    ID3D11ShaderResourceView* srv = ViewOf(frame, &w, &h);
    if (!srv) return 0;
    // the compute state this touches, kept and put back (Lossless Scaling's own pass is about to use it)
    ID3D11ComputeShader* oldCs = nullptr; ctx->CSGetShader(&oldCs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv = nullptr; ctx->CSGetShaderResources(0, 1, &oldSrv);
    ID3D11UnorderedAccessView* oldUav[3] = {}; ctx->CSGetUnorderedAccessViews(0, 3, oldUav);
    ID3D11Buffer* oldCb = nullptr; ctx->CSGetConstantBuffers(0, 1, &oldCb);
    const uint32_t sz[4] = { w, h, 0, 0 };
    ctx->UpdateSubresource(m_cb, 0, nullptr, sz, 0, 0);
    ctx->CSSetShader(m_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    const UINT keep[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };
    ID3D11UnorderedAccessView* uavs[3] = { m_sampleUav[m_flip], m_sampleUav[m_flip ^ 1], m_resultUav };
    ctx->CSSetUnorderedAccessViews(0, 3, uavs, keep);
    ctx->CSSetConstantBuffers(0, 1, &m_cb);
    ctx->Dispatch(1, 1, 1);
    ID3D11UnorderedAccessView* noUav[3] = {}; ctx->CSSetUnorderedAccessViews(0, 3, noUav, keep);
    ctx->CopyResource(m_stage[m_written % kRing], m_result);
    ++m_written; m_flip ^= 1;
    ctx->CSSetShader(oldCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &oldSrv);
    ctx->CSSetUnorderedAccessViews(0, 3, oldUav, keep);
    ctx->CSSetConstantBuffers(0, 1, &oldCb);
    if (oldCs) oldCs->Release(); if (oldSrv) oldSrv->Release(); if (oldCb) oldCb->Release();
    for (ID3D11UnorderedAccessView* u : oldUav) if (u) u->Release();

    // the changes not yet read, oldest first, as far as the card has finished them (a busy card finishes late); too far behind: the oldest are lost and the stay in progress with them
    if (m_written - m_read >= kRing) { m_read = m_written - (kRing - 1); det.Break(); ++m_missed; }
    int pushed = 0;
    while (m_read + 1 < m_written) {   // (the newest is given a frame)
        D3D11_MAPPED_SUBRESOURCE map{};
        if (FAILED(ctx->Map(m_stage[m_read % kRing], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map))) break;
        float change; memcpy(&change, map.pData, sizeof change);
        ctx->Unmap(m_stage[m_read % kRing], 0);
        if (m_read++ == 0) continue;   // (the first frame has nothing before it)
        det.Push(change <= kSameMean); ++pushed;
    }
    return pushed;
}

} // namespace nr::cadence
