// nr_composebench: the GPU time of Neural Rendering's compose pass (Compose11, src/addon/compose11.cpp), run back to back on a 4K frame so the card stays at full
// clocks (the times in a live log or a host-test scenario are taken with the GPU idling between frames and vary a lot).
//
//   nr_composebench [size=3840x2160] [runs=300] [encoding=0|1|2] [sharpen=30] [delta=0.3] [dump=file]
//
// The frame is a made-up fp16 picture (scRGB light with bright glints; SDR for encoding 0), the delta a made-up small signed picture at `delta` of the frame's size. One line per
// setting: the average GPU ms of one Run, and the check sum of the result (the same shader change must keep it).
#include <windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "addon/compose11.h"
#include "addon/bridge.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// (Compose11 names the texture format a buffer is viewed as through the bridge; the bench has no bridge, only this)
DXGI_FORMAT Bridge::ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static int ArgInt(int argc, char** argv, const char* key, int fallback) {
    const size_t k = strlen(key);
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return atoi(argv[i] + k + 1);
    return fallback;
}
static uint32_t Hash(uint32_t x, uint32_t y) { uint32_t h = x * 374761393u + y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }

int main(int argc, char** argv) {
    uint32_t W = 3840, H = 2160;
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "size=", 5)) sscanf_s(argv[i] + 5, "%ux%u", &W, &H);
    const int runs = std::max(10, ArgInt(argc, argv, "runs", 300));
    const int encoding = ArgInt(argc, argv, "encoding", 1);
    const float sharpen = ArgInt(argc, argv, "sharpen", 30) / 100.0f;
    const float deltaScale = ArgInt(argc, argv, "delta", 30) / 100.0f;

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) { printf("no Direct3D 11 device\n"); return 4; }
    Compose11 compose;
    LARGE_INTEGER q0, q1, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    if (!compose.Init(dev, [](const char* m) { printf("  %s\n", m); })) { printf("the compose pass could not start\n"); return 4; }
    QueryPerformanceCounter(&q1);
    printf("Compose11::Init (the shader compiled): %.0f ms\n", double(q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart);

    const bool hdr = encoding != 0;
    const DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * (hdr ? 8 : 4));
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
        const uint32_t c = Hash(x / 6, y / 6);
        float rgb[3] = { 0.03f + 0.7f * ((c & 255) / 255.0f), 0.03f + 0.7f * (((c >> 8) & 255) / 255.0f), 0.03f + 0.7f * (((c >> 16) & 255) / 255.0f) };
        if (Hash(x / 11 + 5, y / 11 + 9) % 37u == 0) rgb[0] = rgb[1] = rgb[2] = 1.5f + 0.1f * (c % 8u);
        if (hdr) { uint16_t* p = reinterpret_cast<uint16_t*>(px.data()) + (static_cast<size_t>(y) * W + x) * 4; for (int k = 0; k < 3; ++k) p[k] = DirectX::PackedVector::XMConvertFloatToHalf(rgb[k] * 2.5f); p[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f); }
        else { uint8_t* p = px.data() + (static_cast<size_t>(y) * W + x) * 4; for (int k = 0; k < 3; ++k) p[k] = static_cast<uint8_t>(std::min(1.0f, rgb[k]) * 255.0f); p[3] = 255; }
    }
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), static_cast<UINT>(W * (hdr ? 8 : 4)), 0 };
    ID3D11Texture2D* target = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &target))) { printf("the target could not be made\n"); return 4; }

    const uint32_t dw = std::max(16u, static_cast<uint32_t>(W * deltaScale)), dh = std::max(16u, static_cast<uint32_t>(H * deltaScale));
    std::vector<uint16_t> dpx(static_cast<size_t>(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) for (int k = 0; k < 4; ++k)
        dpx[(static_cast<size_t>(y) * dw + x) * 4 + k] = DirectX::PackedVector::XMConvertFloatToHalf(k == 3 ? 0.0f : (static_cast<int>(Hash(x + k, y) % 2001u) - 1000) / 1000.0f * 0.04f);
    D3D11_TEXTURE2D_DESC dd = td; dd.Width = dw; dd.Height = dh; dd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA dsd{ dpx.data(), static_cast<UINT>(dw * 8), 0 };
    ID3D11Texture2D* deltaTex = nullptr; ID3D11ShaderResourceView* deltaView = nullptr;
    if (FAILED(dev->CreateTexture2D(&dd, &dsd, &deltaTex)) || FAILED(dev->CreateShaderResourceView(deltaTex, nullptr, &deltaView))) { printf("the delta could not be made\n"); return 4; }

    Compose11::Args a;
    a.target = target; a.delta = deltaView; a.offset = 0.0f; a.intensity = 1.0f; a.maxDelta = 0.5f; a.hiProtect = 0.85f; a.sharpen = sharpen;
    a.saturation = 1.0f; a.vibrance = 0.15f; a.gamma = 1.0f; a.encoding = static_cast<uint32_t>(encoding); a.whiteNits = 240.0f;
    a.ghostGuard = 0.0f;
    // viewport=x,y,w,h (percent of the target): the frame is drawn only there (Lossless Scaling's bars on an ultrawide screen); barcheck=1 then runs once and counts what changed inside and outside it
    int vp[4] = { 0, 0, 100, 100 };
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "viewport=", 9)) sscanf_s(argv[i] + 9, "%d,%d,%d,%d", &vp[0], &vp[1], &vp[2], &vp[3]);
    for (int k = 0; k < 4; ++k) a.viewport[k] = vp[k] / 100.0f;
    if (ArgInt(argc, argv, "barcheck", 0) != 0) {
        D3D11_TEXTURE2D_DESC sd2 = td; sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* before = nullptr; ID3D11Texture2D* after = nullptr; dev->CreateTexture2D(&sd2, nullptr, &before); dev->CreateTexture2D(&sd2, nullptr, &after);
        ctx->CopyResource(before, target); compose.Run(ctx, a); ctx->CopyResource(after, target);
        D3D11_MAPPED_SUBRESOURCE mb{}, ma{};
        if (FAILED(ctx->Map(before, 0, D3D11_MAP_READ, 0, &mb)) || FAILED(ctx->Map(after, 0, D3D11_MAP_READ, 0, &ma))) { printf("barcheck: the textures could not be read\n"); return 4; }
        const size_t bpp = hdr ? 8 : 4; uint64_t barsChanged = 0, bars = 0, pictureChanged = 0, picture = 0;
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            const float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
            const bool inside = u >= a.viewport[0] && u <= a.viewport[0] + a.viewport[2] && v >= a.viewport[1] && v <= a.viewport[1] + a.viewport[3];
            const bool differs = memcmp(static_cast<const uint8_t*>(mb.pData) + y * mb.RowPitch + x * bpp, static_cast<const uint8_t*>(ma.pData) + y * ma.RowPitch + x * bpp, bpp) != 0;
            if (inside) { ++picture; pictureChanged += differs; } else { ++bars; barsChanged += differs; }
        }
        ctx->Unmap(before, 0); ctx->Unmap(after, 0);
        printf("barcheck: bars %llu of %llu pixels changed; picture %llu of %llu changed\n", (unsigned long long)barsChanged, (unsigned long long)bars, (unsigned long long)pictureChanged, (unsigned long long)picture);
        const bool ok = barsChanged == 0 && pictureChanged > picture / 10;
        printf("%s\n", ok ? "BAR CHECK PASSED" : "BAR CHECK FAILED");
        return ok ? 0 : 1;
    }
    if (ArgInt(argc, argv, "once", 0) != 0) {   // once=1 (with dump=file): exactly one Run on the fresh frame, for comparing two versions of the shader
        compose.Run(ctx, a);
        D3D11_TEXTURE2D_DESC sd2 = td; sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd2, nullptr, &st); ctx->CopyResource(st, target);
        D3D11_MAPPED_SUBRESOURCE mm{};
        for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "dump=", 5) && SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm))) {
            FILE* f = nullptr; fopen_s(&f, argv[i] + 5, "wb");
            if (f) { for (uint32_t y = 0; y < H; ++y) fwrite(static_cast<const uint8_t*>(mm.pData) + static_cast<size_t>(y) * mm.RowPitch, 1, static_cast<size_t>(W) * (hdr ? 8 : 4), f); fclose(f); }
            ctx->Unmap(st, 0);
        }
        return 0;
    }
    // warm up (shader compile, the copy's textures), then time a batch with timestamp queries
    for (int i = 0; i < 10; ++i) compose.Run(ctx, a);
    ctx->Flush();
    D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; ID3D11Query* disjoint = nullptr; dev->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP; ID3D11Query *t0 = nullptr, *t1 = nullptr; dev->CreateQuery(&qd, &t0); dev->CreateQuery(&qd, &t1);
    double best = 1e9, sum = 0; const int batches = 5;
    for (int b = 0; b < batches; ++b) {
        ctx->Begin(disjoint); ctx->End(t0);
        for (int i = 0; i < runs; ++i) compose.Run(ctx, a);
        ctx->End(t1); ctx->End(disjoint);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 s0 = 0, s1 = 0;
        while (ctx->GetData(disjoint, &dj, sizeof dj, 0) == S_FALSE) Sleep(1);
        while (ctx->GetData(t0, &s0, sizeof s0, 0) == S_FALSE) Sleep(1);
        while (ctx->GetData(t1, &s1, sizeof s1, 0) == S_FALSE) Sleep(1);
        if (dj.Disjoint) continue;
        const double ms = double(s1 - s0) / double(dj.Frequency) * 1000.0 / runs;
        best = std::min(best, ms); sum += ms;
    }
    // a check of the result: the mean of a sample of the target's pixels (the same shader change must keep it)
    D3D11_TEXTURE2D_DESC sdsc = td; sdsc.Usage = D3D11_USAGE_STAGING; sdsc.BindFlags = 0; sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr; dev->CreateTexture2D(&sdsc, nullptr, &stage); ctx->CopyResource(stage, target);
    D3D11_MAPPED_SUBRESOURCE m{}; double check = 0; size_t n = 0;
    if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {
        for (uint32_t y = 0; y < H; y += 7) for (uint32_t x = 0; x < W; x += 5) {
            if (hdr) { const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch) + x * 4; for (int k = 0; k < 3; ++k) check += DirectX::PackedVector::XMConvertHalfToFloat(p[k]); }
            else { const uint8_t* p = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch + x * 4; for (int k = 0; k < 3; ++k) check += p[k] / 255.0; }
            n += 3;
        }
        ctx->Unmap(stage, 0);
    }
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], "dump=", 5) && SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) {   // dump=file: the result's raw pixels, to compare two versions of the shader
        FILE* f = nullptr; fopen_s(&f, argv[i] + 5, "wb");
        if (f) { for (uint32_t y = 0; y < H; ++y) fwrite(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, 1, static_cast<size_t>(W) * (hdr ? 8 : 4), f); fclose(f); }
        ctx->Unmap(stage, 0);
    }
    printf("%ux%u encoding %d sharpen %.2f: %.3f ms a Run (best of %d batches of %d; mean %.3f), check %.6f\n", W, H, encoding, sharpen, best, batches, runs, sum / batches, n ? check / n : 0.0);
    return 0;
}
