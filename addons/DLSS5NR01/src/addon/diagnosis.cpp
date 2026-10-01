#include "addon/diagnosis.h"
#include <algorithm>
#include <cstdio>

namespace nr::diag {

namespace {
std::string Fmt(const char* f, double a = 0, double b = 0, double c = 0) { char t[320]; snprintf(t, sizeof t, f, a, b, c); return t; }
}

std::vector<Finding> Diagnose(const Snapshot& s) {
    std::vector<Finding> out;
    auto add = [&](Level l, std::string what, std::string tryThis) { out.push_back({ l, std::move(what), std::move(tryThis) }); };

    if (s.showingPlain)
        add(Level::Note, "You are looking at the plain picture (the before / after toggle), not the enhanced one.", "Press the before / after hotkey again to see the result.");

    if (s.recording)   // it is the first thing to rule out when the card is full: it copies every frame it keeps (a recording of what is shown copies every presented frame, 4K and HDR ones are 33 to 66 MB each)
        add(s.frames >= 60 && s.frameP95 > 1.4f * s.frameP50 ? Level::Problem : Level::Note,
            s.recordingShown ? "Recording is on, and it records what is shown: every presented frame is copied (a 4K frame is 33 to 66 MB) and kept in memory."
                             : "Recording is on: it copies the frames it keeps all the time, which takes some of the graphics card and the processor.",
            "Turn it off (Recording in the panel) unless you are recording a problem: it is the first thing to rule out when frames are uneven or the card is full.");

    if (s.frames >= 60 && s.frameP50 > 0) {
        const bool uneven = s.frameP95 > 1.6f * s.frameP50 && s.frameP95 - s.frameP50 > 6.0f;
        const bool waits = s.model && s.startMs > 4.0f;
        if (uneven)
            add(Level::Problem, Fmt("The game's frame times are uneven: typically %.1f ms, but 1 frame in 20 takes over %.1f ms.", s.frameP50, s.frameP95) +
                (s.frameP99 > 3.0f * s.frameP50 ? Fmt(" The worst 1 in 100 take over %.0f ms.", s.frameP99) : std::string()),
                waits ? "The graphics card is full (the model is waiting for it). Lower the game's settings or resolution, or the model resolution."
                      : "If this also happens with this addon switched off, it comes from the game or the card; otherwise compare with the addon's passes off (before / after hotkey).");
        if (waits)
            add(Level::Problem, Fmt("The model waits %.1f ms for the graphics card before it can start.", s.startMs),
                s.autoOn ? "The graphics card has no room left. Auto quality is already lowering the model; lower the game's own settings for more."
                         : "The graphics card has no room left. Lower the Model resolution, or turn on Auto quality.");
    }

    if (s.model) {
        const bool atFloor = s.autoOn && s.scale > 0 && s.scale <= s.floor + 0.005f;
        if (s.runEvery > 1)
            add(Level::Note, Fmt("Auto quality is running the model on every %.0f frames to spare the card (it was already at its lowest resolution).", s.runEvery),
                "The model's result refreshes less often, so a little flicker can show. Lower the game's settings to give it room back.");
        else if (atFloor && s.modelMs > 2.0f)
            add(Level::Note, Fmt("The model is at the lowest resolution Auto quality may use (%.0f%%) and still takes %.1f ms.", 100.0 * s.scale, s.modelMs),
                "Lower the floor in the settings only if you accept a softer model, or lower the game's settings.");
        if (s.keepUpPct < 90.0f && s.runEvery <= 1)
            add(Level::Note, Fmt("The model is behind: it is given %.0f%% of the frames (the rest reuse its last result).", s.keepUpPct),
                s.autoOn ? "Auto quality will lower the resolution if this lasts." : "Lower the Model resolution, or turn on Auto quality.");
        if (!s.autoOn && s.modelMs > 6.0f)
            add(Level::Note, Fmt("The model takes %.1f ms a frame.", s.modelMs), "Auto quality keeps it within a budget and is worth turning on.");
    }

    if (s.upscaler) {
        if (s.upscalerMs > 3.0f)
            add(Level::Note, Fmt("The upscaler costs %.1f ms of GPU for every presented frame.", s.upscalerMs),
                "That is heavy at this resolution; a lower quality mode or the lean passes (Crisp edges off, Steady sharpening off) take some back.");
        else if (s.motionMs > 1.0f)
            add(Level::Info, Fmt("The motion estimate takes %.2f ms.", s.motionMs), "\"Share motion between generated frames\" halves it when frame generation is on.");
    }

    std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.level > b.level; });
    if (out.size() > 4) out.resize(4);
    return out;
}

} // namespace nr::diag
