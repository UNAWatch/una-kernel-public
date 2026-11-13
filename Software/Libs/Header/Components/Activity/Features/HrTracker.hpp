/**
 ******************************************************************************
 * @file    HrTracker.hpp
 * @brief   HR sample tracker with validity and average over the last
 *          minute of valid data.
 *
 *          Validity rules:
 *           - bpm > 0 and trustLevel >= skMinTrustLevel - counts as valid.
 *           - At least skMinValidSamples valid entries in the last
 *             skWindowMs window (lowered to skMinValidSamplesStartup
 *             during the first skWindowMs after the most recent valid
 *             sample, so a short probe can still call itself valid).
 *           - Fallback: skStableCount consecutive high-trust samples
 *             (TL >= skStableTl) within skMaxDeltaBpm of one another.
 *           - Grace window: skValidityGraceMs of "still valid" carry-
 *             over after the last valid sample to ride out HR restarts.
 ******************************************************************************
 */

#ifndef COMPONENT_ACTIVITY_FEATURE_HR_TRACKER_HPP
#define COMPONENT_ACTIVITY_FEATURE_HR_TRACKER_HPP

#include <cstdint>

#include "Activity/RingBuffer.hpp"

namespace Activity
{

class HrTracker
{
public:
    /// Push a raw sample from the HR sensor.
    void onSample(uint8_t bpm, uint8_t tl, uint32_t tsMs);

    /// Whether the trailing minute carries enough trustworthy data.
    bool isValidLastMinute(uint32_t nowMs) const;

    /// Mean BPM over the trailing minute (zero if no valid samples).
    /// Computed independently of isValidLastMinute() - caller decides
    /// whether to publish.
    /// @note The window ends at the last valid sample, not at "now", so
    ///       this keeps returning the last average after the sensor stops.
    ///       Gate it with hasRecentValidSample() before recording it.
    float avgLastMinute() const;

    /// Whether the last valid sample is recent enough (within skMaxAgeMs,
    /// two measurement periods) to stand for the current minute. Stays
    /// true between periodic probes while the watch is worn, and turns
    /// false once samples stop arriving (watch off the wrist, HR stopped).
    bool hasRecentValidSample(uint32_t nowMs) const;

private:
    struct Sample
    {
        uint32_t tsMs;
        uint8_t  bpm;
        uint8_t  tl;
    };

    RingBuffer<Sample, 70> mWindow;
    uint32_t               mLastValidTsMs = 0;

    static constexpr uint32_t skMeasurementPeriod      = 5 * 60 * 1000;
    static constexpr uint32_t skWindowMs               = 60000;
    static constexpr uint8_t  skMinTrustLevel          = 2;
    static constexpr uint8_t  skMinValidSamples        = 5;
    static constexpr uint32_t skMaxAgeMs               = 2 * skMeasurementPeriod;
    static constexpr uint32_t skValidityGraceMs        = 10000;
    static constexpr uint8_t  skMinValidSamplesStartup = 2;
    static constexpr uint8_t  skStableTl               = 3;
    static constexpr uint8_t  skStableCount            = 3;
    static constexpr uint8_t  skMaxDeltaBpm            = 3;
};

} // namespace Activity

#endif // COMPONENT_ACTIVITY_FEATURE_HR_TRACKER_HPP
