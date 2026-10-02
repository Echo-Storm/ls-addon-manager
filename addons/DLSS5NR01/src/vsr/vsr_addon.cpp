// VSRUPSC: Video Super Resolution (prototype). NVIDIA's RTX Video Super Resolution, from the person's own NVIDIA Video Effects SDK (external\vfx, or the folder named by the setting
// vfxDir), in place of Lossless Scaling's NIS pass (issue #8: "only VSR gives the look I want").
//
// Lossless Scaling scales every frame it presents with one compute pass (NIS, when that is the Scaling Type). The pass is recognised from what is bound to it (nis_pass.cpp, the same
// code the other upscalers use), skipped, and the picture VSR makes from the pass's input goes into its output, from the input viewport to the output viewport:
//   1. the grab pass reads the frame through the NIS pass's own t0 view into an 8-bit texture (an HDR frame as its SDR view, engine/hdr_hlsl.h);
//   2. the SDK's own transfer helpers take that texture to a CUDA buffer, the effect runs, and they bring the result back into a texture;
//   3. the place pass writes the result into the NIS pass's output (back into the frame's encoding).
// All of it on Lossless Scaling's own context, synchronously (the prototype's first question is what that costs and whether Lossless Scaling minds; a device and a thread of its own, the way
// the other upscalers work, is the next step if it does). Off unless the setting enabled is 1 (a prototype). Any failure hands the pass back to NIS and says so in the log.
// NVIDIA's SDK is the person's own download (its licence): nothing of it is in this repository or shipped.
#include <eam/addon_exports.h>
#include <eam/events.h>
#include <imgui.h>
#include <eam/widgets.h>
#include "addon/scaler11.h"
#include "addon/hdr.h"
#include "engine/hdr_hlsl.h"
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "nvTransferD3D11.h"
#include "nvVFXVideoSuperRes.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

char* g_nvVFXSDKPath = nullptr;   // (the SDK's proxy asks for it: where NVVideoEffects.dll is)

namespace {

constexpr const char* kId = "VSRUPSC";
constexpr const char* kName = "Video Super Resolution (prototype)";
constexpr const char* kVersion = "0.0.1";

IHost* g_host = nullptr;
std::wstring g_addonDir;
bool g_enabled = false, g_failed = false, g_sdkLoaded = false;
uint64_t g_configAt = 0;
int g_quality = 1;                // 1 Low .. 4 Ultra (VSR_Bicubic is 0)
std::string g_vfxDir;

std::mutex g_textMutex;
std::string g_notice, g_liveStatus;   // for the panel: the last thing that went wrong, and the live line

void Log(const char* fmt, ...) {
    char text[600]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    if (g_host) g_host->Log(EAM_LOG_INFO, (std::string(kId) + ": " + text).c_str());
}
// A line that says why VSR is not running: logged, and shown in the panel.
void Problem(const char* fmt, ...) {
    char text[600]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    if (g_host) g_host->Log(EAM_LOG_WARN, (std::string(kId) + ": " + text).c_str());
    std::lock_guard<std::mutex> lock(g_textMutex); g_notice = text;
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring FolderOf(HMODULE module) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(module, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    return path;
}
std::string Narrow(const std::wstring& w) { if (w.empty()) return ""; std::string s(w.size() * 3, '\0'); s.resize(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr)); return s; }

uint64_t NowMs() { return GetTickCount64(); }

// The pass's own bindings are taken off while our work runs and put back after, so the passes that follow find what they left (as scaler11.cpp does).
struct SavedBindings {
    static const UINT kSrvs = 8, kUavs = 4;
    ID3D11DeviceContext* ctx;
    ID3D11ShaderResourceView* srvs[kSrvs] = {}; ID3D11UnorderedAccessView* uavs[kUavs] = {};
    ID3D11ComputeShader* shader = nullptr; ID3D11Buffer* cb = nullptr;
    explicit SavedBindings(ID3D11DeviceContext* c) : ctx(c) {
        ctx->CSGetShaderResources(0, kSrvs, srvs); ctx->CSGetUnorderedAccessViews(0, kUavs, uavs); ctx->CSGetShader(&shader, nullptr, nullptr); ctx->CSGetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* noSrvs[kSrvs] = {}; ID3D11UnorderedAccessView* noUavs[kUavs] = {};
        ctx->CSSetShaderResources(0, kSrvs, noSrvs); ctx->CSSetUnorderedAccessViews(0, kUavs, noUavs, nullptr);
    }
    ~SavedBindings() {
        ctx->CSSetShader(shader, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetShaderResources(0, kSrvs, srvs); ctx->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        for (auto*& v : srvs) SafeRelease(v);
        for (auto*& v : uavs) SafeRelease(v);
        SafeRelease(shader); SafeRelease(cb);
    }
};

const char* const kGrabHlsl = R"HLSL(
Texture2D<float4>   tFrame : register(t0);
RWTexture2D<float4> uOut   : register(u0);
cbuffer C : register(b0) { uint2 origin; uint encoding; float white; };
[numthreads(8, 8, 1)]
void CSGrab(uint3 id : SV_DispatchThreadID) {
    uint w, h; uOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 c = tFrame.Load(int3(id.xy + origin, 0));
    uOut[id.xy] = float4(saturate(ToSdr(c.rgb, encoding, white)), 1.0);
}
)HLSL";

const char* const kPlaceHlsl = R"HLSL(
Texture2D<float4>   tPicture : register(t0);
RWTexture2D<float4> uOut     : register(u0);
cbuffer C : register(b0) { uint2 origin; uint2 size; uint encoding; float white; float2 unused; };
[numthreads(8, 8, 1)]
void CSPlace(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float4 p = tPicture.Load(int3(id.xy, 0));
    uOut[id.xy + origin] = float4(FromSdr(saturate(p.rgb), encoding, white), 1.0);
}
)HLSL";

// Everything made on one of Lossless Scaling's devices (it makes more than one, and replaces them): the shaders, the textures the SDK's transfers use, and the effect for these sizes.
struct Chain {
    ID3D11Device* dev = nullptr;   // only compared
    ID3D11ComputeShader* grab = nullptr, * place = nullptr; ID3D11Buffer* grabCb = nullptr, * placeCb = nullptr;
    ID3D11Texture2D* texIn = nullptr, * texOut = nullptr; ID3D11UnorderedAccessView* inUav = nullptr; ID3D11ShaderResourceView* outSrv = nullptr;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0; int quality = -1;
    NvVFX_Handle fx = nullptr; NvCVImage gIn{}, gOut{}, dIn{}, dOut{}; CUstream stream = nullptr;
    bool effectLoaded = false;
    double sumMs = 0; int n = 0; uint64_t statusAt = 0;
    void ReleaseSizes() {
        if (fx) { NvVFX_DestroyEffect(fx); fx = nullptr; }
        NvCVImage_Dealloc(&gIn); NvCVImage_Dealloc(&gOut); gIn = {}; gOut = {}; dIn = {}; dOut = {};
        SafeRelease(inUav); SafeRelease(outSrv); SafeRelease(texIn); SafeRelease(texOut);
        inW = inH = outW = outH = 0; quality = -1; effectLoaded = false;
    }
    void Release() { ReleaseSizes(); SafeRelease(grab); SafeRelease(place); SafeRelease(grabCb); SafeRelease(placeCb); dev = nullptr; }
};
// (made after the SDK is loaded, never as a global: NvCVImage's constructors call the SDK's loader stub, which keeps its first failure for good if it is asked before the folder is known)
Chain* g_chainPtr = nullptr;
Chain& ChainRef() { if (!g_chainPtr) g_chainPtr = new Chain(); return *g_chainPtr; }

bool CompileShader(ID3D11Device* dev, const char* body, const char* entry, ID3D11ComputeShader** out) {
    ID3DBlob* code = nullptr, * error = nullptr;
    const std::string source = std::string(NR_HDR_HLSL) + body;
    if (FAILED(D3DCompile(source.c_str(), source.size(), entry, nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
        Log("the %s shader: %s", entry, error ? static_cast<const char*>(error->GetBufferPointer()) : "?"); SafeRelease(error); return false;
    }
    const HRESULT hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    code->Release();
    return SUCCEEDED(hr);
}
ID3D11Buffer* MakeCb(ID3D11Device* dev, UINT bytes) {
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = bytes; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr; dev->CreateBuffer(&cb, nullptr, &b); return b;
}
ID3D11Texture2D* MakeTex(ID3D11Device* dev, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    ID3D11Texture2D* t = nullptr; dev->CreateTexture2D(&d, nullptr, &t); return t;
}

const char* NvError(NvCV_Status s, const char* what) { static char t[200]; snprintf(t, sizeof t, "%s: %s (%d)", what, NvCV_GetErrorStringFromCode(s), static_cast<int>(s)); return t; }

bool LoadSdk() {
    if (g_sdkLoaded) return true;
    std::string vfx = g_vfxDir;
    for (char& c : vfx) if (c == '/') c = '\\';
    while (!vfx.empty() && vfx.back() == '\\') vfx.pop_back();
    const std::string bin = vfx + "\\bin";
    for (const char* dll : { "NVCVImage.dll", "NVVideoEffects.dll" }) {
        if (!LoadLibraryExA((bin + "\\" + dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) { Problem("cannot load %s\\%s (Windows error %lu; is vfxDir the x64 Video Effects SDK, with CUDA 13 support in the driver?)", bin.c_str(), dll, GetLastError()); return false; }
    }
    static std::string binStatic; binStatic = bin; g_nvVFXSDKPath = binStatic.data();
    SetDllDirectoryA(bin.c_str());   // (the SDK's proxies load the rest by name)
    for (const std::string& d : { bin, vfx + "\\features\\nvvfxvideosuperres\\bin" }) {   // (and for a process that limits where DLLs are searched for: Windows keeps these for LoadLibrary by name)
        std::wstring w(d.begin(), d.end()); AddDllDirectory(w.c_str());
    }
    _putenv_s("NV_VIDEO_EFFECTS_PATH", "USE_APP_PATH");
    { const char* old = getenv("PATH"); const std::string path = bin + ";" + vfx + "\\features\\nvvfxvideosuperres\\bin;" + (old ? old : ""); _putenv_s("PATH", path.c_str()); }
    g_sdkLoaded = true;
    Log("the Video Effects SDK in %s is loaded", vfx.c_str());
    return true;
}

// The effect and the textures for these sizes (rebuilt when the sizes, the quality or the device change).
bool Fit(ID3D11Device* dev, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    Chain& g_chain = ChainRef();
    if (g_chain.dev != dev) { g_chain.Release(); g_chain.dev = dev; }
    if (!g_chain.grab) {
        g_chain.grabCb = MakeCb(dev, 16); g_chain.placeCb = MakeCb(dev, 32);
        if (!g_chain.grabCb || !g_chain.placeCb || !CompileShader(dev, kGrabHlsl, "CSGrab", &g_chain.grab) || !CompileShader(dev, kPlaceHlsl, "CSPlace", &g_chain.place)) { Problem("the shaders could not be made"); return false; }
    }
    if (g_chain.inW == inW && g_chain.inH == inH && g_chain.outW == outW && g_chain.outH == outH && g_chain.quality == g_quality && g_chain.effectLoaded) return true;
    g_chain.ReleaseSizes();
    const auto t0 = std::chrono::steady_clock::now();
    if (!g_chain.stream) { if (NvCV_Status s = NvVFX_CudaStreamCreate(&g_chain.stream)) { Problem("%s", NvError(s, "NvVFX_CudaStreamCreate")); return false; } }
    g_chain.texIn = MakeTex(dev, inW, inH); g_chain.texOut = MakeTex(dev, outW, outH);
    if (!g_chain.texIn || !g_chain.texOut) { Problem("the textures could not be made (%ux%u, %ux%u)", inW, inH, outW, outH); g_chain.ReleaseSizes(); return false; }
    if (FAILED(dev->CreateUnorderedAccessView(g_chain.texIn, nullptr, &g_chain.inUav)) || FAILED(dev->CreateShaderResourceView(g_chain.texOut, nullptr, &g_chain.outSrv))) { Problem("the views could not be made"); g_chain.ReleaseSizes(); return false; }
    NvCV_Status s = NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &g_chain.fx);
    if (s != NVCV_SUCCESS) { Problem("%s", NvError(s, "NvVFX_CreateEffect(VideoSuperRes) (is the VideoSuperRes feature installed?)")); g_chain.ReleaseSizes(); return false; }
    NvVFX_SetCudaStream(g_chain.fx, NVVFX_CUDA_STREAM, g_chain.stream);
    const std::string models = g_vfxDir + "\\bin\\models"; NvVFX_SetString(g_chain.fx, NVVFX_MODEL_DIRECTORY, models.c_str());
    NvVFX_SetU32(g_chain.fx, NVVFX_QUALITY_LEVEL, static_cast<unsigned>(g_quality));
    if ((s = NvCVImage_Alloc(&g_chain.gIn, inW, inH, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1)) != NVCV_SUCCESS || (s = NvCVImage_Alloc(&g_chain.gOut, outW, outH, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1)) != NVCV_SUCCESS) { Problem("%s", NvError(s, "the CUDA buffers")); g_chain.ReleaseSizes(); return false; }
    NvVFX_SetImage(g_chain.fx, NVVFX_INPUT_IMAGE, &g_chain.gIn); NvVFX_SetImage(g_chain.fx, NVVFX_OUTPUT_IMAGE, &g_chain.gOut);
    if ((s = NvVFX_Load(g_chain.fx)) != NVCV_SUCCESS) { Problem("%s", NvError(s, "NvVFX_Load")); g_chain.ReleaseSizes(); return false; }
    if ((s = NvCVImage_InitFromD3D11Texture(&g_chain.dIn, g_chain.texIn)) != NVCV_SUCCESS || (s = NvCVImage_InitFromD3D11Texture(&g_chain.dOut, g_chain.texOut)) != NVCV_SUCCESS) { Problem("%s", NvError(s, "InitFromD3D11Texture")); g_chain.ReleaseSizes(); return false; }
    g_chain.inW = inW; g_chain.inH = inH; g_chain.outW = outW; g_chain.outH = outH; g_chain.quality = g_quality; g_chain.effectLoaded = true;
    Log("VSR quality %d for %ux%u to %ux%u is ready (%.0f ms to build)", g_quality, inW, inH, outW, outH, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

void ReadConfig() {
    const uint64_t now = NowMs();
    if (g_configAt && now - g_configAt < 1000) return;
    g_configAt = now;
    g_enabled = atoi(g_host->GetConfig(kId, "enabled", "0")) != 0;
    g_quality = std::clamp(atoi(g_host->GetConfig(kId, "quality", "1")), 0, 23);
    g_vfxDir = g_host->GetConfig(kId, "vfxDir", "");
    if (g_vfxDir.empty()) g_vfxDir = Narrow(g_addonDir) + "\\vfx";
}

// One NIS pass: the frame goes through VSR into the pass's output. False: nothing was done (NIS then runs).
bool RunVsr(ID3D11DeviceContext* ctx, const nr::NisPass& pass) {
    if (pass.outW <= pass.inW && pass.outH <= pass.inH) return false;   // 1:1 or smaller: not an upscale (VSR scales up)
    if (pass.inW < 64 || pass.inH < 64) return false;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev); if (dev) dev->Release();   // (only compared and used on this thread, while the context lives)
    if (!dev) return false;
    if (!LoadSdk()) { g_failed = true; return false; }
    if (!Fit(dev, pass.inW, pass.inH, pass.outW, pass.outH)) { g_failed = true; return false; }

    Chain& g_chain = ChainRef();
    const auto t0 = std::chrono::steady_clock::now();
    const nr::DisplayHdr display = nr::QueryDisplayHdr(nullptr, dev);
    const nr::FrameEncoding enc = nr::EncodingOf(pass.inFmt, 0, display.hdr);
    const float white = display.whiteNits;
    SavedBindings saved(ctx);
    ID3D11ShaderResourceView* frameSrv = saved.srvs[0]; ID3D11UnorderedAccessView* outUav = saved.uavs[0];
    if (!frameSrv || !outUav) return false;

    // 1. the grab
    { D3D11_MAPPED_SUBRESOURCE m{}; if (FAILED(ctx->Map(g_chain.grabCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
      uint32_t* c = static_cast<uint32_t*>(m.pData); c[0] = pass.inX; c[1] = pass.inY; c[2] = static_cast<uint32_t>(enc); float w = white; memcpy(&c[3], &w, 4); ctx->Unmap(g_chain.grabCb, 0); }
    ctx->CSSetShader(g_chain.grab, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &g_chain.grabCb);
    ctx->CSSetShaderResources(0, 1, &frameSrv); ctx->CSSetUnorderedAccessViews(0, 1, &g_chain.inUav, nullptr);
    ctx->Dispatch((pass.inW + 7) / 8, (pass.inH + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrv = nullptr; ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &noSrv); ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);

    // 2. the SDK: texture to CUDA, VSR, CUDA to texture
    NvCV_Status s = NvCVImage_Transfer(&g_chain.dIn, &g_chain.gIn, 1.0f, g_chain.stream, nullptr);
    if (s == NVCV_SUCCESS) s = NvVFX_Run(g_chain.fx, 0);
    if (s == NVCV_SUCCESS) s = NvCVImage_Transfer(&g_chain.gOut, &g_chain.dOut, 1.0f, g_chain.stream, nullptr);
    if (s != NVCV_SUCCESS) { Problem("%s", NvError(s, "the VSR chain")); g_failed = true; return false; }
    { NvCVImage corner{}, cornerCpu{}; unsigned char px[16 * 16 * 4]; NvCVImage_InitView(&corner, &g_chain.gOut, 0, 0, 16, 16);   // (waits for the stream)
      NvCVImage_Init(&cornerCpu, 16, 16, 16 * 4, px, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU); NvCVImage_Transfer(&corner, &cornerCpu, 1.0f, g_chain.stream, nullptr); }

    // 3. the place
    { D3D11_MAPPED_SUBRESOURCE m{}; if (FAILED(ctx->Map(g_chain.placeCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
      uint32_t* c = static_cast<uint32_t*>(m.pData); c[0] = pass.outX; c[1] = pass.outY; c[2] = pass.outW; c[3] = pass.outH; c[4] = static_cast<uint32_t>(enc); float w = white; memcpy(&c[5], &w, 4); c[6] = c[7] = 0; ctx->Unmap(g_chain.placeCb, 0); }
    ctx->CSSetShader(g_chain.place, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &g_chain.placeCb);
    ctx->CSSetShaderResources(0, 1, &g_chain.outSrv); ctx->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
    ctx->Dispatch((pass.outW + 7) / 8, (pass.outH + 7) / 8, 1);
    ctx->CSSetShaderResources(0, 1, &noSrv); ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_chain.sumMs += ms; ++g_chain.n;
    const uint64_t now = NowMs();
    if (now - g_chain.statusAt > 1000 && g_chain.n) {
        g_chain.statusAt = now;
        char t[160]; snprintf(t, sizeof t, "VSR quality %d, %ux%u to %ux%u: %.1f ms a frame on the render thread (%d frames)", g_quality, pass.inW, pass.inH, pass.outW, pass.outH, g_chain.sumMs / g_chain.n, g_chain.n);
        g_host->SetStatus(kId, t, 1); { std::lock_guard<std::mutex> lock(g_textMutex); g_liveStatus = t; g_notice.clear(); }
        if (g_chain.n >= 300) { Log("%s", t); g_chain.sumMs = 0; g_chain.n = 0; }
    }
    return true;
}

bool GuardedRun(ID3D11DeviceContext* ctx, const nr::NisPass& pass, DWORD* code) {   // (no objects with destructors here: __try needs that)
    __try { return RunVsr(ctx, pass); }
    __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring TrialMarker() { return g_addonDir + L"\\running.txt"; }

bool OnPass(uint32_t x, uint32_t y, uint32_t z, void*) {
    if (g_failed) return false;
    ReadConfig();
    if (!g_enabled) return false;
    auto* ctx = static_cast<ID3D11DeviceContext*>(g_host->GetDispatchingContext());
    if (!ctx) return false;
    nr::NisPass pass;
    if (!nr::FindNisPass(ctx, x, y, z, pass, [](const char* t) { Log("%s", t); })) return false;
    DWORD code = 0;
    static bool marked = false;
    if (!marked) { marked = true; FILE* f = nullptr; if (_wfopen_s(&f, TrialMarker().c_str(), L"wb") == 0 && f) { fputs("running", f); fclose(f); } }   // (found at the next start: the last session did not end well)
    const bool done = GuardedRun(ctx, pass, &code);
    if (code) { Problem("the VSR chain raised exception 0x%08lx: VSR stays off until Lossless Scaling restarts", code); g_failed = true; }
    nr::ReleaseNisPass(pass);
    return done;
}

void OnDeviceEvent(uint32_t, const void*, uint32_t, void*) {
    nr::ResetNisViewports();
    if (g_chainPtr) g_chainPtr->Release();   // (the textures belonged to a device that may be gone: the next pass makes them on the new one)
}

}   // namespace

EAM_EXPORT void AddonInitialize(IHost* host, ImGuiContext* ctx, void* allocFunc, void* freeFunc, void* userData) {
    ImGui::SetCurrentContext(ctx);
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFunc), reinterpret_cast<ImGuiMemFreeFunc>(freeFunc), userData);
    eam::ui::InitAddonImGui();
    g_host = host;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&AddonInitialize), &self);
    g_addonDir = FolderOf(self);
    g_failed = false; g_sdkLoaded = false; g_configAt = 0;
    ReadConfig();
    if (GetFileAttributesW(TrialMarker().c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_failed = true;
        Log("the last session did not end normally with VSR running (running.txt is in the addon folder): VSR stays off. Delete that file to try again.");
    }
    host->SubscribeEvent(EAM_EVENT_D3D11_DEVICE_READY, OnDeviceEvent, nullptr);
    host->SubscribeEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, OnDeviceEvent, nullptr);
    if (host->GetHostVersion() >= 0x010100) host->SetPreDispatchCallback(OnPass, nullptr);
    else { g_failed = true; Log("needs LS Addon Manager with addon API 1.1 or newer"); }
    Log("%s %s initialised (enabled %d, quality %d, SDK folder %s)", kName, kVersion, g_enabled ? 1 : 0, g_quality, g_vfxDir.c_str());
    host->SetStatus(kId, g_enabled ? "Prototype: on, waits for a NIS pass" : "Prototype: off (set enabled to 1)", 0);
}

EAM_EXPORT void AddonShutdown() {
    if (g_host) { g_host->SetPreDispatchCallback(nullptr, nullptr); g_host->UnsubscribeEvent(EAM_EVENT_D3D11_DEVICE_READY, OnDeviceEvent); g_host->UnsubscribeEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, OnDeviceEvent); }
    if (g_chainPtr) { g_chainPtr->Release(); if (g_chainPtr->stream) { NvVFX_CudaStreamDestroy(g_chainPtr->stream); g_chainPtr->stream = nullptr; } }
    DeleteFileW(TrialMarker().c_str());   // a normal close: the marker is not a crash
    g_host = nullptr;
}

EAM_EXPORT void AddonRenderSettings() {
    ReadConfig();
    ImGui::TextWrapped("Prototype. NVIDIA's RTX Video Super Resolution, from your own copy of the NVIDIA Video Effects SDK, in place of Lossless Scaling's NIS scaling: a trained network that sharpens the picture it upscales, one frame at a time. Nothing of NVIDIA's comes with this addon.");
    ImGui::Spacing();
    bool enabled = g_enabled;
    if (ImGui::Checkbox("Use Video Super Resolution", &enabled)) { g_host->SetConfig(kId, "enabled", enabled ? "1" : "0"); g_host->SaveConfig(); g_configAt = 0; g_enabled = enabled; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("On, the addon takes Lossless Scaling's NIS pass and puts VSR's picture in its place. Off, nothing changes.");
    static const char* kQualities[] = { "Bicubic (no AI, to compare)", "Low (recommended)", "Medium", "High", "Ultra" };
    int q = std::clamp(g_quality, 0, 4);
    if (g_quality <= 4 && ImGui::Combo("Quality", &q, kQualities, 5)) { char t[16]; snprintf(t, sizeof t, "%d", q); g_host->SetConfig(kId, "quality", t); g_host->SaveConfig(); g_configAt = 0; g_quality = q; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Low and Medium cost 2 to 3 ms a frame at 4K and look as good as High and Ultra in the tests; High and Ultra cost 6 to 8 ms and shimmer more.");
    static char folder[520]; static bool folderInit = false;
    if (!folderInit) { strncpy_s(folder, g_vfxDir.c_str(), sizeof folder - 1); folderInit = true; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##vfxdir", folder, sizeof folder, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) { g_host->SetConfig(kId, "vfxDir", folder); g_host->SaveConfig(); g_configAt = 0; }
    ImGui::TextDisabled("The folder of your NVIDIA Video Effects SDK (x64), with the Video Super Resolution feature installed. A change of folder takes effect after Lossless Scaling restarts.");
    ImGui::Spacing();
    { std::lock_guard<std::mutex> lock(g_textMutex);
      if (!g_liveStatus.empty()) ImGui::TextWrapped("%s", g_liveStatus.c_str());
      else ImGui::TextDisabled(g_enabled ? "Waiting for a NIS pass (choose NIS as the Scaling Type, and a window smaller than the screen)." : "Off.");
      if (!g_notice.empty()) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "%s", g_notice.c_str()); }
    if (g_failed && ImGui::Button("Try again")) {
        if (GetFileAttributesW(TrialMarker().c_str()) != INVALID_FILE_ATTRIBUTES) DeleteFileW(TrialMarker().c_str());
        g_failed = false; { std::lock_guard<std::mutex> lock(g_textMutex); g_notice.clear(); }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Works with: NIS as the Scaling Type, the game in a window smaller than the screen. It replaces the DLSS, FSR and XeSS upscalers (only one can take the NIS pass). A frame is 2 to 4 ms of the render thread at 4K; the picture is as steady as bicubic plus about a quarter more shimmer.");
}
EAM_EXPORT uint32_t GetAddonCapabilities() { return EAM_CAP_HAS_SETTINGS | EAM_CAP_D3D11_DEVICE_ACCESS | EAM_CAP_DISPATCH_HOOK; }
EAM_EXPORT const char* GetAddonName() { return kName; }
EAM_EXPORT const char* GetAddonVersion() { return kVersion; }
EAM_EXPORT const char* GetAddonAuthor() { return "Echo-Storm"; }
EAM_EXPORT const char* GetAddonDescription() { return "Prototype. NVIDIA RTX Video Super Resolution (from your own NVIDIA Video Effects SDK) in place of Lossless Scaling's NIS pass. Off until the setting enabled is 1."; }
