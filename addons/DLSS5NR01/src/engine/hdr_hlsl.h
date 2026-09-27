// HDR frames, as the passes see them: HLSL shared by the engine's passes (D3D12) and the compose (D3D11), put in front of their own source.
//
// The model, the look controls and our other passes work on display-referred pictures, 0..1 and sRGB-encoded. An HDR frame is either scRGB
// (16-bit float, linear, Rec.709 primaries, 1.0 = 80 nits, brighter above) or HDR10 (10-bit, the PQ curve, Rec.2020 primaries). ToSdr gives
// its SDR view: light relative to the SDR white (Windows' "SDR content brightness", `white` in nits), a curve that is the identity up to kKnee
// and rolls everything brighter smoothly (logarithmically) into the rest of 0..1, then sRGB encoding. FromSdr is its exact inverse, so a change made in the SDR
// view goes back into the frame's own encoding, and what was not changed comes back as it was. Encoding 0 is SDR: both are the identity.
#pragma once

#define NR_HDR_HLSL R"HLSL(
static const float kKnee = 0.75;
// Above the knee a logarithmic roll-off: 1.0 is kKnee + kLogTop times the SDR white (10000 nits at 80), and kLogA makes its slope 1 at the knee.
// An exponential one (until 0.9.8) put a 1000-nit highlight within 1e-8 of 1.0, so the upscalers' slightest change there (0.999) brought it back
// at 170 nits; with the logarithm a change of 0.001 moves a highlight by about 3 %.
static const float kLogA = 0.03, kLogTop = 125.0;
static const float kLogSpan = log(1.0 + kLogTop / kLogA);
static const float3x3 kRec2020To709 = { 1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187 };
static const float3x3 kRec709To2020 = { 0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956 };
float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 LinearToSrgb(float3 l) { l = saturate(l); return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }
float3 PqToNits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float3 NitsToPq(float3 n) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const float3 y = pow(saturate(n / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float3 Compress(float3 x) { const float3 over = max(x - kKnee, 0.0); return min(x, kKnee) + (1.0 - kKnee) * log(1.0 + over / kLogA) / kLogSpan; }
float3 Expand(float3 y) { const float3 over = clamp(y - kKnee, 0.0, 1.0 - kKnee); return min(y, kKnee) + kLogA * (exp(over / (1.0 - kKnee) * kLogSpan) - 1.0); }
// encoding: 0 SDR, 1 scRGB, 2 HDR10 (PQ); white: the SDR white in nits
float3 ToLight(float3 c, uint encoding, float white) {   // an HDR frame as light, Rec.709, 1 = the SDR white
    return encoding == 1u ? c * (80.0 / white) : mul(kRec2020To709, PqToNits(c)) / white;
}
float3 ToSdr(float3 c, uint encoding, float white) {
    if (encoding == 0u) return c;
    return LinearToSrgb(Compress(max(ToLight(c, encoding, white), 0.0)));
}
// Light (linear, Rec.709, 1 = the SDR white) and the SDR view, both ways: the upscalers take HDR frames as light, in DLSS's and FSR's own HDR mode
float3 SdrToLight(float3 s) { return Expand(SrgbToLinear(saturate(s))); }
float3 LightToSdr(float3 l) { return LinearToSrgb(Compress(max(l, 0.0))); }
float3 FromLight(float3 lin, uint encoding, float white) {
    return encoding == 1u ? lin * (white / 80.0) : NitsToPq(mul(kRec709To2020, max(lin, 0.0)) * white);
}
float3 FromSdr(float3 s, uint encoding, float white) {
    if (encoding == 0u) return s;
    return FromLight(SdrToLight(s), encoding, white);
}
)HLSL"
