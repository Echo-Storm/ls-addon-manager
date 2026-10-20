#pragma once
#include <cstdint>
#include <string>

struct ID3D11DeviceContext;

namespace eam {
namespace features {
namespace recorder {

// Recording: the last few seconds of Lossless Scaling's frames, saved as a .lsrec file (for bug reports, tests and comparisons) with a hotkey. One
// recorder for the whole manager: it finds the captured frames by itself, with no addon on, and takes the frames only an addon has through
// IHost::RecorderOffer. This is what each addon's own Recording section used to be (up to 0.10.0).

inline constexpr const char* kId = "Recorder";   // the settings section and the id of the on/off switch

// What the feature needs of the rest of the manager, given by main.cpp before the features start (so the tests of the features link without the hook, the host and the
// event bus: without them the recorder simply has no frames to see and nobody to hand its offers to).
struct Backends {
    void (*setDispatchObserver)(void (*observer)(ID3D11DeviceContext*, uint32_t, uint32_t, uint32_t)) = nullptr;
    void (*setHostHooks)(uint32_t (*flags)(), void (*offer)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t), int (*save)(const char*, bool)) = nullptr;
    void (*publishSaved)(const wchar_t* path) = nullptr;
};
void SetBackends(const Backends& backends);

void Start();                 // the switch is on: watch the passes, listen for the hotkey, take offers (safe while Lossless Scaling runs)
void Stop();                  // off: everything is let go, the memory too
void RenderOptions();         // the settings, drawn in the manager's window
std::string Status();         // "Recorder on: 4.2 s kept (3840x2160)", empty when off

} // namespace recorder
} // namespace features
} // namespace eam
