// Offline test of the "what is wrong" rules (src/addon/diagnosis.cpp).
#include "addon/diagnosis.h"
#include "addon/source_cadence.h"
#include <cstdio>
#include <string>
using namespace nr::diag;
static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") { printf("%s  %s%s\n", ok ? "PASS" : "FAIL", what, ok || detail.empty() ? "" : ("  (" + detail + ")").c_str()); if (!ok) ++g_failed; }
static bool Has(const std::vector<Finding>& v, const char* text) { for (const auto& f : v) if (f.what.find(text) != std::string::npos) return true; return false; }

int main() {
    Snapshot calm; calm.frames = 300; calm.frameP50 = 16.7f; calm.frameP95 = 17.5f; calm.frameP99 = 18.0f;
    calm.model = true; calm.modelMs = 2.0f; calm.startMs = 0.5f; calm.keepUpPct = 100; calm.autoOn = true; calm.scale = 0.5f; calm.floor = 0.25f;
    Check("a calm session has nothing to report", Diagnose(calm).empty());

    Snapshot none; Check("nothing measured yet: nothing to report", Diagnose(none).empty());

    Snapshot uneven = calm; uneven.frameP95 = 31.0f; uneven.frameP99 = 60.0f;
    auto r = Diagnose(uneven);
    Check("uneven game frames are reported as a problem", r.size() == 1 && r[0].level == Level::Problem && Has(r, "uneven"));
    Check("...with the worst 1 in 100 when it is far out", Has(r, "worst 1 in 100"));
    Check("...and advice that the game or the card may be the cause", !r.empty() && r[0].tryThis.find("switched off") != std::string::npos);

    Snapshot waiting = uneven; waiting.startMs = 9.0f;
    r = Diagnose(waiting);
    Check("uneven frames with the model waiting for the card: the card is full", r.size() >= 1 && Has(r, "waits 9.0 ms") && r[0].tryThis.find("full") != std::string::npos);
    Check("...worst first", r.size() >= 2 && r[0].level >= r[1].level);

    Snapshot every = calm; every.runEvery = 2; every.scale = 0.25f;
    r = Diagnose(every);
    Check("the model on every 2nd frame is explained", r.size() == 1 && Has(r, "every 2 frames"));

    Snapshot floorHit = calm; floorHit.scale = 0.25f; floorHit.modelMs = 4.0f;
    Check("the floor is reported when the model is still slow there", Has(Diagnose(floorHit), "lowest resolution"));

    Snapshot behind = calm; behind.keepUpPct = 70.0f;
    Check("a model that is given too few frames is reported", Has(Diagnose(behind), "behind"));

    Snapshot noAuto = calm; noAuto.autoOn = false; noAuto.modelMs = 9.0f;
    Check("a slow model with auto quality off suggests turning it on", Has(Diagnose(noAuto), "9.0 ms a frame"));

    Snapshot plain = calm; plain.showingPlain = true;
    r = Diagnose(plain);
    Check("the plain picture (before / after toggle) is pointed out", r.size() == 1 && Has(r, "plain picture"));

    Snapshot up; up.upscaler = true; up.frames = 600; up.frameP50 = 16.7f; up.frameP95 = 17.0f; up.upscalerMs = 3.5f;
    Check("a heavy upscaler is reported", Has(Diagnose(up), "3.5 ms of GPU"));
    up.upscalerMs = 1.2f; up.motionMs = 1.4f;
    r = Diagnose(up);
    Check("a costly motion estimate is an info line", r.size() == 1 && r[0].level == Level::Info);

    Snapshot rec = calm; rec.recording = true; rec.recordingShown = true;
    r = Diagnose(rec);
    Check("the recorder on is pointed out (a note while the frames are steady)", r.size() == 1 && r[0].level == Level::Note && Has(r, "Recording is on") && Has(r, "what is shown"));
    rec.frameP95 = 33.0f; r = Diagnose(rec);
    Check("...and a problem, first, when the frames are uneven", r.size() >= 2 && r[0].level == Level::Problem && Has(r, "Recording is on"));
    rec.recordingShown = false; rec.frameP95 = 17.5f; r = Diagnose(rec);
    Check("...a plain recorder says so without the presented frames", r.size() == 1 && Has(r, "copies the frames it keeps") && !Has(r, "what is shown"));

    Snapshot many = waiting; many.runEvery = 3; many.keepUpPct = 50; many.showingPlain = true; many.autoOn = false; many.modelMs = 9; many.scale = 0.25f; many.floor = 0.25f;
    Check("at most four findings", Diagnose(many).size() <= 4);

    // source cadence
    {
        nr::cadence::Detector det;
        auto feed = [&](std::initializer_list<int> pattern, int times) { for (int t = 0; t < times; ++t) for (int stay : pattern) { det.Push(false); for (int k = 1; k < stay; ++k) det.Push(true); } };
        feed({ 1 }, 80); Check("a game at its real rate (every frame new) is no repeat", det.Verdict().repeats == 0);
        det.Reset(); feed({ 2 }, 40); Check("each picture twice (30 in a 60 feed): 2", det.Verdict().repeats == 2);
        det.Reset(); feed({ 4 }, 40); Check("each picture four times (30 in a 120 feed): 4", det.Verdict().repeats == 4);
        det.Reset(); feed({ 2 }, 5); Check("too few stays to say", det.Verdict().repeats == 0);
        det.Reset(); feed({ 1 }, 10); det.Push(false); for (int i = 0; i < 400; ++i) det.Push(true); Check("a still scene (one very long stay) is no repeat", det.Verdict().repeats == 0);
        det.Reset(); feed({ 2, 2, 2, 2, 2, 2, 2, 1 }, 10); Check("one stay in eight odd (a hitch) still reads as 2", det.Verdict().repeats == 2);
        det.Reset(); feed({ 2, 1 }, 40); Check("stays alternating 2 and 1 (a 40 in 60 feed) is not called a repeat", det.Verdict().repeats == 0);
        Snapshot s2; s2.sourceRepeats = 2; s2.sourceFps = 60;
        auto f = Diagnose(s2);
        Check("a repeating source is a note with the capture setting as advice", f.size() == 1 && f[0].level == Level::Note && Has(f, "30 a second") && f[0].tryThis.find("real rate") != std::string::npos);
    }
    printf("\n%s\n", g_failed ? "DIAGNOSIS TEST FAILED" : "DIAGNOSIS TEST PASSED");
    return g_failed ? 1 : 0;
}
