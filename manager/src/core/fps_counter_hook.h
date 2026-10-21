// FpsCounterHook: lets an addon that makes frames outside Lossless Scaling (EchoFG) have Lossless Scaling's own frame rate counter show the rate the screen gets.
//
// The counter ("60 / 120", drawn with DirectWrite; config.ini [fps_counter]) is one object in Lossless_original.dll that counts the frames Lossless Scaling captures and
// presents and, once a second, turns the counts into two rates. The function that does it (rva 0x1C950 in the 2025-08-05 build) is hooked: just before it works out the
// rates, the count of presented frames is multiplied by what the addon says it adds. Only that one known build is hooked: the bytes of the function, the build's time stamp
// and size are checked first, and any other Lossless_original.dll is left alone (the call then says false and the addon draws a readout of its own).
#pragma once
#include <windows.h>

namespace eam::fpscounter {

// Hooks the counter in `lossless` (Lossless_original.dll). False (and nothing changed) for a build it does not know.
bool Install(HMODULE lossless);
void Uninstall();

// How many frames the screen gets for each one Lossless Scaling presents (1 = as Lossless Scaling counts). True when the hook is in place.
bool SetMultiplier(float multiplier);

} // namespace eam::fpscounter
