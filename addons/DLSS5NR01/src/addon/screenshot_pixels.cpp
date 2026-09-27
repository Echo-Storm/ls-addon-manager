// The conversion of a presented buffer's rows to 8-bit BGRA with full alpha, for the PNG (kept apart so the offline test can use it alone).
#include "addon/screenshot.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace nr::screenshot {

namespace {
float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else { int e = -1; uint32_t m = mantissa; do { ++e; m <<= 1; } while (!(m & 0x400)); bits = sign | ((112 - e) << 23) | ((m & 0x3FF) << 13); }
    } else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
    else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f; memcpy(&f, &bits, sizeof f);
    return f;
}
unsigned char Byte(float v) { return static_cast<unsigned char>(v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f + 0.5f); }

// engine/hdr_hlsl.h's ToSdr, on the CPU: light relative to the SDR white, the identity up to 0.75 and brighter rolled off into the rest
// of 0..1, then sRGB encoding
float Compress(float x) { const float k = 0.75f; return x <= k ? x : k + (1.0f - k) * (1.0f - std::exp(-(x - k) / (1.0f - k))); }
float SrgbEncode(float l) { l = l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l; return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f; }
float PqToNits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float p = std::pow(e < 0.0f ? 0.0f : e > 1.0f ? 1.0f : e, 1.0f / m2);
    const float num = p - c1 > 0.0f ? p - c1 : 0.0f;
    return 10000.0f * std::pow(num / (c2 - c3 * p), 1.0f / m1);
}
unsigned char SdrByte(float linearToWhite) { return Byte(SrgbEncode(Compress(linearToWhite > 0.0f ? linearToWhite : 0.0f))); }

} // namespace

bool ToBgra8(DXGI_FORMAT format, const void* row, unsigned width, unsigned char* out) {
    const auto* in = static_cast<const unsigned char*>(row);
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        for (unsigned x = 0; x < width; ++x) { memcpy(out + x * 4, in + x * 4, 3); out[x * 4 + 3] = 255; }
        return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        for (unsigned x = 0; x < width; ++x) { out[x * 4] = in[x * 4 + 2]; out[x * 4 + 1] = in[x * 4 + 1]; out[x * 4 + 2] = in[x * 4]; out[x * 4 + 3] = 255; }
        return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        for (unsigned x = 0; x < width; ++x) {
            uint32_t v; memcpy(&v, in + x * 4, 4);
            out[x * 4] = static_cast<unsigned char>(((v >> 20) & 1023) >> 2); out[x * 4 + 1] = static_cast<unsigned char>(((v >> 10) & 1023) >> 2);
            out[x * 4 + 2] = static_cast<unsigned char>((v & 1023) >> 2); out[x * 4 + 3] = 255;
        }
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:   // kept to 0..1: HDR highlights are cut, not tone-mapped
        for (unsigned x = 0; x < width; ++x) {
            uint16_t c[4]; memcpy(c, in + x * 8, 8);
            out[x * 4] = Byte(HalfToFloat(c[2])); out[x * 4 + 1] = Byte(HalfToFloat(c[1])); out[x * 4 + 2] = Byte(HalfToFloat(c[0])); out[x * 4 + 3] = 255;
        }
        return true;
    default:
        return false;
    }
}

bool ToBgra8Sdr(DXGI_FORMAT format, const void* row, unsigned width, uint32_t encoding, float white, unsigned char* out) {
    if (encoding == 0) return ToBgra8(format, row, width, out);
    const auto* in = static_cast<const unsigned char*>(row);
    const float w = white > 1.0f ? white : 200.0f;
    if (encoding == 1 && (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS)) {   // scRGB: 1.0 = 80 nits
        const float scale = 80.0f / w;
        for (unsigned x = 0; x < width; ++x) {
            uint16_t c[4]; memcpy(c, in + x * 8, 8);
            out[x * 4] = SdrByte(HalfToFloat(c[2]) * scale); out[x * 4 + 1] = SdrByte(HalfToFloat(c[1]) * scale);
            out[x * 4 + 2] = SdrByte(HalfToFloat(c[0]) * scale); out[x * 4 + 3] = 255;
        }
        return true;
    }
    if (encoding == 2 && (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS)) {   // HDR10: PQ, Rec.2020
        for (unsigned x = 0; x < width; ++x) {
            uint32_t v; memcpy(&v, in + x * 4, 4);
            const float r = PqToNits((v & 1023) / 1023.0f), g = PqToNits(((v >> 10) & 1023) / 1023.0f), b = PqToNits(((v >> 20) & 1023) / 1023.0f);
            const float r7 = 1.6605f * r - 0.5876f * g - 0.0728f * b, g7 = -0.1246f * r + 1.1329f * g - 0.0083f * b, b7 = -0.0182f * r - 0.1006f * g + 1.1187f * b;
            out[x * 4] = SdrByte(b7 / w); out[x * 4 + 1] = SdrByte(g7 / w); out[x * 4 + 2] = SdrByte(r7 / w); out[x * 4 + 3] = 255;
        }
        return true;
    }
    return ToBgra8(format, row, width, out);   // a format with no HDR meaning: as it is
}

} // namespace nr::screenshot
