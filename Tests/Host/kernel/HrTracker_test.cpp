#include <gtest/gtest.h>

#include "Activity/Features/HrTracker.hpp"

// HR freshness for the per-minute HR that DailyHealth records.
//
// avgLastMinute() averages the minute ending at the last valid sample, so
// once samples stop it keeps returning the last average. SensorPolicy used to
// publish that unconditionally, and DailyHealth booked it into every minute:
// a watch taken off at midnight recorded a flat HR until morning, and that
// flat stretch became the day's resting HR. hasRecentValidSample() is the gate
// SensorPolicy now applies; these tests pin where it opens and closes.

using Activity::HrTracker;

namespace
{
constexpr uint8_t  kGoodTl   = 3;                 // trusted sample
constexpr uint32_t kMinuteMs = 60u * 1000u;
constexpr uint32_t kMaxAgeMs = 10u * kMinuteMs;   // 2 x 5 min measurement period

// One sample per second over [startMs, startMs + durationMs).
void feed(HrTracker& t, uint8_t bpm, uint32_t startMs, uint32_t durationMs,
          uint8_t tl = kGoodTl)
{
    for (uint32_t ts = startMs; ts < startMs + durationMs; ts += 1000u) {
        t.onSample(bpm, tl, ts);
    }
}
} // namespace

// --- Why the gate exists: the average itself never goes stale -------------

TEST(HrTracker, AverageHoldsAfterSamplesStop)
{
    HrTracker t;
    feed(t, 69, 1000, kMinuteMs);

    // Nothing arrives for hours (watch off the wrist). The average is
    // anchored to the last sample, so it still reads 69.
    EXPECT_FLOAT_EQ(t.avgLastMinute(), 69.0f);
}

// --- Fresh while samples keep landing --------------------------------------

TEST(HrTracker, RecentRightAfterASample)
{
    HrTracker t;
    feed(t, 72, 1000, kMinuteMs);
    const uint32_t last = 1000 + kMinuteMs - 1000;

    EXPECT_TRUE(t.hasRecentValidSample(last));
    EXPECT_TRUE(t.hasRecentValidSample(last + kMinuteMs));
}

// --- Periodic probes while worn keep it fresh between probes ---------------

TEST(HrTracker, StaysRecentBetweenPeriodicProbes)
{
    HrTracker t;
    // A short probe every 5 minutes, as in STATIONARY.
    for (uint32_t probe = 0; probe < 6; ++probe) {
        const uint32_t start = 1000 + probe * 5u * kMinuteMs;
        feed(t, 64, start, 15000);
        // Just before the next probe lands, the last one still counts.
        EXPECT_TRUE(t.hasRecentValidSample(start + 5u * kMinuteMs - 1000));
    }
}

// --- The regression: stale once samples stop for longer than max age ------

TEST(HrTracker, NotRecentOnceSamplesStop)
{
    HrTracker t;
    feed(t, 69, 1000, kMinuteMs);
    const uint32_t last = 1000 + kMinuteMs - 1000;

    EXPECT_TRUE(t.hasRecentValidSample(last + kMaxAgeMs));
    EXPECT_FALSE(t.hasRecentValidSample(last + kMaxAgeMs + 1));
    EXPECT_FALSE(t.hasRecentValidSample(last + 7u * 60u * kMinuteMs));   // overnight
}

// --- Untrusted or empty readings don't count as fresh ----------------------

TEST(HrTracker, NotRecentWithoutValidSamples)
{
    HrTracker t;
    EXPECT_FALSE(t.hasRecentValidSample(5000));

    feed(t, 0,  1000, 10000);          // no reading
    feed(t, 80, 11000, 10000, 1);      // below the trust threshold
    EXPECT_FALSE(t.hasRecentValidSample(21000));
}

TEST(HrTracker, InvalidSamplesDoNotRefreshAStaleReading)
{
    HrTracker t;
    feed(t, 70, 1000, 10000);
    const uint32_t last = 10000;

    // The sensor keeps reporting, but with no trusted value.
    feed(t, 0, last + kMinuteMs, kMaxAgeMs);
    EXPECT_FALSE(t.hasRecentValidSample(last + kMaxAgeMs + kMinuteMs));
}
