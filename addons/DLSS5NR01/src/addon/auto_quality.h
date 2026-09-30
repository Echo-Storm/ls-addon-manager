// Auto quality: keeps the model within a time budget by lowering its working scale (the "Model resolution") when it runs over, and raising it
// again toward the person's own setting when there is room. The person's setting is the ceiling, a floor they choose is the lowest it goes, and
// nothing that changes the look is touched.
//
// Every change of the working scale has the model's feature made again (in the background, but the model pauses for a fraction of a second
// meanwhile, and its own history starts afresh, which shows), so it changes rarely and in as few steps as it can:
//   * it goes straight to the scale where the model's time is expected to fit (the time grows with the area, so from 13 ms at 1.0 with a 3 ms
//     budget it goes to about 0.47 at once, not through nine steps), then corrects once from the time it measures there;
//   * down after 3 s over the budget (and at least 5 s after the last change), up after 10 s well under it (and 20 s after the last change),
//     and only up to a scale where the time it expects still fits;
//   * it can start from the scale it settled at last time (Seed), so a session does not begin with a ramp down from the person's setting;
//   * when the game's own frames get slow (the frame time well over the best it has lately managed: the graphics card is out of room for
//     everything), the budget shrinks in proportion, and it does not go back up for a minute after that.
// Frames slower than 100 ms (a loading screen, a pause) are not judged.
#pragma once
#include <cstdint>
#include <deque>

namespace nr {

class AutoQuality {
public:
    struct Settings { bool on = false; float budgetMs = 5.0f; float floor = 0.25f; };
    struct Step { uint64_t atMs; float from, to, modelMs; };

    // At every model run: the time now, the model's last time, the time between frames and the person's working scale. True when the scale
    // it wants changed (Scale() and the newest Step say to what, and why).
    bool Update(uint64_t nowMs, float modelMs, float frameIntervalMs, float ceiling, const Settings& s);
    // The working scale to run the model at: the person's own when auto is off.
    float Scale() const { return m_scale; }
    float AverageMs() const { return m_avgMs; }
    // The scale to start from instead of the person's own (their setting stays the ceiling); only before the first Update.
    void Seed(float scale) { if (m_scale <= 0 && scale > 0) m_scale = scale; }
    // The scale, if it has been held for at least holdMs with the model within budget (else 0): worth keeping for the next session.
    float StableScale(uint64_t nowMs, uint64_t holdMs) const { return m_scale > 0 && m_settled && m_lastChange + holdMs <= nowMs ? m_scale : 0.0f; }
    // The share of the budget left by frame pressure (1: none).
    float Pressure() const { return m_pressure; }
    const std::deque<Step>& History() const { return m_history; }

private:
    bool Change(uint64_t nowMs, float to);
    float ScaleFor(float targetMs) const;
    float m_prevScale = 0, m_prevMs = 0;   // the measurement at the scale before the last change
    float m_scale = 0;           // 0 until the first update
    float m_avgMs = 0;
    float m_frameAvg = 0, m_frameBase = 0, m_pressure = 1.0f;   // the game's frame time: now, and the best it has lately managed
    bool m_settled = false;                                       // the model fits the budget at the current scale
    uint64_t m_overSince = 0, m_underSince = 0, m_lastChange = 0, m_noRaiseUntil = 0;
    std::deque<Step> m_history;  // the newest last, at most 8
};

} // namespace nr
