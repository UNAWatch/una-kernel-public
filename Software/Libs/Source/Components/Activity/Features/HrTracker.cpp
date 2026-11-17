/**
 ******************************************************************************
 * @file    HrTracker.cpp
 ******************************************************************************
 */

#include "Activity/Features/HrTracker.hpp"

#include <cstdlib>

namespace Activity
{

void HrTracker::onSample(uint8_t bpm, uint8_t tl, uint32_t tsMs)
{
    // Detect time rollback / non-monotonic timestamp - drop the window.
    if (!mWindow.empty() && tsMs < mWindow.back().tsMs) {
        mWindow.clear();
        mLastValidTsMs = 0;
    }

    if (bpm > 0 && tl >= skMinTrustLevel) {
        mLastValidTsMs = tsMs;
    }

    mWindow.push_back({tsMs, bpm, tl});

    const uint32_t minTs = (tsMs > skMaxAgeMs) ? (tsMs - skMaxAgeMs) : 0;
    while (!mWindow.empty() && mWindow.front().tsMs < minTs) {
        mWindow.pop_front();
    }
}

bool HrTracker::isValidLastMinute(uint32_t nowMs) const
{
    if (mWindow.empty()) return false;
    if (mLastValidTsMs == 0) return false;

    // Even the last valid sample is too old to vouch for current
    // wearer state.
    if (nowMs - mLastValidTsMs > skMaxAgeMs) {
        return false;
    }

    // Grace window: a valid sample within the last few seconds is
    // enough on its own. Covers brief HR restarts and the moment
    // immediately after a sensor sample lands.
    if (nowMs - mLastValidTsMs <= skValidityGraceMs) {
        return true;
    }

    // Strict check: count valid samples in the trailing minute
    // relative to NOW, not relative to mLastValidTsMs. The latter
    // would let "valid" carry forever between probes - when the HR
    // sensor is off we want this to read false so the FSM can drop
    // STATIONARY for fake-touch scenarios.
    const uint32_t windowEnd   = nowMs;
    const uint32_t windowStart =
            (nowMs > skWindowMs) ? (nowMs - skWindowMs) : 0;

    uint32_t validCount = 0;

    // Variables for the high-trust continuity rule.
    uint8_t stableCount = 0;
    uint8_t lastBpm     = 0;
    bool    hasLastBpm  = false;

    for (size_t i = 0; i < mWindow.size(); ++i) {
        const Sample& s = mWindow[i];

        if (s.tsMs < windowStart || s.tsMs > windowEnd) continue;
        if (s.bpm == 0)                                 continue;
        if (s.tl < skMinTrustLevel)                     continue;

        ++validCount;

        if (s.tl >= skStableTl) {
            if (!hasLastBpm ||
                (std::abs(int(s.bpm) - int(lastBpm)) <= skMaxDeltaBpm))
            {
                ++stableCount;
            } else {
                stableCount = 1;
            }

            lastBpm    = s.bpm;
            hasLastBpm = true;
        }
    }

    // Looser threshold while we are still inside the first window
    // after the most recent valid sample (probe just started, only a
    // few samples landed yet).
    const uint32_t sinceLastValid = nowMs - mLastValidTsMs;
    const uint32_t requiredSamples =
            (sinceLastValid < skWindowMs)
                ? skMinValidSamplesStartup
                : skMinValidSamples;

    if (validCount >= requiredSamples) {
        return true;
    }

    if (stableCount >= skStableCount) {
        return true;
    }

    return false;
}

bool HrTracker::hasRecentValidSample(uint32_t nowMs) const
{
    if (mLastValidTsMs == 0) return false;
    return (nowMs - mLastValidTsMs) <= skMaxAgeMs;
}

float HrTracker::avgLastMinute() const
{
    if (mWindow.empty())     return 0.0f;
    if (mLastValidTsMs == 0) return 0.0f;

    const uint32_t windowEnd   = mLastValidTsMs;
    const uint32_t windowStart =
            (windowEnd > skWindowMs) ? (windowEnd - skWindowMs) : 0;

    uint32_t sum   = 0;
    uint32_t count = 0;

    for (size_t i = 0; i < mWindow.size(); ++i) {
        const Sample& s = mWindow[i];

        if (s.tsMs < windowStart || s.tsMs > windowEnd) continue;
        if (s.bpm == 0)                                 continue;
        if (s.tl < skMinTrustLevel)                     continue;

        sum += s.bpm;
        ++count;
    }

    return (count > 0) ? static_cast<float>(sum) / count : 0.0f;
}

} // namespace Activity
