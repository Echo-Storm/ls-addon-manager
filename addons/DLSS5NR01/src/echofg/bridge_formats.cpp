// The two static format helpers of Bridge (src/addon/bridge.cpp), which the recorder uses to read a frame's format. ECHOFG links this instead of the whole of bridge.cpp: the Bridge is
// Neural Rendering's way to its model engine, which this addon has none of.
#include "addon/bridge.h"

DXGI_FORMAT Bridge::ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
bool Bridge::FormatSupported(DXGI_FORMAT f) {
    const DXGI_FORMAT view = ViewFormat(f);
    return view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM || view == DXGI_FORMAT_R10G10B10A2_UNORM || view == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
