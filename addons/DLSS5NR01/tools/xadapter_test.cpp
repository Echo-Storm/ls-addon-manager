// nr_xadapter: does the plumbing for running the model on a second graphics card work? (docs/dual-gpu.md)
//
// A frame made on one card has to reach the other: a buffer in a cross-adapter heap (D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) is created on the first device, opened on the
// second, and a shared fence orders the two queues. This copies a made-up 4K half-float frame (66 MB) over it and back, checks the pixels and times it. With one card
// it uses the Microsoft software adapter (WARP) as the second, which proves the API calls and the ordering but says nothing about the bus speed.
//
//   nr_xadapter [runs=30] [mb=66] [first=<adapter index>] [second=<adapter index>|warp]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static int Arg(int argc, char** argv, const char* key, int dflt) {
    const size_t n = strlen(key);
    for (int i = 1; i < argc; ++i) if (strncmp(argv[i], key, n) == 0 && argv[i][n] == '=') return atoi(argv[i] + n + 1);
    return dflt;
}
static std::string ArgS(int argc, char** argv, const char* key) {
    const size_t n = strlen(key);
    for (int i = 1; i < argc; ++i) if (strncmp(argv[i], key, n) == 0 && argv[i][n] == '=') return argv[i] + n + 1;
    return "";
}
#define CHECK(x) do { const HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL  %s -> 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)

struct Card { ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; };

static bool MakeCard(IDXGIAdapter1* a, Card& c) {
    c.adapter = a;
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.dev)))) return false;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_COPY;   // copies only: the point is the transfer
    if (FAILED(c.dev->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)))) return false;
    if (FAILED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&c.alloc)))) return false;
    if (FAILED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, c.alloc.Get(), nullptr, IID_PPV_ARGS(&c.list)))) return false;
    c.list->Close();
    return true;
}

static ComPtr<ID3D12Resource> Buffer(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, heapFlags, &rd, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

static void Run(Card& c, ID3D12Resource* dst, ID3D12Resource* src, UINT64 bytes, ID3D12Fence* waitFence, UINT64 waitValue, ID3D12Fence* signalFence, UINT64 signalValue) {
    c.alloc->Reset(); c.list->Reset(c.alloc.Get(), nullptr);
    c.list->CopyBufferRegion(dst, 0, src, 0, bytes);
    c.list->Close();
    if (waitFence) c.queue->Wait(waitFence, waitValue);
    ID3D12CommandList* l[] = { c.list.Get() };
    c.queue->ExecuteCommandLists(1, l);
    c.queue->Signal(signalFence, signalValue);
}

static void WaitOn(ID3D12Fence* f, UINT64 v) {
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (f->GetCompletedValue() < v) { f->SetEventOnCompletion(v, e); WaitForSingleObject(e, 20000); }
    CloseHandle(e);
}

int main(int argc, char** argv) {
    const int runs = Arg(argc, argv, "runs", 30);
    const UINT64 bytes = static_cast<UINT64>(Arg(argc, argv, "mb", 66)) * 1024 * 1024;
    if (Arg(argc, argv, "debug", 0)) { ComPtr<ID3D12Debug> dbg; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    for (UINT i = 0;; ++i) { ComPtr<IDXGIAdapter1> a; if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break; adapters.push_back(a); }
    printf("adapters:\n");
    for (size_t i = 0; i < adapters.size(); ++i) { DXGI_ADAPTER_DESC1 d; adapters[i]->GetDesc1(&d); printf("  %zu  %ls  (%.1f GB%s)\n", i, d.Description, d.DedicatedVideoMemory / 1073741824.0, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software" : ""); }
    if (adapters.empty()) { printf("FAIL  no adapters\n"); return 1; }

    Card first, second;
    const int firstIndex = Arg(argc, argv, "first", 0);
    if (firstIndex < 0 || firstIndex >= static_cast<int>(adapters.size()) || !MakeCard(adapters[firstIndex].Get(), first)) { printf("FAIL  the first adapter cannot make a D3D12 device\n"); return 1; }
    const std::string secondArg = ArgS(argc, argv, "second");
    ComPtr<IDXGIAdapter1> secondAdapter;
    if (secondArg.empty() || secondArg == "warp") { CHECK(factory->EnumWarpAdapter(IID_PPV_ARGS(&secondAdapter))); }
    else secondAdapter = adapters[atoi(secondArg.c_str())];
    if (!MakeCard(secondAdapter.Get(), second)) { printf("FAIL  the second adapter cannot make a D3D12 device\n"); return 1; }
    { DXGI_ADAPTER_DESC1 a, b; first.adapter->GetDesc1(&a); second.adapter->GetDesc1(&b); printf("first: %ls\nsecond: %ls\n", a.Description, b.Description); }

    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    first.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    printf("cross-adapter row-major textures supported by the first: %s\n", o.CrossAdapterRowMajorTextureSupported ? "yes" : "no");

    // the shared memory: a heap made on the first device (a committed buffer may not be cross-adapter, only a texture can) and opened on the second, with a buffer placed in it on each
    ComPtr<ID3D12Heap> heapA;
    {
        const char* kinds[] = { "default heap", "custom heap (system memory, no CPU access)" };
        for (int kind = 0; kind < 2 && !heapA; ++kind) {
            D3D12_HEAP_DESC hd{}; hd.SizeInBytes = (bytes + 65535) & ~static_cast<UINT64>(65535); hd.Alignment = 0;
            hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
            if (kind == 0) hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            else { hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM; hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0; }
            const HRESULT hr = first.dev->CreateHeap(&hd, IID_PPV_ARGS(&heapA));
            printf("  cross-adapter heap, %s: %s (0x%08x)\n", kinds[kind], SUCCEEDED(hr) ? "made" : "refused", (unsigned)hr);
            if (FAILED(hr)) heapA.Reset();
        }
    }
    if (!heapA && Arg(argc, argv, "debug", 0)) {
        ComPtr<ID3D12InfoQueue> q;
        if (SUCCEEDED(first.dev.As(&q))) for (UINT64 i = 0; i < q->GetNumStoredMessages() && i < 6; ++i) { SIZE_T n = 0; q->GetMessage(i, nullptr, &n); std::vector<char> b(n); q->GetMessage(i, reinterpret_cast<D3D12_MESSAGE*>(b.data()), &n); printf("  layer: %s\n", reinterpret_cast<D3D12_MESSAGE*>(b.data())->pDescription); }
    }
    if (!heapA) { printf("FAIL  a cross-adapter heap cannot be made on the first card\n"); return 1; }
    HANDLE bufferHandle = nullptr;
    CHECK(first.dev->CreateSharedHandle(heapA.Get(), nullptr, GENERIC_ALL, nullptr, &bufferHandle));
    ComPtr<ID3D12Heap> heapB;
    CHECK(second.dev->OpenSharedHandle(bufferHandle, IID_PPV_ARGS(&heapB)));
    CloseHandle(bufferHandle);
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = bytes; bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ComPtr<ID3D12Resource> sharedA, sharedB;
    CHECK(first.dev->CreatePlacedResource(heapA.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sharedA)));
    CHECK(second.dev->CreatePlacedResource(heapB.Get(), 0, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sharedB)));

    // one fence, shared: it orders the two queues
    ComPtr<ID3D12Fence> fenceA, fenceB;
    CHECK(first.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&fenceA)));
    HANDLE fenceHandle = nullptr;
    CHECK(first.dev->CreateSharedHandle(fenceA.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle));
    CHECK(second.dev->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&fenceB)));
    CloseHandle(fenceHandle);
    // the second card's own fence, to know when its copy is done
    ComPtr<ID3D12Fence> doneB;
    CHECK(second.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&doneB)));
    // the first's own fence for its upload
    ComPtr<ID3D12Fence> doneA;
    CHECK(first.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&doneA)));

    ComPtr<ID3D12Resource> upload = Buffer(first.dev.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> readback = Buffer(second.dev.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!upload || !readback) { printf("FAIL  the upload or readback buffer cannot be made\n"); return 1; }

    int failures = 0;
    double totalMs = 0, worst = 0;
    for (int run = 1; run <= runs; ++run) {
        // a new pattern each run
        uint32_t* p = nullptr; D3D12_RANGE none{ 0, 0 };
        upload->Map(0, &none, reinterpret_cast<void**>(&p));
        const size_t words = static_cast<size_t>(bytes / 4);
        for (size_t i = 0; i < words; i += 4096) p[i] = static_cast<uint32_t>(i * 2654435761u) ^ static_cast<uint32_t>(run);
        upload->Unmap(0, nullptr);

        const auto t0 = std::chrono::steady_clock::now();
        // first card: the frame goes into the shared buffer; its queue signals the shared fence
        Run(first, sharedA.Get(), upload.Get(), bytes, nullptr, 0, fenceA.Get(), static_cast<UINT64>(run));
        // second card: waits for that, copies the shared buffer to its own readback memory (the model's input would be read from here), then signals its own fence
        Run(second, readback.Get(), sharedB.Get(), bytes, fenceB.Get(), static_cast<UINT64>(run), doneB.Get(), static_cast<UINT64>(run));
        WaitOn(doneB.Get(), static_cast<UINT64>(run));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        totalMs += ms; if (ms > worst) worst = ms;

        const uint32_t* r = nullptr; D3D12_RANGE all{ 0, static_cast<SIZE_T>(bytes) };
        readback->Map(0, &all, reinterpret_cast<void**>(const_cast<uint32_t**>(&r)));
        bool ok = true;
        for (size_t i = 0; i < words; i += 4096) if (r[i] != (static_cast<uint32_t>(i * 2654435761u) ^ static_cast<uint32_t>(run))) { ok = false; break; }
        readback->Unmap(0, &none);
        if (!ok) ++failures;
    }
    printf("%d runs of %.0f MB: %.2f ms on average (%.1f GB/s), worst %.2f ms, %d with wrong pixels\n", runs, bytes / 1048576.0, totalMs / runs, (bytes / 1073741824.0) / (totalMs / runs / 1000.0), worst, failures);
    printf("%s\n", failures ? "CROSS-ADAPTER TEST FAILED" : "CROSS-ADAPTER TEST PASSED");
    return failures ? 1 : 0;
}
