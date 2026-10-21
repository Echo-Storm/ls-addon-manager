#include "addon/fps_label.h"
#include "addon/label_font.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace nr {

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

float SrgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

uint16_t ToHalf(float f) {   // round to nearest; values of a label (0 to a few) only
    uint32_t x; memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    const uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) + ((man + 0x1000u) >> 13));   // (a carry out of the mantissa rolls into the exponent, as it should)
}

float Pq(float nits) {   // SMPTE ST 2084, nits to 0..1
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = std::pow(std::clamp(nits / 10000.0f, 0.0f, 1.0f), m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

DXGI_FORMAT Family(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace

std::string FpsLabel::ConvertPixels(const uint32_t* px, uint32_t count, DXGI_FORMAT format, uint32_t encoding, float whiteNits) {
    const DXGI_FORMAT fam = Family(format);
    std::string out;
    const float white = whiteNits > 0.0f ? whiteNits : 80.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(&px[i]);   // R, G, B, A in memory
        if (fam == DXGI_FORMAT_R8G8B8A8_UNORM) { out.append(reinterpret_cast<const char*>(c), 4); }
        else if (fam == DXGI_FORMAT_B8G8R8A8_UNORM) { const char b[4] = { static_cast<char>(c[2]), static_cast<char>(c[1]), static_cast<char>(c[0]), static_cast<char>(255) }; out.append(b, 4); }
        else if (fam == DXGI_FORMAT_R10G10B10A2_UNORM) {
            uint32_t v = 3u << 30;
            for (int k = 0; k < 3; ++k) {
                const float s = c[k] / 255.0f;
                const float e = encoding == 2 ? Pq(SrgbToLinear(s) * white) : s;   // HDR10: the SDR picture's light at the SDR white, PQ coded; SDR: as it is
                v |= static_cast<uint32_t>(std::clamp(e, 0.0f, 1.0f) * 1023.0f + 0.5f) << (10 * k);
            }
            out.append(reinterpret_cast<const char*>(&v), 4);
        } else if (fam == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            uint16_t h[4];
            for (int k = 0; k < 3; ++k) h[k] = ToHalf(SrgbToLinear(c[k] / 255.0f) * white / 80.0f);   // scRGB: 1.0 is 80 nits
            h[3] = ToHalf(1.0f);
            out.append(reinterpret_cast<const char*>(h), 8);
        } else return {};
    }
    return out;
}

void FpsLabel::Set(const std::string& row1, const std::string& row2) {
    if (row1 == m_row1 && row2 == m_row2) return;
    m_row1 = row1; m_row2 = row2; m_dirty = true;
}

bool FpsLabel::Stamp(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, int corner, uint32_t encoding, float whiteNits) {
    if (!ctx || !frame || corner < 1 || m_row1.empty()) return false;
    D3D11_TEXTURE2D_DESC d{}; frame->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.Width < 400 || d.Height < 200 || Family(d.Format) == DXGI_FORMAT_UNKNOWN) return false;
    ID3D11Device* dev = nullptr; frame->GetDevice(&dev); if (dev) dev->Release();   // (only compared; the context keeps it alive)
    if (!dev) return false;
    const int scale = std::clamp(static_cast<int>(d.Width / 800), 2, 8);
    if (dev != m_dev || d.Format != m_format || scale != m_scale || encoding != m_enc || whiteNits != m_white) { SafeRelease(m_tex); m_dirty = true; }
    if (m_dirty || !m_tex) {
        SafeRelease(m_tex);
        std::vector<std::string> rows = { m_row1 }; if (!m_row2.empty()) rows.push_back(m_row2);
        uint32_t w = 0, h = 0;
        const std::vector<uint32_t> px = labelfont::LabelImage(rows, 0x40B0A0, scale, w, h);
        const std::string bytes = ConvertPixels(px.data(), static_cast<uint32_t>(px.size()), d.Format, encoding, whiteNits);
        if (bytes.empty() || w == 0 || h == 0) return false;
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = d.Format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        const D3D11_SUBRESOURCE_DATA init{ bytes.data(), static_cast<UINT>(bytes.size() / h), 0 };
        if (FAILED(dev->CreateTexture2D(&td, &init, &m_tex))) { m_tex = nullptr; return false; }
        m_dev = dev; m_format = d.Format; m_scale = scale; m_enc = encoding; m_white = whiteNits; m_w = w; m_h = h; m_dirty = false;
    }
    const uint32_t margin = static_cast<uint32_t>(scale) * 3u;
    const uint32_t x = corner == 2 ? (d.Width > m_w + margin ? d.Width - m_w - margin : 0) : margin, y = margin;
    if (x + m_w > d.Width || y + m_h > d.Height) return false;
    ctx->CopySubresourceRegion(frame, 0, x, y, 0, m_tex, 0, nullptr);
    return true;
}

void FpsLabel::Release() { SafeRelease(m_tex); m_dev = nullptr; m_dirty = true; }

} // namespace nr
