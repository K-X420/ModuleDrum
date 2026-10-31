#include "MdSequencer.h"
#include <algorithm>
#include <cmath>

namespace mnm::md {

void PatternPlayer::set(const mddump::Pattern& p)
{
    m_p = p;
    m_len = std::clamp(int(p.length), 1, p.extended ? 64 : 32);
    m_start = 0;
    m_span = m_len;
    m_step = stepClocks(p.doubleTempo);   // the multiplier byte (+0x92): 0 1x, 1 2x, 2 3/4x, 3 3/2x (4-7: the plugin's extra speeds)
    m_swing = double(m_step) * double(p.swingAmount) / 16384.0;
    m_swingWhole = int((int64_t(m_step) * int64_t(p.swingAmount)) >> 14);
    for (int t = 0; t < 16; ++t)
        for (int q = 0; q < 24; ++q) m_row[size_t(t)][size_t(q)] = int16_t(p.lockRow(t, q));
}

void PatternPlayer::setRange(int start, int end)
{
    m_start = std::clamp(start, 0, m_len - 1);
    if (end <= m_start || end > m_len) end = m_len;
    m_span = end - m_start;
}

int PatternPlayer::lock(int track, int param, int step) const
{
    const int row = m_row[size_t(track)][size_t(param)];
    if (row < 0 || step < 0 || step >= 64) return -1;
    const int v = m_p.locks[row][step];
    return v <= 127 ? v : -1;
}

double PatternPlayer::trackStepClocks(const mddump::Pattern& p, int track, bool extras)
{
    const double step = stepClocks(p.doubleTempo);
    if (!extras || track < 0 || track > 15 || !p.trackSpeed[track]) return step;
    int num = 1, den = 1;
    mddump::trackSpeedRatio(p.trackSpeed[track], num, den);
    return step * den / num;
}

double PatternPlayer::tailClocks(bool extras) const
{
    if (!extras) return m_swing;
    double slowest = m_step;
    for (int t = 0; t < 16; ++t) slowest = std::max(slowest, trackStep(t, true));
    return slowest * double(m_p.swingAmount) / 16384.0 + slowest * 9.0;
}

SeqTrig PatternPlayer::trigAt(int64_t stepIndex, int t, bool extras) const
{
    SeqTrig e;
    const int span = spanOf(t, extras);
    const double sc = trackStep(t, extras);
    const double swing = sc == m_step ? m_swing : sc * double(m_p.swingAmount) / 16384.0;
    const int swingWhole = sc == m_step ? m_swingWhole : int(std::lround(swing - 0.5));   // the OS's whole clocks (a slide's)
    e.stepIndex = stepIndex;
    e.step = stepOf(stepIndex, t, extras);
    e.track = t;
    e.clock = double(stepIndex) * sc + (swung(t, e.step) ? swing : 0.0);
    e.accent = accented(t, e.step);
    e.locks.fill(-1);
    e.slideTo.fill(-1);
    for (int q = 0; q < 24; ++q) e.locks[size_t(q)] = int16_t(lock(t, q, e.step));
    if (slid(t, e.step)) {
        // the track's next trig: steps on (wrapping in the range; itself when it is the only one)
        auto after = [&](int s) { return s + 1 >= m_start + span ? m_start : s + 1; };
        int n = 1, next = after(e.step);
        while (!hasTrig(t, next) && n < span) { ++n; next = after(next); }
        int32_t clocks = sc == m_step ? n * m_step : int32_t(std::lround(n * sc));
        const bool a = swung(t, e.step), b = swung(t, next);
        if (a != b) clocks += a ? -swingWhole : swingWhole;
        if (clocks == 0) clocks = 1;
        for (int q = 0; q < 24; ++q) {
            if (e.locks[size_t(q)] < 0) continue;
            e.slideMask |= 1u << q;
            e.slideTo[size_t(q)] = int16_t(lock(t, q, next));
            e.slideClocks[size_t(q)] = clocks;
        }
    }
    return e;
}

namespace {
uint32_t mix(uint32_t x)   // a hash: the probabilities' dice, the same for the same seed, step and track
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}
bool refersBack(int c) { return c == mddump::kCondPre || c == mddump::kCondNotPre || c == mddump::kCondNei || c == mddump::kCondNotNei; }
}

bool PatternPlayer::plainCondition(int64_t k, int t, const TrigContext& ctx) const
{
    const int c = m_p.cond[t][stepOf(k, t, true)];
    if (c == 0) return true;
    if (const int pct = mddump::conditionPercent(c); pct > 0)
        return int(mix(ctx.seed ^ mix(uint32_t(k) * 16u + uint32_t(t) + 0x9E3779B9u)) % 100u) < pct;
    const int64_t pass = k / spanOf(t, true);   // the track's own passes
    switch (c) {
        case mddump::kCondFill: return ctx.fill;
        case mddump::kCondNotFill: return !ctx.fill;
        case mddump::kCondFirst: return pass == 0;
        case mddump::kCondNotFirst: return pass != 0;
        default: break;
    }
    int a = 0, b = 0;
    if (mddump::conditionRatio(c, a, b)) return pass % b == a - 1;
    return true;
}

bool PatternPlayer::lastCondition(int64_t k, int t, bool inclusive, const TrigContext& ctx) const
{
    if (t < 0 || t > 15) return false;
    const int64_t stop = std::max<int64_t>(0, k - 64);
    for (int64_t j = inclusive ? k : k - 1; j >= stop; --j) {
        const int s = stepOf(j, t, true), c = m_p.cond[t][s];
        if (hasTrig(t, s) && c != 0 && !refersBack(c)) return plainCondition(j, t, ctx);
    }
    return false;
}

int64_t PatternPlayer::neighbourStep(int64_t k, int t) const
{
    if (t < 1) return k;
    const double a = trackStep(t, true), b = trackStep(t - 1, true);
    return a == b ? k : int64_t(std::floor(double(k) * a / b + 1e-9));   // the track before's step at the same time
}

bool PatternPlayer::conditionPlays(int64_t k, int t, const TrigContext& ctx) const
{
    const int c = m_p.cond[t][stepOf(k, t, true)];
    switch (c) {
        case mddump::kCondPre: return lastCondition(k, t, false, ctx);
        case mddump::kCondNotPre: return !lastCondition(k, t, false, ctx);
        case mddump::kCondNei: return lastCondition(neighbourStep(k, t), t - 1, true, ctx);   // the track before plays first
        case mddump::kCondNotNei: return !lastCondition(neighbourStep(k, t), t - 1, true, ctx);
        default: return plainCondition(k, t, ctx);
    }
}

void PatternPlayer::trigs(double from, double to, std::vector<SeqTrig>& out, int64_t maxSteps, const TrigContext& ctx) const
{
    if (to <= from) return;
    const size_t first = out.size();
    if (!ctx.extras) {   // as the unit: one step clock for every track
        const double reach = m_swing;   // a step's trigs: at its start, or a swing delay later
        int64_t k = std::max<int64_t>(0, int64_t(std::floor((from - reach) / m_step)));
        for (; double(k) * m_step < to && (maxSteps < 0 || k < maxSteps); ++k)
            for (int t = 0; t < 16; ++t) {
                const int step = stepOf(k, t, false);
                if (!hasTrig(t, step)) continue;
                const double c = double(k) * m_step + (swung(t, step) ? m_swing : 0.0);
                if (c < from || c >= to) continue;
                out.push_back(trigAt(k, t));
            }
    } else {
        // each track on its own step clock (its speed), its steps counted from the origin; the segment ends at the
        // same clock for every track. A step's trigs: from its start (a nudge early) to its retrigs up to 8 steps on.
        const double end = maxSteps < 0 ? -1.0 : double(maxSteps) * m_step;
        for (int t = 0; t < 16; ++t) {
            if (!m_p.trigs[t] && !m_p.lockTrigs[t]) continue;
            const double sc = trackStep(t, true);
            const double swing = sc == m_step ? m_swing : sc * double(m_p.swingAmount) / 16384.0;
            const double reach = swing + sc * 9.0;
            for (int64_t k = std::max<int64_t>(0, int64_t(std::floor((from - reach) / sc))); double(k) * sc < to + sc; ++k) {
                if (end >= 0 && double(k) * sc >= end - 1e-9) break;
                const int step = stepOf(k, t, true);
                const bool trig = hasTrig(t, step), lockTrig = !trig && hasLockTrig(t, step);
                if (!trig && !lockTrig) continue;
                double c = double(k) * sc + (swung(t, step) ? swing : 0.0);
                c = std::max(0.0, c + microClocks(t, step, sc));   // (a nudge early on the very first step: at the start)
                const uint8_t r = trig ? m_p.retrig[t][step] : 0;
                const int hits = mddump::retrigHits(r) * (r ? mddump::retrigSteps(r) : 1);
                const double gap = r ? sc / mddump::retrigHits(r) : 0.0;
                const double last = c + gap * std::max(0, hits - 1);
                if (last < from || c >= to) continue;
                if (!conditionPlays(k, t, ctx)) continue;
                SeqTrig e = trigAt(k, t, true);
                if (lockTrig) {   // its locks only, there (no slide from it)
                    e.clock = c;
                    e.lockOnly = true;
                    e.slideMask = 0;
                    if (c >= from && c < to) out.push_back(e);
                    continue;
                }
                const int fade = m_p.retrigFade[t][step];
                for (int h = 0; h < std::max(1, hits); ++h) {
                    const double at = c + gap * h;
                    if (at < from || at >= to) continue;
                    SeqTrig x = e;
                    x.clock = at;
                    x.retrig = h > 0;
                    x.gain = mddump::retrigGain(fade, h, hits);
                    if (h > 0) x.slideMask = 0;
                    out.push_back(x);
                }
            }
        }
    }
    std::stable_sort(out.begin() + std::ptrdiff_t(first), out.end(), [](const SeqTrig& a, const SeqTrig& b) { return a.clock < b.clock; });
}

void Glide::start(int from, int to, int32_t clocks, double trigClock)
{
    value = from << 7;
    end = to << 7;
    rate = (end - value) / std::max<int32_t>(1, clocks);   // divs: toward zero
    active = rate != 0;
    nextClock = std::floor(trigClock) + 1.0;               // the first step at the next clock
}

bool Glide::advance(double now)
{
    bool changed = false;
    while (active && nextClock <= now) {
        const int32_t v = value + rate;
        if ((rate > 0 && v > end) || (rate < 0 && v < end)) { active = false; break; }   // stops short of the end
        if ((v >> 7) != (value >> 7)) changed = true;
        value = v;
        nextClock += 1.0;
    }
    return changed;
}

} // namespace mnm::md
