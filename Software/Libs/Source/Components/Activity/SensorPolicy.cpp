/**
 ******************************************************************************
 * @file    SensorPolicy.cpp
 * @brief   Sensor adapter - parses sensor frames, forwards normalised
 *          events to FeatureCenter, exposes the cumulative-from-boot
 *          snapshot via IActivityDataSource.
 ******************************************************************************
 */

#include "Activity/SensorPolicy.hpp"

#define LOG_MODULE_PRX      "Activity.Sensors"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include "SDK/SensorLayer/DataParsers/SensorDataParserStepCounter.hpp"
#include "SDK/SensorLayer/DataParsers/SensorDataParserFloorCounter.hpp"
#include "SDK/SensorLayer/DataParsers/SensorDataParserHeartRate.hpp"
#include "SDK/SensorLayer/DataParsers/SensorDataParserMotionDetect.hpp"
#include "SDK/SensorLayer/DataParsers/SensorDataParserTouch.hpp"
#include "SensorLayer/Core/SensorManager.hpp"
#include "SDK/SensorLayer/SensorDataBatch.hpp"

namespace Activity
{

SensorPolicy::SensorPolicy(Activity::FeatureCenter& features)
    : mFeatures(features)
    , mDataMutex()
    , mDriverPedo(SDK::Sensor::Type::STEP_COUNTER, this)
    , mDriverFloor(SDK::Sensor::Type::FLOOR_COUNTER, this)
    , mDriverHR(SDK::Sensor::Type::HEART_RATE, this)
    , mDriverMotion(SDK::Sensor::Type::MOTION_DETECT, this)
    , mDriverTouch(SDK::Sensor::Type::TOUCH_DETECT, this)
{
    mDataMutex.init(LOG_MODULE_PRX);
}

void SensorPolicy::onActivityDurationChanged(uint32_t minutesSinceBoot)
{
    OS::MutexCS cs(mDataMutex);
    mActivityMinutes = minutesSinceBoot;
}

void SensorPolicy::onWornChanged(bool worn)
{
    OS::MutexCS cs(mDataMutex);
    mWorn = worn;
}

// ============================================================================
// IActivityDataSource
// ============================================================================

uint32_t SensorPolicy::stepsSinceBoot() const
{
    OS::MutexCS cs(mDataMutex);
    return mSteps;
}

void SensorPolicy::floorsSinceBoot(uint32_t& up, uint32_t& down) const
{
    OS::MutexCS cs(mDataMutex);
    up   = mFloorsUp;
    down = mFloorsDown;
}

uint32_t SensorPolicy::activityMinutesSinceBoot() const
{
    OS::MutexCS cs(mDataMutex);
    return mActivityMinutes;
}

float SensorPolicy::currentAvgHr() const
{
    OS::MutexCS cs(mDataMutex);

    // mHr is only refreshed when an HR frame arrives. Once frames stop
    // (watch taken off, HR subsensor stopped) it would keep its last value
    // indefinitely, and DailyHealth books currentAvgHr() into every minute:
    // a watch left on the bedside table recorded hours of a flat HR, which
    // then became the day's resting HR. Report "no reading" as soon as the
    // FSM says the watch is off the wrist, and otherwise once the last
    // valid sample is too old to stand for the present (HR stopped while
    // still worn).
    // Lock order mDataMutex -> FeatureCenter::mMutex, as elsewhere here.
    if (!mWorn) {
        return 0.0f;
    }
    return mFeatures.hasRecentHr() ? mHr : 0.0f;
}

// ============================================================================
// Sensor frame parsing
// ============================================================================
//
// mDataMutex covers both the snapshot writes and the feature-update
// call in a single critical section. FeatureCenter takes its own
// internal mutex when it needs to (FSM queries hit it from the
// Activity::Service task, where mDataMutex is not held). Lock order
// is always mDataMutex -> FeatureCenter::mMutex when both are
// involved, so no cycle is possible.

void SensorPolicy::onSdlNewData(uint16_t                 handle,
                                const SDK::Sensor::Data* base,
                                uint16_t                 count,
                                uint16_t                 stride)
{
    SDK::Sensor::DataBatch batch(base, count, stride);
    const uint32_t now = getTick();

    OS::MutexCS cs(mDataMutex);

    if (mDriverPedo.matchesDriver(handle)) {
        SDK::SensorDataParser::StepCounter p(batch[0]);
        const uint32_t steps = p.getStepCount();
        LOG_DEBUG("Steps: %u\n", steps);

        mSteps = steps;
        mFeatures.onStepCounter(steps, now);
    }
    else if (mDriverFloor.matchesDriver(handle)) {
        SDK::SensorDataParser::FloorCounter p(batch[0]);
        LOG_DEBUG("Floors Up/Down: %d/%d\n",
                  p.getFloorsUp(), p.getFloorsDown());

        // Parser exposes int32 (positive = up, negative = down by
        // convention). Floors are always non-negative in our pipeline
        // (Sensor::Floor counts up via FloorCounter algorithm), so we
        // reinterpret as uint32.
        mFloorsUp   = static_cast<uint32_t>(p.getFloorsUp());
        mFloorsDown = static_cast<uint32_t>(p.getFloorsDown());
    }
    else if (mDriverHR.matchesDriver(handle)) {
        SDK::SensorDataParser::HeartRate p(batch[0]);
        const uint8_t bpm = p.getBpm();
        const uint8_t tl  = p.getTrustLevel();

        mFeatures.onHrSample(bpm, tl, now);
        mHr = mFeatures.currentAvgHr();
        LOG_DEBUG("HR: %u, Trust Level %u, AVG %.1f\n", bpm, tl, mHr);
    }
    else if (mDriverMotion.matchesDriver(handle)) {
        SDK::SensorDataParser::MotionDetect p(batch[0]);
        LOG_DEBUG("Motion detected: %d\n", static_cast<int>(p.getID()));

        const bool detected =
            (p.getID() == SDK::SensorDataParser::MotionDetect::Motion::MOTION) ||
            (p.getID() == SDK::SensorDataParser::MotionDetect::Motion::SIG_MOTION);

        mFeatures.onMotionUpdate(detected, now);
    }
    else if (mDriverTouch.matchesDriver(handle)) {
        SDK::SensorDataParser::Touch p(batch[0]);
        LOG_DEBUG("Touch: %d\n", p.isTouched());

        mFeatures.onTouchUpdate(p.isTouched(), now);
    }
}

// ============================================================================
// ISensorControl
// ============================================================================

Sensor::Connection& SensorPolicy::driverFor(Activity::FsmSensor s)
{
    switch (s) {
        case Activity::FsmSensor::PEDO:     return mDriverPedo;
        case Activity::FsmSensor::FLOOR:    return mDriverFloor;
        case Activity::FsmSensor::HR:       return mDriverHR;
        case Activity::FsmSensor::MOTION:   return mDriverMotion;
        case Activity::FsmSensor::TOUCH:    return mDriverTouch;
        case Activity::FsmSensor::COUNT:
        default:
            break;
    }
    // Unreachable for valid input - defined fallback to avoid UB.
    return mDriverPedo;
}

void SensorPolicy::connect(Activity::FsmSensor s)
{
    driverFor(s).connect();
}

void SensorPolicy::disconnect(Activity::FsmSensor s)
{
    driverFor(s).disconnect();
}

bool SensorPolicy::isConnected(Activity::FsmSensor s)
{
    return driverFor(s).isConnected();
}

uint32_t SensorPolicy::getTick()
{
    return OS::GetTimeMs();
}

} // namespace Activity
