// "What is wrong": the numbers the addon already keeps (the game's frame-time spread, the model's time and its wait for the card, what auto quality is doing, the
// upscaler's cost) turned into a few plain sentences with what to try, for a card in the panel. The idea, an in-app "recent issue" card, is from the Magpie fork
// (docs/magpie-dissection.md); the rules are ours, from what the logs of real sessions showed. Pure: the panel and the test feed it a Snapshot.
#pragma once
#include <string>
#include <vector>

namespace nr::diag {

struct Snapshot {
    // the game's real frames over the last window (0 frames: not measured yet)
    int frames = 0;
    float frameP50 = 0, frameP95 = 0, frameP99 = 0;
    // Neural Rendering's model (model = false: not this addon)
    bool model = false;
    float modelMs = 0, startMs = 0, keepUpPct = 100;
    bool autoOn = false; float scale = 0, floor = 0; int runEvery = 1;
    // the upscalers (upscaler = false: not these addons)
    bool upscaler = false;
    float upscalerMs = 0, motionMs = 0;   // the engine's GPU time for a presented frame, and its motion estimate's share
    // the picture on screen is the plain one (the before / after toggle)
    bool showingPlain = false;
    // the recorder is on (it copies frames all the time) and whether it records what is shown (every presented frame, twice the frames and big ones)
    bool recording = false, recordingShown = false;
};

enum class Level { Info = 0, Note = 1, Problem = 2 };

struct Finding { Level level; std::string what, tryThis; };

// The findings, the worst first, at most four. Empty when nothing stands out (or there is too little to judge).
std::vector<Finding> Diagnose(const Snapshot& s);

} // namespace nr::diag
