// The frame trace: a ring of timestamped events (a real frame arriving, every present, the model's runs, the upscaler's passes, auto quality's changes, hotkeys),
// written from any thread without a lock and exported as a CSV, so a bumpy stretch can be looked at event by event instead of as the averages the log keeps
// (tools/analyze_frame_trace.py reads it). It costs a timestamp and four integers a frame. The idea, a per-frame timeline with an offline analysis, is from the
// Magpie fork's frame trace (docs/magpie-dissection.md); the events and the code are our own.
//
//   t_us    microseconds since the first event (QPC)
//   kind    tap | present | model | upscale | auto | hotkey | mark
//   a, b, c what the kind says (below)
//
//   tap      a real frame reached the addon: c = its index
//   present  a frame was presented: a = a number for the swap chain (the busiest is Lossless Scaling's output), b = the present flags, c = the sync interval
//   model    a model run ended: a = 1 started / 0 left out (the model was busy), b = its GPU start delay in 0.01 ms, c = its run in 0.01 ms
//   upscale  an upscaler pass: a = 1 the upscaler's picture was used / 0 NIS stayed (+ 2: a generated frame given the lighter run), b = the engine's GPU ms and c = its motion estimate's ms, both in 0.01 ms
//   auto     auto quality changed something: a = the model resolution in percent, b = the model runs on every Nth frame
//   hotkey   a = which hotkey (Ctrl+Shift+F-key index, see runtime.cpp)
#pragma once
#include <cstdint>
#include <string>

namespace nr::trace {

enum Kind : uint8_t { kTap = 0, kPresent = 1, kModel = 2, kUpscale = 3, kAuto = 4, kHotkey = 5, kMark = 6 };

// Adds an event (lock-free; safe from any thread).
void Add(Kind kind, int32_t a = 0, int32_t b = 0, int32_t c = 0);

// Writes the recent events as a CSV. False (with `error`) when the file cannot be written.
bool Export(const std::wstring& path, std::string* error = nullptr);

// How many events are kept (the ring's size); the newest win.
constexpr uint32_t kCapacity = 1u << 17;

} // namespace nr::trace
