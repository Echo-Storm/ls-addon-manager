#include "addon/auto_quality.h"
#include <algorithm>
#include <cmath>

namespace nr {

namespace {
constexpr float kStep = 0.05f;
constexpr uint64_t kOverFor = 3000, kUnderFor = 10000, kDownPause = 5000, kUpPause = 20000, kPressureHold = 60000;
constexpr float kPauseFrameMs = 100.0f;
constexpr float kPressureFrom = 1.25f;   // the frame time this far over its best counts as pressure
constexpr float kAim = 0.92f;            // aim a little under the budget, so the next measurement does not land just over it
constexpr float kMaxDrop = 0.5f;         // never more than halve the scale in one change (the estimate is a model, not a measurement)
}

// The scale where the model's time is expected to be `targetMs`. Its time is a fixed part plus a part that grows with the area (scale squared): with two
// measurements at different scales both parts are known; with one, the fixed part is taken as 1 ms (what the RTX 4070 Ti SUPER showed: 13.6 ms at 1.0, about 4 at 0.5).
float AutoQuality::ScaleFor(float targetMs) const {
    float fixedMs = std::min(1.0f, m_avgMs * 0.3f), perArea = (m_avgMs - fixedMs) / (m_scale * m_scale);
    if (m_prevMs > 0 && std::fabs(m_prevScale - m_scale) > 0.02f) {
        const float b = (m_prevMs - m_avgMs) / (m_prevScale * m_prevScale - m_scale * m_scale);
        const float a = m_avgMs - b * m_scale * m_scale;
        if (b > 0 && a >= 0 && a < m_avgMs) { fixedMs = a; perArea = b; }
    }
    if (targetMs <= fixedMs || perArea <= 0) return 0.0f;   // not reachable: the floor
    return std::sqrt((targetMs - fixedMs) / perArea);
}

bool AutoQuality::Change(uint64_t nowMs, float to) {
    m_prevScale = m_scale; m_prevMs = m_avgMs;
    m_history.push_back({ nowMs, m_scale, to, m_avgMs });
    if (m_history.size() > 8) m_history.pop_front();
    m_scale = to;
    m_lastChange = nowMs;
    m_overSince = m_underSince = 0;
    m_avgMs = 0;   // measured afresh at the new scale
    m_settled = false;
    return true;
}

bool AutoQuality::Update(uint64_t nowMs, float modelMs, float frameIntervalMs, float ceiling, const Settings& s) {
    const float floor = std::min(s.floor, ceiling);
    if (!s.on || m_scale <= 0 || m_scale > ceiling) {   // off, the first time, or the person lowered their own setting below it
        const bool changed = m_scale != ceiling && m_scale > 0 && s.on;
        m_scale = ceiling;
        m_overSince = m_underSince = 0;
        m_settled = false;
        if (!s.on) { m_avgMs = 0; return false; }
        return changed;
    }
    if (m_scale < floor - 0.001f) m_scale = floor;   // a seed (or an old setting) below a floor raised since
    if (modelMs <= 0 || frameIntervalMs > kPauseFrameMs) { m_overSince = m_underSince = 0; return false; }
    m_avgMs = m_avgMs == 0 ? modelMs : m_avgMs * 0.9f + modelMs * 0.1f;

    // frame pressure: the game's real frame time against the best it has lately managed (the best creeps up, so a scene that is heavy for good stops counting)
    if (frameIntervalMs > 0) {
        m_frameAvg = m_frameAvg == 0 ? frameIntervalMs : m_frameAvg * 0.98f + frameIntervalMs * 0.02f;
        if (m_frameBase == 0 || m_frameAvg < m_frameBase) m_frameBase = m_frameAvg;
        else m_frameBase += (m_frameAvg - m_frameBase) * 0.0002f;
        const bool pressed = m_frameAvg > m_frameBase * kPressureFrom;
        m_pressure = pressed ? std::max(0.4f, m_frameBase / m_frameAvg) : 1.0f;
        if (pressed) m_noRaiseUntil = nowMs + kPressureHold;
    }
    const float budget = s.budgetMs * m_pressure;

    if (m_avgMs > budget * 1.1f && m_scale > floor + 0.001f) {
        m_underSince = 0; m_settled = false;
        if (!m_overSince) m_overSince = nowMs;
        if (nowMs - m_overSince < kOverFor || (m_lastChange && nowMs - m_lastChange < kDownPause)) return false;
        return Change(nowMs, std::clamp(ScaleFor(budget * kAim), std::max(floor, m_scale * kMaxDrop), m_scale - 0.01f));
    }
    m_settled = m_avgMs <= budget * 1.1f;
    if (m_avgMs < budget * 0.7f && m_scale < ceiling - 0.001f) {
        m_overSince = 0;
        if (!m_underSince) m_underSince = nowMs;
        if (nowMs - m_underSince < kUnderFor || (m_lastChange && nowMs - m_lastChange < kUpPause) || nowMs < m_noRaiseUntil) return false;
        const float to = std::min(ceiling, m_scale + kStep);
        const float ratio = to / m_scale;
        if (m_avgMs * ratio * ratio > budget * 0.9f) return false;   // the model's time grows with the area it works on
        return Change(nowMs, to);
    }
    m_overSince = m_underSince = 0;
    return false;
}

} // namespace nr
