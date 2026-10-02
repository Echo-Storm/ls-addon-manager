// Offline test of auto quality (auto_quality.cpp) with a simulated model: its time grows with the area it works on, about 2.7 ms plus 1.8 ms a
// megapixel (as measured on an RTX 4070 Ti SUPER), on 2560x1440 frames at 60 per second.
#include "addon/auto_quality.h"
#include <cmath>
#include <cstdio>
#include <string>

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

static float ModelMs(float scale, float load) { return load * (2.7f + 1.8f * (2560.0f * scale) * (1440.0f * scale) / 1e6f); }

struct Run { int changes = 0; uint64_t shortestGap = ~0ull; float lowest = 9, highest = 0; };

// `seconds` of frames at 60 per second; `load` scales the model's time (a busier scene, a hotter card).
static Run Simulate(nr::AutoQuality& a, uint64_t& now, int seconds, float ceiling, const nr::AutoQuality::Settings& s, float load, float frameMs = 16.7f, unsigned gpu = 0) {
    Run r; uint64_t last = 0;
    for (int i = 0; i < seconds * 60; ++i) {
        now += 17;
        const float scale = a.Scale() > 0 ? a.Scale() : ceiling;
        if (a.Update(now, ModelMs(scale, load), frameMs, ceiling, s, gpu)) {
            ++r.changes;
            if (last) r.shortestGap = std::min<uint64_t>(r.shortestGap, now - last);
            last = now;
        }
        r.lowest = std::min(r.lowest, a.Scale()); r.highest = std::max(r.highest, a.Scale());
    }
    return r;
}

int main() {
    using nr::AutoQuality;
    char text[160];
    printf("== a model over its budget\n");
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 5.0f, 0.25f };
        const Run r = Simulate(a, now, 180, 1.0f, s, 1.0f);   // at 1.0 the model takes about 9.3 ms
        snprintf(text, sizeof text, "scale %.2f, model %.1f ms, %d changes", a.Scale(), ModelMs(a.Scale(), 1.0f), r.changes);
        Check("it lowers the model resolution until the model fits the budget", ModelMs(a.Scale(), 1.0f) <= 5.5f && a.Scale() >= 0.25f, text);
        Check("...without overshooting far below what fits", ModelMs(a.Scale() + 0.10f, 1.0f) > 5.0f * 0.9f, text);
        Check("...changing at most once every 5 seconds", r.shortestGap >= 5000, std::to_string(r.shortestGap) + " ms");
        const float settled = a.Scale();
        const Run later = Simulate(a, now, 120, 1.0f, s, 1.0f);
        Check("once it fits, it stays put (no swinging back and forth)", later.changes == 0 && a.Scale() == settled, std::to_string(later.changes) + " changes");

        const Run easier = Simulate(a, now, 300, 1.0f, s, 0.5f);   // the model gets twice as fast: room to go back up
        snprintf(text, sizeof text, "scale %.2f, model %.1f ms", a.Scale(), ModelMs(a.Scale(), 0.5f));
        Check("with room again it goes back up, still within the budget", a.Scale() > settled && ModelMs(a.Scale(), 0.5f) <= 5.0f, text);
        Check("...at most once every 20 seconds going up", easier.shortestGap >= 20000, std::to_string(easier.shortestGap) + " ms");
        Check("...and never above the person's own setting", easier.highest <= 1.0f);
    }

    printf("== limits\n");
    {
        AutoQuality a; uint64_t now = 0;
        Simulate(a, now, 300, 0.8f, { true, 1.0f, 0.4f }, 1.0f);   // a budget it can never meet
        Check("it never goes below the floor", std::fabs(a.Scale() - 0.4f) < 0.001f, std::to_string(a.Scale()));
        Simulate(a, now, 1, 0.3f, { true, 1.0f, 0.4f }, 1.0f);
        Check("a person's setting below the floor wins over the floor", a.Scale() <= 0.3f + 0.001f, std::to_string(a.Scale()));
    }
    {
        AutoQuality a; uint64_t now = 0;
        const Run r = Simulate(a, now, 60, 1.0f, { true, 5.0f, 0.25f }, 1.0f, 250.0f);   // a loading screen: 4 frames a second
        Check("frames slower than 100 ms (loading, a pause) are not judged", r.changes == 0 && a.Scale() == 1.0f);
    }
    {
        AutoQuality a; uint64_t now = 0;
        Simulate(a, now, 120, 1.0f, { true, 5.0f, 0.25f }, 1.0f);
        const Run off = Simulate(a, now, 1, 0.9f, { false, 5.0f, 0.25f }, 1.0f);
        Check("switched off, the person's own setting is used at once", a.Scale() == 0.9f && off.changes == 0);
    }
    {
        AutoQuality a; uint64_t now = 0;
        Simulate(a, now, 120, 1.0f, { true, 5.0f, 0.25f }, 1.0f);
        Check("each change is kept with its time, the scales and the model time", !a.History().empty() && a.History().back().to == a.Scale() &&
              a.History().back().from > a.History().back().to && a.History().back().modelMs > 5.0f);
    }

    printf("== few, direct changes (a ramp of nine steps showed as the picture changing look every few seconds)\n");
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 3.0f, 0.25f };
        const Run r = Simulate(a, now, 120, 1.0f, s, 1.0f);   // 9.3 ms at 1.0, a 3 ms budget: about 0.3 fits
        snprintf(text, sizeof text, "%d changes, scale %.2f, model %.1f ms", r.changes, a.Scale(), ModelMs(a.Scale(), 1.0f));
        Check("from 1.0 to a scale that fits in at most three changes", r.changes <= 3 && ModelMs(a.Scale(), 1.0f) <= 3.0f * 1.1f, text);
        Check("the first change goes straight most of the way (not one small step)", a.History().front().to <= 0.75f, std::to_string(a.History().front().to));
    }
    {
        AutoQuality a; uint64_t now = 0;
        a.Seed(0.3f);
        const AutoQuality::Settings s{ true, 3.0f, 0.25f };
        const Run r = Simulate(a, now, 60, 1.0f, s, 1.0f);
        snprintf(text, sizeof text, "%d changes, scale %.2f, highest %.2f", r.changes, a.Scale(), r.highest);
        Check("seeded with the scale it settled at, it starts there (never at the person's 1.0)", r.highest <= 0.3f + 0.001f, text);
        Check("...and with the model within budget there, it does not change at all", r.changes == 0, text);
        Check("the seed never exceeds the person's own setting", [] { AutoQuality b; b.Seed(0.9f); uint64_t n = 0; Simulate(b, n, 2, 0.5f, { true, 5.0f, 0.25f }, 1.0f); return b.Scale() <= 0.5f + 0.001f; }());
    }
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 5.0f, 0.25f };
        Simulate(a, now, 90, 1.0f, s, 1.0f);
        Check("a scale held for 30 s with the model in budget is reported as stable", a.StableScale(now, 30000) > 0.0f && std::fabs(a.StableScale(now, 30000) - a.Scale()) < 0.001f, std::to_string(a.StableScale(now, 30000)));
        Check("...and not before that", a.StableScale(a.History().back().atMs + 1000, 30000) == 0.0f);
    }
    {
        // the game's frames slow down (the card is out of room): the model is asked to take less, then goes back up only after a while
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 5.0f, 0.25f };
        Simulate(a, now, 60, 0.6f, s, 1.0f, 16.7f);                 // settled at 60 fps, model within budget
        const float before = a.Scale();
        const Run slow = Simulate(a, now, 40, 0.6f, s, 1.0f, 30.0f);   // frames now take 30 ms: a busy scene
        snprintf(text, sizeof text, "scale %.2f -> %.2f, %d changes, pressure %.2f", before, a.Scale(), slow.changes, a.Pressure());
        Check("frame times well over the best it has managed make it lower the model's resolution", a.Scale() < before && a.Pressure() < 0.8f, text);
        const float low = a.Scale();
        const Run back = Simulate(a, now, 90, 0.6f, s, 1.0f, 16.7f);
        Check("...and it stays down for a minute after the frames recover, then may rise", back.lowest >= low - 0.001f, std::to_string(back.lowest));
    }

    printf("== the model on every Nth frame (at the floor, while the game's frames are slow)\n");
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 1.0f, 0.25f };   // a budget it can never meet, so it sits at the floor
        Simulate(a, now, 60, 0.6f, s, 1.0f, 16.7f);            // 60 fps: the best the game has managed
        Check("at 60 fps the model runs on every frame", a.RunEvery() == 1, std::to_string(a.RunEvery()));
        Simulate(a, now, 12, 0.6f, s, 1.0f, 30.0f);            // the game's frames turn slow
        snprintf(text, sizeof text, "every %d, scale %.2f, pressure %.2f", a.RunEvery(), a.Scale(), a.Pressure());
        Check("slow frames at the floor: it moves to every 2nd frame (or 3rd), not further at once", a.RunEvery() >= 2 && a.RunEvery() <= 3 && std::fabs(a.Scale() - 0.25f) < 0.001f, text);
        Simulate(a, now, 40, 0.6f, s, 1.0f, 36.0f);
        Check("...and at most every 3rd frame", a.RunEvery() <= 3, std::to_string(a.RunEvery()));
        const int slowEvery = a.RunEvery();
        Simulate(a, now, 8, 0.6f, s, 1.0f, 16.7f);             // the frames recover, but not for long
        Check("a short calm does not bring the model back to every frame (30 s hold after the last increase)", a.RunEvery() == slowEvery, std::to_string(a.RunEvery()));
        Simulate(a, now, 60, 0.6f, s, 1.0f, 16.7f);            // a minute of steady frames
        Check("after the frames have been steady for a while it runs on every frame again", a.RunEvery() == 1, std::to_string(a.RunEvery()));
    }
    {   // the model skipping frames lets the game recover, going back to every frame slows it again: a cycle every minute; each time the hold is longer
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 1.0f, 0.25f };
        Simulate(a, now, 60, 0.6f, s, 1.0f, 16.7f);
        Simulate(a, now, 12, 0.6f, s, 1.0f, 30.0f);            // slow: every 2nd or 3rd
        Simulate(a, now, 60, 0.6f, s, 1.0f, 16.7f);            // recovered (the skipping's doing): back to every frame
        Check("the first return to every frame comes after a minute of calm", a.RunEvery() == 1, std::to_string(a.RunEvery()));
        Simulate(a, now, 12, 0.6f, s, 1.0f, 30.0f);            // slow again at once
        Check("slow again soon after going back: every 2nd or 3rd again", a.RunEvery() >= 2, std::to_string(a.RunEvery()));
        Simulate(a, now, 45, 0.6f, s, 1.0f, 16.7f);            // 45 s of calm: with the old 30 s hold it would be back already
        Check("...and the hold is longer now: 45 s of calm is not enough", a.RunEvery() >= 2, std::to_string(a.RunEvery()));
        Simulate(a, now, 90, 0.6f, s, 1.0f, 16.7f);
        Check("...but a long calm does bring it back", a.RunEvery() == 1, std::to_string(a.RunEvery()));
    }
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 5.0f, 0.25f };    // a budget it can meet by lowering the resolution
        Simulate(a, now, 60, 0.6f, s, 0.3f, 16.7f);
        Simulate(a, now, 40, 0.6f, s, 0.3f, 30.0f);            // slow frames, but the scale is above the floor
        Check("above the floor the resolution is what moves, not how often the model runs", a.RunEvery() == 1 || a.Scale() <= 0.25f + 0.001f, "every " + std::to_string(a.RunEvery()) + ", scale " + std::to_string(a.Scale()));
    }
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings off{ false, 1.0f, 0.25f };
        Simulate(a, now, 30, 0.6f, off, 1.0f, 30.0f);
        Check("auto quality off: the model always runs on every frame", a.RunEvery() == 1);
    }

    printf("== keeping the card under a limit\n");
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 12.0f, 0.4f, 95.0f };   // a budget the model fits at 1.0 (9.3 ms)
        Simulate(a, now, 60, 1.0f, s, 1.0f, 16.7f, 80);
        const float before = a.Scale();
        Check("under the limit it changes nothing", before == 1.0f && !a.GpuOver(), "scale " + std::to_string(before));
        Simulate(a, now, 2, 1.0f, s, 1.0f, 16.7f, 99);
        Check("a card over the limit for less than 3 s is not yet pressure", !a.GpuOver());
        Simulate(a, now, 30, 1.0f, s, 1.0f, 16.7f, 99);
        snprintf(text, sizeof text, "scale %.2f -> %.2f, pressure %.2f", before, a.Scale(), a.Pressure());
        Check("over the limit for a while it counts as pressure and the resolution comes down", a.GpuOver() && a.Pressure() < 0.7f && a.Scale() < before - 0.05f, text);
        const float lowered = a.Scale();
        Simulate(a, now, 20, 1.0f, s, 1.0f, 16.7f, 92);
        Check("a load just under the limit (not 8 points under) keeps it", a.GpuOver());
        Simulate(a, now, 15, 1.0f, s, 1.0f, 16.7f, 70);
        Check("8 points under the limit for 10 s lets go", !a.GpuOver() && a.Pressure() > 0.99f);
        Simulate(a, now, 240, 1.0f, s, 1.0f, 16.7f, 70);
        snprintf(text, sizeof text, "scale %.2f -> %.2f", lowered, a.Scale());
        Check("and the resolution goes back up in time", a.Scale() > lowered + 0.05f, text);
    }
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 5.0f, 0.4f, 95.0f };   // the model takes more than the budget even at the floor
        Simulate(a, now, 120, 1.0f, s, 1.0f, 16.7f, 99);
        Check("at the floor with the card over its limit the model runs less often", a.Scale() <= 0.4f + 0.001f && a.RunEvery() >= 2, "scale " + std::to_string(a.Scale()) + ", every " + std::to_string(a.RunEvery()));
    }
    {
        AutoQuality a; uint64_t now = 0;
        const AutoQuality::Settings s{ true, 12.0f, 0.4f, 95.0f };
        Simulate(a, now, 90, 1.0f, s, 1.0f, 16.7f, 0);   // 0: the load is not known (no NVML)
        Check("a load that is not known changes nothing", a.Scale() == 1.0f && !a.GpuOver());
        const AutoQuality::Settings noLimit{ true, 12.0f, 0.4f, 0.0f };
        AutoQuality b; uint64_t t2 = 0;
        Simulate(b, t2, 90, 1.0f, noLimit, 1.0f, 16.7f, 100);
        Check("without the limit asked for, a busy card changes nothing", b.Scale() == 1.0f && !b.GpuOver());
    }

    printf("\n%s\n", g_failed ? "AUTO QUALITY TEST FAILED" : "AUTO QUALITY TEST PASSED");
    return g_failed ? 1 : 0;
}
