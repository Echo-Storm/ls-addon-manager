#define NOMINMAX
#include "engine/repeat_gate.h"
#include <d3dcompiler.h>
#include <windows.h>
#include <cstring>
#include <cstdlib>

namespace nr {

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Our own compare: one thread a pixel; the first thread that finds a channel further apart than the tolerance (or a NaN) sets the flag, the rest of the picture stops looking.
const char* kCompareHlsl = R"(
Texture2D<float4> a : register(t0);
Texture2D<float4> b : register(t1);
RWStructuredBuffer<uint> flag : register(u0);
cbuffer C : register(b0) { uint2 size; float tolerance; uint pad; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    if (flag[0] != 0) return;
    float3 d = abs(a.Load(int3(id.xy, 0)).rgb - b.Load(int3(id.xy, 0)).rgb);
    if (!(max(d.x, max(d.y, d.z)) <= tolerance)) flag[0] = 1;
}
)";

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    return d;
}

bool MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    const D3D12_RESOURCE_DESC d = BufferDesc(bytes, flags);
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(out)));
}

} // namespace

void RepeatGate::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    m_list->ResourceBarrier(1, &b);
}

bool RepeatGate::Init(ID3D12Device* dev, ID3D12CommandQueue* queue) {
    Shutdown();
    if (!dev || !queue) return false;
    m_dev = dev; m_queue = queue;
    D3D12_DESCRIPTOR_RANGE srv{}; srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srv.NumDescriptors = 2;
    D3D12_DESCRIPTOR_RANGE uav{}; uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = { 1, &srv };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &uav };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants.Num32BitValues = 4;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    const D3D12_ROOT_SIGNATURE_DESC rootDesc{ 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* error = nullptr;
    const bool rootOk = SUCCEEDED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) &&
                        SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_root)));
    Rel(blob); Rel(error);
    if (!rootOk) { Shutdown(); return false; }
    ID3DBlob* code = nullptr;
    if (FAILED(D3DCompile(kCompareHlsl, strlen(kCompareHlsl), "repeat_gate", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) { Rel(error); Shutdown(); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = m_root; pso.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    const bool psoOk = SUCCEEDED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(&m_pso)));
    Rel(code); Rel(error);
    if (!psoOk) { Shutdown(); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 3; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_heap)))) { Shutdown(); return false; }
    m_descriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &m_flag) ||
        !MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &m_zero) ||
        !MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &m_readback)) { Shutdown(); return false; }
    { void* p = nullptr; D3D12_RANGE none{ 0, 0 }; if (SUCCEEDED(m_zero->Map(0, &none, &p))) { memset(p, 0, 256); m_zero->Unmap(0, nullptr); } }
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc, nullptr, IID_PPV_ARGS(&m_list)))) { Shutdown(); return false; }
    m_list->Close();
    for (int i = 0; i < kCopyRing; ++i) { if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_copyAlloc[i])))) { Shutdown(); return false; } m_copyDone[i] = 0; }
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) { Shutdown(); return false; }
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_event) { Shutdown(); return false; }
    m_ready = true;
    return true;
}

void RepeatGate::Shutdown() {
    if (m_fence && m_event && m_fence->GetCompletedValue() < m_fenceValue) { m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(m_event)); WaitForSingleObject(static_cast<HANDLE>(m_event), 2000); }
    for (auto& a : m_copyAlloc) Rel(a);
    Rel(m_prev); Rel(m_list); Rel(m_alloc); Rel(m_readback); Rel(m_zero); Rel(m_flag); Rel(m_heap); Rel(m_pso); Rel(m_root); Rel(m_fence);
    if (m_event) { CloseHandle(static_cast<HANDLE>(m_event)); m_event = nullptr; }
    m_havePrev = false; m_ready = false; m_fenceValue = 0; m_dev = nullptr; m_queue = nullptr;
}

bool RepeatGate::EnsurePrev(const D3D12_RESOURCE_DESC& want) {
    D3D12_RESOURCE_DESC d = want;
    d.Flags = D3D12_RESOURCE_FLAG_NONE; d.Alignment = 0; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (m_prev && m_prevDesc.Width == d.Width && m_prevDesc.Height == d.Height && m_prevDesc.Format == d.Format && m_prevDesc.MipLevels == d.MipLevels && m_prevDesc.DepthOrArraySize == d.DepthOrArraySize) return true;
    Rel(m_prev); m_havePrev = false;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(m_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_prev)))) return false;
    m_prevDesc = d;
    return true;
}

RepeatGate::Result RepeatGate::Compare(ID3D12Resource* in, DXGI_FORMAT viewFormat, ID3D12Fence* copied, uint64_t copiedValue, float tolerance, unsigned waitMs) {
    if (!m_ready || !in) return Result::Different;
    if (m_fence->GetCompletedValue() < m_fenceValue) {   // the call before is still on the card (it timed out): its list and allocator are not free
        m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(m_event));
        if (WaitForSingleObject(static_cast<HANDLE>(m_event), waitMs) != WAIT_OBJECT_0) { ++m_timedOut; m_havePrev = false; return Result::Different; }
    }
    LARGE_INTEGER q0, q1, freq; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&freq);
    if (!EnsurePrev(in->GetDesc())) return Result::Different;
    const D3D12_RESOURCE_DESC d = in->GetDesc();
    const bool compare = m_havePrev && viewFormat != DXGI_FORMAT_UNKNOWN;
    m_alloc->Reset(); m_list->Reset(m_alloc, nullptr);
    if (compare) {
        Transition(m_flag, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->CopyBufferRegion(m_flag, 0, m_zero, 0, 4);
        Transition(m_flag, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(m_prev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = viewFormat; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        m_dev->CreateShaderResourceView(in, &sv, cpu); cpu.ptr += m_descriptorSize;
        m_dev->CreateShaderResourceView(m_prev, &sv, cpu); cpu.ptr += m_descriptorSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = 4; uv.Buffer.StructureByteStride = 4;
        m_dev->CreateUnorderedAccessView(m_flag, nullptr, &uv, cpu);
        ID3D12DescriptorHeap* heaps[] = { m_heap };
        m_list->SetDescriptorHeaps(1, heaps);
        m_list->SetComputeRootSignature(m_root);
        m_list->SetPipelineState(m_pso);
        D3D12_GPU_DESCRIPTOR_HANDLE g = m_heap->GetGPUDescriptorHandleForHeapStart(), gUav = g; gUav.ptr += 2ull * m_descriptorSize;
        m_list->SetComputeRootDescriptorTable(0, g);
        m_list->SetComputeRootDescriptorTable(1, gUav);
        uint32_t constants[4] = { static_cast<uint32_t>(d.Width), d.Height, 0, 0 }; memcpy(&constants[2], &tolerance, 4);
        m_list->SetComputeRoot32BitConstants(2, 4, constants, 0);
        m_list->Dispatch(static_cast<UINT>((d.Width + 7) / 8), (d.Height + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER ub{}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = m_flag; m_list->ResourceBarrier(1, &ub);
        Transition(m_flag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->CopyBufferRegion(m_readback, 0, m_flag, 0, 4);
        Transition(m_flag, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Transition(in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(m_prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    } else {
        Transition(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    m_list->CopyResource(m_prev, in);   // this input is the next call's "the one before"
    Transition(in, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    m_list->Close();
    if (copied) m_queue->Wait(copied, copiedValue);
    ID3D12CommandList* lists[] = { m_list };
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_fence->SetEventOnCompletion(m_fenceValue, static_cast<HANDLE>(m_event));
    const bool hadPrev = compare;
    m_havePrev = true;   // (this input is being copied in whatever comes of the wait)
    if (WaitForSingleObject(static_cast<HANDLE>(m_event), waitMs) != WAIT_OBJECT_0) {
        ++m_timedOut;
        if (getenv("EAM_GATE_FINISH")) WaitForSingleObject(static_cast<HANDLE>(m_event), 3000);   // EXPERIMENT
        return Result::Different;
    }
    QueryPerformanceCounter(&q1); m_lastMs = (q1.QuadPart - q0.QuadPart) * 1000.0 / freq.QuadPart;
    if (!hadPrev) return Result::Different;
    uint32_t flag = 1; void* p = nullptr; D3D12_RANGE r{ 0, 4 };
    if (SUCCEEDED(m_readback->Map(0, &r, &p)) && p) { flag = *static_cast<uint32_t*>(p); D3D12_RANGE none{ 0, 0 }; m_readback->Unmap(0, &none); }
    return flag == 0 ? Result::Same : Result::Different;
}

bool RepeatGate::CopyPicture(ID3D12Resource* src, ID3D12Resource* dst) {
    if (!m_ready || !src || !dst) return false;
    if (src == dst) return true;   // the same picture (a caller that writes every output into one texture): already there, and a resource cannot be copied onto itself
    const int i = m_copyNext; m_copyNext = (m_copyNext + 1) % kCopyRing;
    if (m_copyDone[i] && m_fence->GetCompletedValue() < m_copyDone[i]) {   // this allocator's last copy still on the card
        m_fence->SetEventOnCompletion(m_copyDone[i], static_cast<HANDLE>(m_event));
        if (WaitForSingleObject(static_cast<HANDLE>(m_event), 3000) != WAIT_OBJECT_0) return false;
    }
    m_copyAlloc[i]->Reset(); m_list->Reset(m_copyAlloc[i], nullptr);
    Transition(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    m_list->CopyResource(dst, src);
    Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    m_list->Close();
    ID3D12CommandList* lists[] = { m_list };
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_copyDone[i] = m_fenceValue;
    return true;
}

} // namespace nr
