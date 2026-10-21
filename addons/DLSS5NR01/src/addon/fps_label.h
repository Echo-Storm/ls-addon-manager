// EchoFG's frame rate readout. Lossless Scaling draws its own frame rate counter, and counts its own frames: with its frame generation OFF and EchoFG making the frames between,
// it can only show the real (captured) rate, 60 where the screen gets 120. This draws what the screen gets (the frames shown a second, the real rate and the multiplier) in a corner
// of every frame EchoFG presents, as a small label in the comparison's letter font (label_font.h), copied into the swap chain's back buffer just before each present.
#pragma once
#include <d3d11.h>
#include <cstdint>
#include <string>

namespace nr {

class FpsLabel {
public:
    // The text, in two rows. The picture is remade at the next Stamp.
    void Set(const std::string& row1, const std::string& row2);
    // Copies the label into `frame` (a swap chain's back buffer, on ctx's device) at a corner: 1 top left, 2 top right. encoding: what the frame holds (0 SDR, 1 scRGB, 2 HDR10; hdr.h),
    // whiteNits the SDR white (HDR frames). False when the frame's format or size cannot take it (nothing is drawn).
    bool Stamp(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, int corner, uint32_t encoding, float whiteNits);
    void Release();

    // The pixels of a label (RGBA8, as label_font.h makes them) in the format a back buffer has, for a frame of this encoding: empty for a format it does not take. (Public for the tests.)
    static std::string ConvertPixels(const uint32_t* rgba8, uint32_t count, DXGI_FORMAT format, uint32_t encoding, float whiteNits);

private:
    std::string m_row1, m_row2;
    bool m_dirty = true;
    ID3D11Device* m_dev = nullptr;   // only compared
    ID3D11Texture2D* m_tex = nullptr;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    uint32_t m_w = 0, m_h = 0, m_enc = 0xFFFFFFFFu; int m_scale = 0, m_corner = 0; float m_white = 0;
};

} // namespace nr
