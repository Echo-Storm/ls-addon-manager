// Screenshots of the picture as it is shown: taken from Lossless Scaling's swap chain at a present, after Neural Rendering's result, the scaling
// and frame generation are all in it (ReShade's own screenshots show the game's frame before any of that).
//
// A request (the panel's button or the Ctrl+Shift key) is served at the next present of Lossless Scaling's swap chain: the buffer is copied on
// the GPU, read back a few presents later without waiting for the GPU, and written as a PNG on a thread of its own, so the game never stalls.
#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>
#include <vector>

namespace nr::screenshot {

void Request();
// A snapshot for the panel instead of a file: the same picture, made at most 960 pixels wide (RGBA), kept in memory.
void RequestSnapshot();
// The newest snapshot, when it is newer than `serial` (which it then updates).
bool NewSnapshot(uint64_t& serial, std::vector<unsigned char>& rgba, unsigned& w, unsigned& h);
// At every present of Lossless Scaling's swap chain, after the compose (on its render thread, under g_frameMutex).
void OnPresent(ID3D11DeviceContext* ctx, IDXGISwapChain* chain, const std::string& game);
void Forget();   // the device is going away: drop a copy in progress

std::wstring Folder();         // the chosen folder, or Pictures\Lossless Scaling
void ChooseFolder();           // a folder dialog, on a thread of its own
bool Choosing();
bool Busy();                   // a picture is being taken or written
std::string LastResult(bool& ok);

// For the test: the conversion of one row of a presented buffer to 8-bit BGRA with full alpha. False for a format it cannot convert.
bool ToBgra8(DXGI_FORMAT format, const void* row, unsigned width, unsigned char* out);
// The same for an HDR frame (encoding 1 scRGB, 2 HDR10; white: the SDR white in nits): its SDR view, the curve engine/hdr_hlsl.h uses, so
// highlights roll off instead of being cut. Encoding 0 is ToBgra8.
bool ToBgra8Sdr(DXGI_FORMAT format, const void* row, unsigned width, uint32_t encoding, float white, unsigned char* out);

// ---- pictures taken now, of part of a texture (the upscalers' before / after pair, at the NIS pass)
// The region is copied on the GPU at once, read back at a later Tick without waiting for the GPU, and written as a PNG on a thread of its
// own at `path`. viewFormat: how to read the texture (a typeless one has none of its own). HDR pictures are saved in their SDR view.
bool Capture(ID3D11DeviceContext* ctx, ID3D11Resource* source, const D3D11_BOX& region, DXGI_FORMAT viewFormat, const std::wstring& path,
             uint32_t encoding, float white);
void Tick(ID3D11DeviceContext* ctx);   // on the render thread, at each pass: finishes the captures whose copies are done
void ForgetCaptures();                  // the device is going away
// "<folder>\<game>_<date>" without an extension (the pair adds "_NIS.png" and "_DLSS.png" or "_FSR.png").
std::wstring PairBase(const std::string& game);

} // namespace nr::screenshot
