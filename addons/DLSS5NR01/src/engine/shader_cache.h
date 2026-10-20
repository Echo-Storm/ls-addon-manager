// A disk cache for compiled compute shaders: D3DCompile of the motion estimate, the synthesis and the upscalers' passes takes 1 to 5 seconds when an addon starts (EchoFG:
// 1.2 s of it on the presenting thread, the rest before it can make a frame), and the answer is the same every time for the same source. The bytecode is kept under
// %LOCALAPPDATA%\LSAddonManager\shader-cache, named by a hash of everything that decides it (the text, the entry, the target, the flags, the defines and a version of this
// cache), with a checksum so that a half-written or damaged file is compiled again instead of used. NR_SHADER_CACHE=0 turns it off. Files not used for 90 days are removed.
#pragma once
#include <windows.h>
#include <d3dcompiler.h>
#include <cstddef>

namespace nr {

// Like D3DCompile (the same arguments, the include handler is not used), answering from the cache when the same compile was made before.
HRESULT CompileCached(const char* source, size_t length, const char* name, const D3D_SHADER_MACRO* defines, const char* entry, const char* target, UINT flags,
                      ID3DBlob** code, ID3DBlob** errors);

} // namespace nr
