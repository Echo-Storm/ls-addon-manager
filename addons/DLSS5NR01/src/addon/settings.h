// The addon's settings, and its saved looks.
//
// Every setting of the look (the model's knobs, the picture's, the HUD areas) is described once, in a table in settings.cpp: its key in the
// settings file, its default and its range. Loading, saving, and turning a look into text and back all go through that table, so a range is
// written in one place. A look is kept as "key=value;key=value" text under "preset.<name>", with the names joined by '|' in "presetNames"
// (the settings file has no way to list keys); its HUD areas are "l,t,r,b/l,t,r,b".
#pragma once
#include "engine/nr_engine.h"
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

struct IHost;

namespace nr {

// The upscalers' picture settings as kept for one game (Config::scalerGames).
struct ScalerProfile { float sharpen = 0.5f, stability = 0.0f, edges = 0.0f; unsigned preset = 16; int motion = 0; float brightness = 0.0f, contrast = 1.0f, gamma = 1.0f;
                       float shadows = 0.0f, highlights = 0.0f, saturation = 1.0f, vibrance = 0.0f; };
ScalerProfile ProfileOf(const struct Config& c);
void ApplyProfile(struct Config& c, const ScalerProfile& p);
// Keeps c's picture settings as game exe's own (added, or replacing what it had).
void KeepForGame(struct Config& c, const std::string& exe);

struct Config {
    bool enabled = true;
    int model = 0;                       // 0 DLSS 5 Neural Rendering (the person's model file), 1 DLAA (NVIDIA's DLSS runtime, shipped)
    unsigned dlaaPreset = 16;            // the DLSS model (preset): 16 auto (E at 1:1, L upscaling; SrEngine::kPresetAuto), 0 NVIDIA's default (K), 10 J, 12 L, 13 M (DLSS 4.5), 5 E (CNN)
    int motionSource = 0;                // the DLSS Upscaler's motion: 0 measured from the frames, 1 frame generation's flow, 2 none
    // The upscalers: when no newer picture is finished, Lossless Scaling's queue waits on the GPU for the next one rather than show the same
    // picture again (ScalerLink::Upscale). A CPU wait tried before made repeats more frequent on a busy GPU and is gone (2026-09-25).
    bool scalerGpuWait = true;
    bool frameGen = false;   // the FSR Upscaler's frame generation of our own (a prototype, framegen11.h)
    bool frameGenGuard = true;           // ...its guard against pasted background (FgEngine::Generate)
    bool scalerFastMotion = true;       // the upscalers lean on the frame in fast motion (SrEngine::SetFastMotion; off: never, as before 0.9.13)
    float scalerLeanRest = 0.5f;        // the least share of the plain resample in the picture even at rest, 0..1 (SrEngine::SetLeanRest)
    float scalerMoveCut = 0.5f;         // sharpening is cut by this much where the picture moves fast, 0..1 (SrEngine::SetMoveCut)
    float scalerSteadySharp = 0.6f;     // sharpening follows the picture's stability: 0..0.9, how much of the running average it sharpens from (SrEngine::SetSteadySharpen; 0 off)
    float scalerLeanFrom = 0.0f;        // the motion the lean starts from, in % of the frame's width a frame (0: automatic, 0.1 at 1:1 and 0.05 upscaling)
    bool leanEasu = false;              // a test: the moving picture (the lean) is FSR 1's edge-adaptive EASU of the frame, not Catmull-Rom (SrEngine::SetLeanMode)
    bool scalerLightGen = false;        // a test: with frame generation the upscaler runs on the real frames only, the generated ones get a lighter picture (SrEngine::SetCheapNext)
    bool scalerReuseMotion = true;      // with frame generation (two or more frames presented per real one), every other frame keeps the motion estimate of the one before (SrEngine::SetFlowReuse)
    bool motionShapes = false;          // a test: the motion matched by shape and edges, not brightness (SrEngine::SetMeanWeight / SetGradWeight)
    float scalerStability = 0.0f;        // the upscalers: less shimmer, more trailing (SrEngine::SetStability)
    float scalerEdges = 0.0f;            // the upscalers: edge smoothing of the upscaled picture (SrEngine::SetEdgeSmoothing)
    bool scalerPerGame = true;           // the upscalers: their picture settings kept per game (scalerGames), back when the game takes focus
    std::vector<std::pair<std::string, struct ScalerProfile>> scalerGames;   // lower-case exe name, its settings
    int scalerHandoff = 0;               // the DLSS Upscaler's handoff (ScalerLink::Handoff): 0 one frame late, 1 GPU wait, 2 DLSS runs but NIS stays,
                                         // 3 NIS runs and DLSS's picture is copied over it at Present
    NrParams p;                          // the look, and the few model settings that are not part of a look
    bool composeFillsScreen = false;     // Neural Rendering: the picture is stretched over the whole screen (Lossless Scaling's stretch mode), not drawn at its aspect ratio with bars: the model's change is then everywhere
    bool presentMode = true;             // with frame generation off, the model takes the presented frame (see Present in runtime.cpp)
    bool presentWait = false;            // ...and each frame waits on the GPU for its own result (off: the newest ready one, moved along the motion)
    int frameEncoding = 0;               // what 10-bit and half-float frames hold (hdr.h EncodingOf): 0 automatic, 1 SDR, 2 HDR
    bool freshFlow = true;               // run the model once LSFG has this frame's own motion (off: at capture, with the frame before's)
    bool lsFirst = true;                 // give Lossless Scaling's GPU work priority over the model's
    bool hotkeys = true;                 // Ctrl+Shift + an F key, read at every present
    int keyAB = VK_F7, keySplit = VK_F8, keySharpDn = VK_F2, keySharpUp = VK_F3, keyPreset = VK_F10, keyShot = VK_F4, keyRecord = VK_F1;
    bool recordOn = false;               // the recorder (recorder.h): keep the last few seconds of frames ready to save
    bool recordShown = false;            // ...the presented frames instead (with frame generation of our own: the frames between too, tagged)
    float recordSeconds = 5.0f;
    int recordBudgetMb = 3072;           // at most this much memory for them (the oldest go first)
    std::string recordFolder;            // empty: Videos\Lossless Scaling
    int recordSaveAfter = 0;             // for the tests: save once this many frames are held (0: never by itself)
    std::string screenshotFolder;        // empty: Pictures\Lossless Scaling
    bool autoQuality = false;            // lower the model resolution when the model runs over its time budget (auto_quality.h)
    float autoBudgetMs = 5.0f, autoFloor = 0.25f;
    int modelEvery = 1;                  // the model runs on every Nth real frame (1..4), the frames between are shown with its last result moved along the motion; auto quality may ask for more
    float autoScaleLast = 0.0f;   // the model resolution auto quality settled at last time (0: none yet): where it starts
    bool gameAuto = true;                // switch to a program's look when it takes focus
    std::vector<std::pair<std::string, std::string>> games;   // lower-case exe name, look name
    int tapMode = 0;                     // 0 automatic, 1 by hand
    int frameSlot = -1;                  // -1 automatic (the highest large texture slot)
    std::string tickSig, tapSig;         // the passes chosen by hand
    float watchdogMs = 80.0f;
    std::string snippetPath;             // empty: nvngx_dlssnr.dll in the Lossless Scaling folder
};

struct Look { std::string name, data; };

// A look as text, and back. ApplyLook changes only the settings the text names (so a look saved by an older version applies what it has)
// and keeps each within its range; false when the text names none.
std::string LookToText(const NrParams& p);
bool ApplyLook(const std::string& text, NrParams& p);
std::string HudToText(const NrParams& p);
void HudFromText(const std::string& text, NrParams& p);
// A name that fits the stores: no '|', ';' or '=', no trailing spaces, at most 40 characters.
std::string CleanName(std::string name);

// The settings file, through the host.
struct Loaded { Config config; std::vector<Look> looks; int compareStart = 0; float splitStart = 0.5f; };
Loaded LoadSettings(IHost* host, const char* addonId);
NrParams ProductDefaults();
// The upscalers' Sharpening slider (0..1) spans this much strength: 1 is the standard CAS / RCAS maximum, above it the sharpening's effect is
// amplified (the engine's sharpening pass). Before 0.9.2 the slider was the strength itself; LoadSettings converts a value saved then.
inline constexpr float kScalerSharpenScale = 1.6f;   // the defaults of this addon's picture settings (NrParams(), except the upscalers' sharpening)
void SaveSettings(IHost* host, const char* addonId, const Config& config, const std::vector<Look>& looks);
// The saved text for a look or a program is cleared when it is deleted (the list no longer names it, the text would linger).
void ForgetLook(IHost* host, const char* addonId, const std::string& name);
void ForgetGame(IHost* host, const char* addonId, const std::string& exe);

} // namespace nr
