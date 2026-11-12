/**
 ******************************************************************************
 * @file    FeatureCenter.cpp
 ******************************************************************************
 */

#include "Activity/FeatureCenter.hpp"

#define LOG_MODULE_PRX      "Activity.Features"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

namespace Activity
{

FeatureCenter::FeatureCenter()
{
    mMutex.init(LOG_MODULE_PRX);
}

void FeatureCenter::attachTrigger(IFsmTrigger& trigger)
{
    mTrigger = &trigger;
}

// ============================================================================
// Sink side
// ============================================================================

void FeatureCenter::onStepCounter(uint32_t total, uint32_t tsMs)
{
    OS::MutexCS cs(mMutex);
    mSteps.onUpdate(total, tsMs);
}

void FeatureCenter::resetStepCounter(uint32_t total, uint32_t tsMs)
{
    OS::MutexCS cs(mMutex);
    mSteps.reset(total, tsMs);
}

void FeatureCenter::onHrSample(uint8_t bpm, uint8_t tl, uint32_t tsMs)
{
    OS::MutexCS cs(mMutex);
    mHr.onSample(bpm, tl, tsMs);
    // Forward to ActivityTracker's HR-evidence window. The tracker
    // rate-limits and trust-filters internally, so it's safe to feed
    // every sample (including the continuous-mode stream in ACTIVE).
    mActivity.onHrSample(bpm, tl, tsMs);
}

// Out-of-cadence FSM ticks fire on rising edges only. Falling edges
// (touch released, motion stopped) are unconditionally followed by a
// timeout in the FSM logic itself (skNoTouchTimeout, isNoMotion(180),
// etc.), so an immediate wake-up cannot bring the transition forward
// - the next periodic tick is just as soon. Triggering only on rising
// edges keeps wake-ups proportional to actual user activity.

void FeatureCenter::onMotionUpdate(bool detected, uint32_t tsMs)
{
    bool risingEdge;
    {
        OS::MutexCS cs(mMutex);
        risingEdge = (!mMotion.raw() && detected);
        mMotion.onUpdate(detected, tsMs);
    }

    if (risingEdge && mTrigger) {
        mTrigger->requestFsmTick();
    }
}

void FeatureCenter::onTouchUpdate(bool touched, uint32_t tsMs)
{
    bool risingEdge;
    {
        OS::MutexCS cs(mMutex);
        risingEdge = (!mTouch.raw() && touched);
        mTouch.onUpdate(touched, tsMs);
    }

    if (risingEdge && mTrigger) {
        mTrigger->requestFsmTick();
    }
}

float FeatureCenter::currentAvgHr() const
{
    OS::MutexCS cs(mMutex);
    return mHr.avgLastMinute();
}

bool FeatureCenter::hasRecentHr() const
{
    OS::MutexCS cs(mMutex);
    return mHr.hasRecentValidSample(OS::GetTimeMs());
}

uint32_t FeatureCenter::currentActivityDuration()
{
    OS::MutexCS cs(mMutex);
    const uint32_t now           = OS::GetTimeMs();
    const uint32_t stepsLastMin  = mSteps.lastMinute(now);

    // "User moving" gate for the HR-path: instantaneous motionRaw is
    // too jittery (e.g. vehicle vibration flips it on and off), so we
    // require either some step activity in the trailing minute OR
    // motion that has been continuous for at least 30 s. Same
    // criterion the FSM uses to leave STATIONARY into LIGHT_ACTIVE.
    const bool     isUserMoving  = (stepsLastMin > 0)
                                || mMotion.isMotion(30, now);

    return mActivity.tick(now, isUserMoving, stepsLastMin);
}

void FeatureCenter::resetActivityDuration(uint32_t baselineMinutes, uint32_t tsMs)
{
    OS::MutexCS cs(mMutex);
    mActivity.reset(baselineMinutes, tsMs);
}

void FeatureCenter::setActivityHrThresholdBpm(uint8_t bpm)
{
    OS::MutexCS cs(mMutex);
    mActivity.setHrThresholdBpm(bpm);
}

// ============================================================================
// IFeatureSource
// ============================================================================

uint32_t FeatureCenter::nowMs() const
{
    return OS::GetTimeMs();
}

uint32_t FeatureCenter::stepsLastMinute() const
{
    OS::MutexCS cs(mMutex);
    return mSteps.lastMinute(OS::GetTimeMs());
}

bool FeatureCenter::isHrValidLastMinute() const
{
    OS::MutexCS cs(mMutex);
    return mHr.isValidLastMinute(OS::GetTimeMs());
}

bool FeatureCenter::isActuallyTouched() const
{
    OS::MutexCS cs(mMutex);
    return mTouch.isActuallyTouched(OS::GetTimeMs());
}

bool FeatureCenter::isMotion(uint32_t sec) const
{
    OS::MutexCS cs(mMutex);
    return mMotion.isMotion(sec, OS::GetTimeMs());
}

bool FeatureCenter::isNoMotion(uint32_t sec) const
{
    OS::MutexCS cs(mMutex);
    return mMotion.isNoMotion(sec, OS::GetTimeMs());
}

bool FeatureCenter::touchRaw() const
{
    OS::MutexCS cs(mMutex);
    return mTouch.raw();
}

bool FeatureCenter::motionRaw() const
{
    OS::MutexCS cs(mMutex);
    return mMotion.raw();
}

} // namespace Activity
