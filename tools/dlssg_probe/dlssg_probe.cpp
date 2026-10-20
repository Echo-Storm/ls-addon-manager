// dlssg_probe: can NVIDIA's frame generation (DLSS-G, through Streamline's public sl_dlss_g API) be driven by a program that is not a game, with motion
// that WE supply and a constant depth plane? (Board card S8; see docs/mfg-super-motion-study.md.)
//
// A small Direct3D 12 window draws a pattern that scrolls a known number of pixels a frame. Because it is known, the motion vectors can be exact (the best case)
// or wrong on purpose, and the depth is a constant plane. Every step reports what Streamline said; once a second it reports DLSS-G's own state. With grab=FILE
// it saves what is on the screen in the window (a .bmp) after a few seconds, so that a generated frame can be compared with the true in-between picture
// (tools/dlssg_probe/check_grab.py).
//
//   dlssg_probe.exe [seconds=10] [mode=on|off] [gen=1] [speed=8] [size=1280x720] [mvec=exact|zero|wrong] [fps=60] [onlygen=0|1] [grab=file.bmp] [hws=check]
//
// Needs the Streamline SDK (addons/DLSS5NR01/external/streamline/sdk, NVIDIA's own download, not in this repository) and its plugins beside the exe (CMake copies them).
// It never edits the driver's profiles and does not change what the card reports.

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_dlss_g.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <thread>

using Microsoft::WRL::ComPtr;

static FILE* g_log = nullptr;
static void Say(const char* fmt, ...) {
    char text[1024];
    va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    printf("%s\n", text); fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", text); fflush(g_log); }
}
#define CHECK_HR(expr) do { const HRESULT hr_ = (expr); if (FAILED(hr_)) { Say("FAILED %s: 0x%08X", #expr, (unsigned)hr_); return 1; } } while (0)

static const char* SlResultName(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "eOk";
    case sl::Result::eErrorIO: return "eErrorIO";
    case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
    case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
    case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
    case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated";
    case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
    case sl::Result::eErrorAdapterNotSupported: return "eErrorAdapterNotSupported";
    case sl::Result::eErrorNoPlugins: return "eErrorNoPlugins";
    case sl::Result::eErrorVulkanAPI: return "eErrorVulkanAPI";
    case sl::Result::eErrorDXGIAPI: return "eErrorDXGIAPI";
    case sl::Result::eErrorD3DAPI: return "eErrorD3DAPI";
    case sl::Result::eErrorNRDAPI: return "eErrorNRDAPI";
    case sl::Result::eErrorNVAPI: return "eErrorNVAPI";
    case sl::Result::eErrorReflexAPI: return "eErrorReflexAPI";
    case sl::Result::eErrorNGXFailed: return "eErrorNGXFailed";
    case sl::Result::eErrorJSONParsing: return "eErrorJSONParsing";
    case sl::Result::eErrorMissingProxy: return "eErrorMissingProxy";
    case sl::Result::eErrorMissingResourceState: return "eErrorMissingResourceState";
    case sl::Result::eErrorInvalidIntegration: return "eErrorInvalidIntegration";
    case sl::Result::eErrorMissingInputParameter: return "eErrorMissingInputParameter";
    case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
    case sl::Result::eErrorComputeFailed: return "eErrorComputeFailed";
    case sl::Result::eErrorInitNotCalled: return "eErrorInitNotCalled";
    case sl::Result::eErrorExceptionHandler: return "eErrorExceptionHandler";
    case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
    case sl::Result::eErrorMissingConstants: return "eErrorMissingConstants";
    case sl::Result::eErrorDuplicatedConstants: return "eErrorDuplicatedConstants";
    case sl::Result::eErrorMissingOrInvalidAPI: return "eErrorMissingOrInvalidAPI";
    case sl::Result::eErrorCommonConstantsMissing: return "eErrorCommonConstantsMissing";
    case sl::Result::eErrorUnsupportedInterface: return "eErrorUnsupportedInterface";
    case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
    case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
    case sl::Result::eErrorFeatureMissingHooks: return "eErrorFeatureMissingHooks";
    case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
    case sl::Result::eErrorFeatureWrongPriority: return "eErrorFeatureWrongPriority";
    case sl::Result::eErrorFeatureMissingDependency: return "eErrorFeatureMissingDependency";
    case sl::Result::eErrorFeatureManagerInvalidState: return "eErrorFeatureManagerInvalidState";
    case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
    case sl::Result::eWarnOutOfVRAM: return "eWarnOutOfVRAM";
    default: return "(other)";
    }
}
#define SL_STEP(expr) [&]() { const sl::Result r_ = (expr); Say("  %-46s -> %s (%d)", #expr, SlResultName(r_), (int)r_); return r_; }()

static void SlLog(sl::LogType type, const char* msg) {
    const char* kind = type == sl::LogType::eError ? "ERROR" : type == sl::LogType::eWarn ? "warn" : "info";
    std::string line = msg ? msg : "";
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    Say("[sl %s] %s", kind, line.c_str());
}

static const char* kShader = R"(
cbuffer C : register(b0) { float shift; float w; float h; float pad; };
float4 VS(uint id : SV_VertexID, out float2 uv : TEXCOORD0) : SV_Position {
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 p = pos.xy + float2(shift, 0);
    uint2 c = uint2(floor(p / 16));
    uint hh = c.x * 374761393u + c.y * 668265263u;
    hh = (hh ^ (hh >> 13)) * 1274126177u; hh ^= hh >> 16;
    float n = (hh & 0xFFFFu) / 65535.0;
    float2 q = frac(p / 128) - 0.5;
    float ring = smoothstep(0.03, 0.0, abs(length(q) - 0.3));
    return float4(n * 0.6 + ring * 0.4, n * 0.4 + abs(q.x) * 0.5, n * 0.5 + abs(q.y) * 0.5, 1);
}
)";

struct Args {
    int seconds = 10;
    bool on = true;
    int gen = 1;
    float speed = 8.0f;
    int width = 1280, height = 720;
    std::string mvec = "exact";
    bool onlyGen = false;
    std::string grab;
    int fps = 60;   // real frames a second (Reflex's frame limiter); 0 = unlimited
};

static Args ParseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        const size_t eq = s.find('=');
        const std::string k = s.substr(0, eq), v = eq == std::string::npos ? "" : s.substr(eq + 1);
        if (k == "seconds") a.seconds = atoi(v.c_str());
        else if (k == "mode") a.on = v != "off";
        else if (k == "gen") a.gen = atoi(v.c_str());
        else if (k == "speed") a.speed = (float)atof(v.c_str());
        else if (k == "size") sscanf(v.c_str(), "%dx%d", &a.width, &a.height);
        else if (k == "mvec") a.mvec = v;
        else if (k == "onlygen") a.onlyGen = v == "1";
        else if (k == "grab") a.grab = v;
        else if (k == "fps") a.fps = atoi(v.c_str());
    }
    return a;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static bool SaveGrab(HWND hwnd, const char* path, int w, int h) {
    RECT rc{}; GetClientRect(hwnd, &rc);
    POINT tl{ 0, 0 }; ClientToScreen(hwnd, &tl);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);
    BITMAPINFOHEADER bi{ sizeof bi, w, -h, 1, 32, BI_RGB };
    std::vector<uint8_t> px((size_t)w * h * 4);
    GetDIBits(mem, bmp, 0, h, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    FILE* f = fopen(path, "wb");
    if (f) {
        BITMAPFILEHEADER fh{ 0x4D42, (DWORD)(sizeof fh + sizeof bi + px.size()), 0, 0, sizeof fh + sizeof bi };
        fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px.data(), px.size(), 1, f); fclose(f);
    }
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    return f != nullptr;
}

// A burst of small grabs (192 x 192 at 300,200 in the window) back to back, into one raw file: each is a different moment of what the screen showed, real or generated.
static bool SaveBurst(HWND hwnd, const char* path, int count) {
    POINT tl{ 0, 0 }; ClientToScreen(hwnd, &tl);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 192, 192); SelectObject(mem, bmp);
    BITMAPINFOHEADER bi{ sizeof bi, 192, -192, 1, 32, BI_RGB };
    FILE* f = fopen(path, "wb");
    std::vector<uint8_t> px(192 * 192 * 4);
    for (int i = 0; i < count && f; ++i) {
        BitBlt(mem, 0, 0, 192, 192, screen, tl.x + 300, tl.y + 200, SRCCOPY);
        GetDIBits(mem, bmp, 0, 192, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        fwrite(px.data(), px.size(), 1, f);
        Sleep(5);   // a few milliseconds apart: the burst spans a dozen or so displayed frames
    }
    if (f) fclose(f);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    return f != nullptr;
}

// Desktop Duplication: the desktop as the compositor shows it, one image each time it changes, with nothing in between lost (a GDI grab is not a reliable tap). The same
// 192 x 192 patch at 300,200 of the window, every picture the screen got over `count` pictures, into a raw file.
static bool SaveDuplication(HWND hwnd, IDXGIAdapter1* adapter, const char* path, int count) {
    POINT tl{ 0, 0 }; ClientToScreen(hwnd, &tl);
    const LONG px = tl.x + 300, py = tl.y + 200;
    ComPtr<IDXGIOutput> output;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC od{}; output->GetDesc(&od);
        if (px >= od.DesktopCoordinates.left && px < od.DesktopCoordinates.right && py >= od.DesktopCoordinates.top && py < od.DesktopCoordinates.bottom) break;
        output.Reset();
    }
    if (!output) { Say("duplication: the window is on no output of this adapter"); return false; }
    DXGI_OUTPUT_DESC od{}; output->GetDesc(&od);
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { Say("duplication: no D3D11 device"); return false; }
    ComPtr<IDXGIOutput1> o1; ComPtr<IDXGIOutputDuplication> dup;
    if (FAILED(output.As(&o1)) || FAILED(o1->DuplicateOutput(dev.Get(), &dup))) { Say("duplication: DuplicateOutput failed"); return false; }
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = 192; sd.Height = 192; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; dev->CreateTexture2D(&sd, nullptr, &staging);
    FILE* f = fopen(path, "wb");
    int got = 0, tries = 0;
    while (got < count && tries++ < count * 20 && f) {
        DXGI_OUTDUPL_FRAME_INFO info{}; ComPtr<IDXGIResource> res;
        const HRESULT hr = dup->AcquireNextFrame(100, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;
        if (info.LastPresentTime.QuadPart != 0) {
            ComPtr<ID3D11Texture2D> tex; res.As(&tex);
            { static bool said = false; if (!said) { said = true; D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td); Say("duplication: the desktop's format is %u (87 = BGRA8, 10 = RGBA16F, 24 = RGB10A2)", (unsigned)td.Format); } }
            const D3D11_BOX box{ (UINT)(px - od.DesktopCoordinates.left), (UINT)(py - od.DesktopCoordinates.top), 0, (UINT)(px - od.DesktopCoordinates.left) + 192, (UINT)(py - od.DesktopCoordinates.top) + 192, 1 };
            ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                for (UINT y = 0; y < 192; ++y) fwrite((uint8_t*)m.pData + (size_t)m.RowPitch * y, 4, 192, f);
                ctx->Unmap(staging.Get(), 0); ++got;
            }
        }
        dup->ReleaseFrame();
    }
    if (f) fclose(f);
    Say("duplication: %d pictures of the desktop saved (%d tries)", got, tries);
    return got > 0;
}

int main(int argc, char** argv) {
    SetProcessDPIAware();   // real pixels: the grab and the window size must not be scaled
    const Args args = ParseArgs(argc, argv);
    g_log = fopen("dlssg_probe.log", "w");
    Say("dlssg_probe: %dx%d, %d s, DLSS-G %s, %d generated per real frame, %.1f px a frame, motion '%s'%s", args.width, args.height, args.seconds, args.on ? "on" : "off",
        args.gen, args.speed, args.mvec.c_str(), args.onlyGen ? ", only the generated frames shown" : "");

    // ---- Streamline first, before any D3D12 / DXGI call
    wchar_t exeDir[MAX_PATH]; GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    *wcsrchr(exeDir, L'\\') = 0;
    const wchar_t* plugins[] = { exeDir };
    const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = plugins; pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = exeDir;
    pref.logMessageCallback = SlLog;
    pref.featuresToLoad = features; pref.numFeaturesToLoad = 3;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    pref.applicationId = 0;   // none: NVIDIA issues those to registered applications and this is not one
    pref.engine = sl::EngineType::eCustom;   // the documented alternative: an engine type and version, and a project GUID of our own
    pref.engineVersion = "1.0";
    pref.projectId = "51d8f732-1908-4dd7-8645-cf9976a81f91";
    if (SL_STEP(slInit(pref)) != sl::Result::eOk) return 2;

    // ---- the NVIDIA adapter
    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        char name[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        Say("adapter %u: %s (vendor %04X)", i, name, d.VendorId);
        if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
        adapter.Reset();
    }
    if (!adapter) { Say("no NVIDIA adapter"); return 3; }
    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
    sl::AdapterInfo ai{}; ai.deviceLUID = (uint8_t*)&ad.AdapterLuid; ai.deviceLUIDSizeInBytes = sizeof(LUID);
    const sl::Result supported = SL_STEP(slIsFeatureSupported(sl::kFeatureDLSS_G, ai));
    SL_STEP(slIsFeatureSupported(sl::kFeatureReflex, ai));
    {
        sl::FeatureRequirements req{};
        if (slGetFeatureRequirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk)
            Say("  requirements: flags 0x%X, os %u.%u.%u (need %u.%u.%u), driver %u.%u.%u (need %u.%u.%u)", (unsigned)req.flags, req.osVersionDetected.major, req.osVersionDetected.minor,
                req.osVersionDetected.build, req.osVersionRequired.major, req.osVersionRequired.minor, req.osVersionRequired.build, req.driverVersionDetected.major,
                req.driverVersionDetected.minor, req.driverVersionDetected.build, req.driverVersionRequired.major, req.driverVersionRequired.minor, req.driverVersionRequired.build);
    }
    if (supported != sl::Result::eOk) Say("DLSS-G reported as not supported here; going on to see what the rest says");

    // ---- device, queue, window, swap chain
    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    SL_STEP(slSetD3DDevice(device.Get()));
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));

    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"dlssg_probe"; wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    RECT wr{ 0, 0, args.width, args.height }; AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"dlssg_probe", (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE, 60, 40, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);

    constexpr UINT kBuffers = 3;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = args.width; sd.Height = args.height; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kBuffers; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1; ComPtr<IDXGISwapChain3> sc;
    CHECK_HR(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
    CHECK_HR(sc1.As(&sc));

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers };
    CHECK_HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    const UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> backBuffers[kBuffers];
    for (UINT i = 0; i < kBuffers; ++i) {
        CHECK_HR(sc->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * rtvSize;
        device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, h);
    }

    // ---- the pipeline
    ComPtr<ID3DBlob> vs, ps, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vs, &err)) || FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &ps, &err))) {
        Say("shader: %s", err ? (const char*)err->GetBufferPointer() : "?"); return 1;
    }
    D3D12_ROOT_PARAMETER rp{}; rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp.Constants.Num32BitValues = 4; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{ 1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> rsBlob; ComPtr<ID3D12RootSignature> rootSig;
    CHECK_HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err));
    CHECK_HR(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig.Get(); pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    // ---- the two inputs that stay the same every frame: depth (a constant plane) and motion vectors (what the pattern's scroll makes, in pixels, current -> previous)
    auto makeTexture = [&](DXGI_FORMAT fmt, const void* data, UINT bytesPerPixel, ComPtr<ID3D12Resource>& tex) -> HRESULT {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd{ D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, (UINT64)args.width, (UINT)args.height, 1, 1, fmt, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
        HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
        if (FAILED(hr)) return hr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows; UINT64 rowBytes, total;
        device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        ComPtr<ID3D12Resource> upload;
        hr = device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr)) return hr;
        uint8_t* mapped = nullptr; upload->Map(0, nullptr, (void**)&mapped);
        for (UINT y = 0; y < rows; ++y) for (UINT x = 0; x < (UINT)args.width; ++x) memcpy(mapped + fp.Footprint.RowPitch * y + (size_t)x * bytesPerPixel, data, bytesPerPixel);
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> cl; ComPtr<ID3D12Fence> fence;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));
        D3D12_TEXTURE_COPY_LOCATION dst{ tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Transition = { tex.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE }; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b); cl->Close();
        ID3D12CommandList* lists[] = { cl.Get() }; queue->ExecuteCommandLists(1, lists);
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        queue->Signal(fence.Get(), 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000); CloseHandle(ev);
        return S_OK;
    };
    // a half-float two-channel value from a float (enough for 0..2048)
    auto toHalf = [](float f) -> uint16_t {
        uint32_t x; memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000; int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15; uint32_t m = x & 0x7FFFFF;
        if (e <= 0) return (uint16_t)sign;
        if (e >= 31) return (uint16_t)(sign | 0x7C00);
        return (uint16_t)(sign | (e << 10) | (m >> 13));
    };
    ComPtr<ID3D12Resource> depthTex, mvecTex;
    const float depthValue = 0.5f;
    CHECK_HR(makeTexture(DXGI_FORMAT_R32_FLOAT, &depthValue, 4, depthTex));
    const float mvx = args.mvec == "zero" ? 0.0f : args.mvec == "wrong" ? -args.speed : args.speed;   // current -> previous: the content moves left, so a pixel was to the right
    const uint16_t mv[2] = { toHalf(mvx), toHalf(0.0f) };
    CHECK_HR(makeTexture(DXGI_FORMAT_R16G16_FLOAT, mv, 4, mvecTex));
    Say("depth: constant %.2f; motion vectors: %.1f, 0 pixels (current -> previous)", depthValue, mvx);

    // ---- per-frame objects
    ComPtr<ID3D12CommandAllocator> allocs[kBuffers];
    for (auto& a : allocs) CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0].Get(), pso.Get(), IID_PPV_ARGS(&cmd)));
    cmd->Close();
    ComPtr<ID3D12Fence> fence; CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 fenceValue = 0, frameFence[kBuffers] = {};
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // ---- Reflex (required by DLSS-G) and DLSS-G itself
    sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatency; ro.frameLimitUs = args.fps > 0 ? 1000000u / (uint32_t)args.fps : 0u;
    SL_STEP(slReflexSetOptions(ro));
    const sl::ViewportHandle viewport(0);
    sl::DLSSGOptions opt{};
    opt.mode = args.on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    opt.numFramesToGenerate = (uint32_t)args.gen;
    opt.numBackBuffers = kBuffers;
    opt.colorWidth = args.width; opt.colorHeight = args.height; opt.mvecDepthWidth = args.width; opt.mvecDepthHeight = args.height;
    opt.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM; opt.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT; opt.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    if (args.onlyGen) opt.flags = sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    SL_STEP(slDLSSGSetOptions(viewport, opt));
    {
        sl::DLSSGState st{};
        const sl::Result r = SL_STEP(slDLSSGGetState(viewport, st, &opt));
        if (r == sl::Result::eOk) Say("  state: status 0x%X, max frames to generate %u, min size %u, VRAM %.0f MB", (unsigned)st.status, st.numFramesToGenerateMax, st.minWidthOrHeight, st.estimatedVRAMUsageInBytes / 1048576.0);
    }

    // ---- constants that describe a camera that does not move (the motion is all in the vectors)
    auto constants = [&](bool reset) {
        sl::Constants c{};
        const float fov = 1.0f, aspect = (float)args.width / args.height, zn = 0.1f, zf = 100.0f;
        const float f = 1.0f / tanf(fov * 0.5f);
        auto rows = [](float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3, float c0, float c1, float c2, float c3, float d0, float d1, float d2, float d3) {
            sl::float4x4 m{};
            m.setRow(0, sl::float4(a0, a1, a2, a3)); m.setRow(1, sl::float4(b0, b1, b2, b3)); m.setRow(2, sl::float4(c0, c1, c2, c3)); m.setRow(3, sl::float4(d0, d1, d2, d3));
            return m;
        };
        const sl::float4x4 proj = rows(f / aspect, 0, 0, 0,  0, f, 0, 0,  0, 0, zf / (zf - zn), 1,  0, 0, -zn * zf / (zf - zn), 0);
        const sl::float4x4 inv = rows(aspect / f, 0, 0, 0,  0, 1.0f / f, 0, 0,  0, 0, 0, (zn - zf) / (zn * zf),  0, 0, 1, zf / (zn * zf));
        sl::float4x4 id{};
        for (int i = 0; i < 4; ++i) id.setRow(i, sl::float4(i == 0, i == 1, i == 2, i == 3));
        c.cameraViewToClip = proj; c.clipToCameraView = inv; c.clipToLensClip = id; c.clipToPrevClip = id; c.prevClipToClip = id;
        c.jitterOffset = { 0, 0 }; c.mvecScale = { 1.0f / args.width, 1.0f / args.height }; c.cameraPinholeOffset = { 0, 0 };
        c.cameraPos = { 0, 0, 0 }; c.cameraUp = { 0, 1, 0 }; c.cameraRight = { 1, 0, 0 }; c.cameraFwd = { 0, 0, 1 };
        c.cameraNear = zn; c.cameraFar = zf; c.cameraFOV = fov; c.cameraAspectRatio = aspect; c.motionVectorsInvalidValue = FLT_MIN;
        c.depthInverted = sl::Boolean::eFalse; c.cameraMotionIncluded = sl::Boolean::eTrue; c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.orthographicProjection = sl::Boolean::eFalse; c.motionVectorsDilated = sl::Boolean::eFalse; c.motionVectorsJittered = sl::Boolean::eFalse;
        return c;
    };

    // ---- the loop
    LARGE_INTEGER freq, t0, last; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0); last = t0;
    uint32_t frameIndex = 0, framesThisSecond = 0, secondsDone = 0;
    uint64_t realFrames = 0, errorsLogged = 0;
    bool grabbed = false, quit = false;
    std::thread burst;
    uint32_t statusSeen = 0xFFFFFFFF;
    while (!quit) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) quit = true; TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (quit) break;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double elapsed = double(now.QuadPart - t0.QuadPart) / freq.QuadPart;
        if (elapsed >= args.seconds) break;
        if (!args.grab.empty() && !grabbed && elapsed > 3.0) { grabbed = true; 
            const bool raw = args.grab.size() > 4 && args.grab.compare(args.grab.size() - 4, 4, ".raw") == 0;
            if (raw) { burst = std::thread([&] { SaveDuplication(hwnd, adapter.Get(), args.grab.c_str(), 240); }); Say("grab: burst into %s (on its own thread, while frames go on)", args.grab.c_str()); }
            else Say("grab: %s (%s)", args.grab.c_str(), SaveGrab(hwnd, args.grab.c_str(), args.width, args.height) ? "saved" : "FAILED");
        }

        sl::FrameToken* token = nullptr;
        ++frameIndex;   // (the index handed to Streamline is the one it matches the markers, constants and tags to)
        if (slGetNewFrameToken(token, &frameIndex) != sl::Result::eOk || !token) { if (errorsLogged++ < 3) Say("slGetNewFrameToken failed"); break; }
        slReflexSleep(*token);
        slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
        const float shift = frameIndex * args.speed;
        slSetConstants(constants(frameIndex == 0), *token, viewport);
        slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);

        const UINT bi = sc->GetCurrentBackBufferIndex();
        if (frameFence[bi] > fence->GetCompletedValue()) { fence->SetEventOnCompletion(frameFence[bi], fenceEvent); WaitForSingleObject(fenceEvent, 2000); }
        allocs[bi]->Reset(); cmd->Reset(allocs[bi].Get(), pso.Get());
        slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);

        // tag the inputs for this frame (the command list is still open; both stay valid until the frame is presented)
        sl::Resource depthRes(sl::ResourceType::eTex2d, depthTex.Get(), nullptr, nullptr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        sl::Resource mvecRes(sl::ResourceType::eTex2d, mvecTex.Get(), nullptr, nullptr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        sl::Extent ext{ 0, 0, (uint32_t)args.width, (uint32_t)args.height };
        sl::ResourceTag tags[] = { sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext),
                                   sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext) };
        const sl::Result tagResult = slSetTagForFrame(*token, viewport, tags, 2, cmd.Get());
        if (tagResult != sl::Result::eOk && errorsLogged++ < 3) Say("slSetTagForFrame -> %s", SlResultName(tagResult));

        D3D12_RESOURCE_BARRIER rb{}; rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition = { backBuffers[bi].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        cmd->ResourceBarrier(1, &rb);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += (SIZE_T)bi * rtvSize;
        D3D12_VIEWPORT vp{ 0, 0, (float)args.width, (float)args.height, 0, 1 }; D3D12_RECT sr{ 0, 0, args.width, args.height };
        cmd->SetGraphicsRootSignature(rootSig.Get()); cmd->RSSetViewports(1, &vp); cmd->RSSetScissorRects(1, &sr); cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float consts[4] = { shift, (float)args.width, (float)args.height, 0 };
        cmd->SetGraphicsRoot32BitConstants(0, 4, consts, 0);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cmd->DrawInstanced(3, 1, 0, 0);
        std::swap(rb.Transition.StateBefore, rb.Transition.StateAfter); cmd->ResourceBarrier(1, &rb);
        cmd->Close();
        ID3D12CommandList* lists[] = { cmd.Get() }; queue->ExecuteCommandLists(1, lists);
        slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);

        slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
        const HRESULT phr = sc->Present(0, 0);
        slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
        if (FAILED(phr)) { Say("Present failed: 0x%08X", (unsigned)phr); break; }
        queue->Signal(fence.Get(), ++fenceValue); frameFence[bi] = fenceValue;
        ++realFrames; ++framesThisSecond;

        QueryPerformanceCounter(&now);
        if (double(now.QuadPart - last.QuadPart) / freq.QuadPart >= 1.0) {
            sl::DLSSGState st{};
            const sl::Result sr2 = slDLSSGGetState(viewport, st, &opt);
            Say("t=%2us  real %u fps  presented-by-DLSS-G %u  status 0x%X%s  (state call %s)", ++secondsDone, framesThisSecond, st.numFramesActuallyPresented, (unsigned)st.status,
                st.status == sl::DLSSGStatus::eOk ? " (ok)" : "", SlResultName(sr2));
            if ((uint32_t)st.status != statusSeen) {
                statusSeen = (uint32_t)st.status;
                if (st.status & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) Say("  -> Reflex was not detected at run time");
                if (st.status & sl::DLSSGStatus::eFailCommonConstantsInvalid) Say("  -> the common constants were judged invalid");
                if (st.status & sl::DLSSGStatus::eFailResolutionTooLow) Say("  -> resolution too low");
                if (st.status & sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) Say("  -> GetCurrentBackBufferIndex not seen");
                if (st.status & sl::DLSSGStatus::eFailHDRFormatNotSupported) Say("  -> HDR format not supported");
            }
            framesThisSecond = 0; last = now;
        }
    }

    // ---- shut down in the order Streamline asks for
    if (burst.joinable()) burst.join();
    fence->SetEventOnCompletion(fenceValue, fenceEvent); WaitForSingleObject(fenceEvent, 2000);
    opt.mode = sl::DLSSGMode::eOff; slDLSSGSetOptions(viewport, opt);
    sc.Reset(); sc1.Reset();
    SL_STEP(slShutdown());
    Say("done: %llu real frames", (unsigned long long)realFrames);
    if (g_log) fclose(g_log);
    return 0;
}
