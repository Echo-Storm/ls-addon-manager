// Offline test of the "what is wrong" rules (src/addon/diagnosis.cpp).
#include "addon/diagnosis.h"
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

    Snapshot many = waiting; many.runEvery = 3; many.keepUpPct = 50; many.showingPlain = true; many.autoOn = false; many.modelMs = 9; many.scale = 0.25f; many.floor = 0.25f;
    Check("at most four findings", Diagnose(many).size() <= 4);

    printf("\n%s\n", g_failed ? "DIAGNOSIS TEST FAILED" : "DIAGNOSIS TEST PASSED");
    return g_failed ? 1 : 0;
}
