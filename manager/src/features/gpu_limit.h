#pragma once
#include <cstdint>
#include <string>

namespace eam {
namespace features {
namespace gpulimit {

// GPU limit: keep the graphics card's load under a percent (80 to 100), for everything of ours that can make its work cheaper. The manager reads the load of the card Lossless Scaling
// runs on (IHost::GpuLoadPercent) and tells the addons the limit (IHost::GpuLimitPercent); an addon lowers its cost while the load is at or over it (Neural Rendering's auto quality
// takes the model's working size down, the upscalers' "lighter upscaling when the card is full" gives the frames between the light pass) and goes back when it is well under.
// This is what each addon's own "Keep the graphics card under a limit" setting used to be (up to 0.10.0).

inline constexpr const char* kId = "GpuLimit";   // the settings section and the id of the on/off switch

// What the feature needs of the rest of the manager, given by main.cpp before the features start (so the tests of the features link without the host).
struct Backends {
    void (*setLimit)(uint32_t percent) = nullptr;   // IHost::GpuLimitPercent says this (0: off)
    uint32_t (*readLoad)() = nullptr;               // the card's load now, 0 when it is not known (also keeps the sampler going)
};
void SetBackends(const Backends& backends);

void Start();                 // the switch is on
void Stop();                  // off: the limit is 0 again
void RenderOptions();         // the slider and the live reading
std::string Status();         // "Limit 90 %: the card is at 62 %", empty when off

} // namespace gpulimit
} // namespace features
} // namespace eam
