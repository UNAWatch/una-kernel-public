/**
 ******************************************************************************
 * @file    FeatureCenter.hpp
 * @brief   Owns per-feature trackers and exposes the IFeatureSource
 *          interface the activity FSM consumes.
 *
 *          SensorPolicy parses raw sensor frames and forwards normalised
 *          events (onStepCounter / onHrSample / onMotionUpdate /
 *          onTouchUpdate) to FeatureCenter on the SensorLayer task.
 *          FSM queries IFeatureSource methods on the Activity::Service
 *          task. A single internal mutex serialises both sides.
 *
 *          Adding a new query is a one-place change: drop a tracker
 *          method (or a new tracker), forward it through one IFeatureSource
 *          accessor, and the FSM can use it. No need to touch
 *          SensorPolicy unless a new sensor stream feeds it.
 ******************************************************************************
 */

#ifndef COMPONENT_ACTIVITY_FEATURE_CENTER_HPP
#define COMPONENT_ACTIVITY_FEATURE_CENTER_HPP

#include <cstdint>

#include "OS.hpp"

#include "Activity/FsmInterfaces.hpp"
#include "Activity/IFsmTrigger.hpp"
#include "Activity/Features/StepCounterTracker.hpp"
#include "Activity/Features/HrTracker.hpp"
#include "Activity/Features/MotionTracker.hpp"
#include "Activity/Features/TouchTracker.hpp"
#include "Activity/Features/ActivityTracker.hpp"

namespace Activity
{

class FeatureCenter : public IFeatureSource
{
public:
    FeatureCenter();
    ~FeatureCenter() override = default;

    /**
     * @brief Attach a target that wants out-of-cadence FSM ticks on
     *        touch / motion edges. Optional - without it the FSM only
     *        runs on its own timer cadence.
     *
     * Set once at startup; the pointer is read lockless on the sensor
     * task and is expected to outlive this FeatureCenter.
     */
    void attachTrigger(IFsmTrigger& trigger);

    // ----- Sink side: called from the SensorLayer task by SensorPolicy
    void onStepCounter(uint32_t total, uint32_t tsMs);
    void resetStepCounter(uint32_t total, uint32_t tsMs);
    void onHrSample(uint8_t bpm, uint8_t tl, uint32_t tsMs);
    void onMotionUpdate(bool detected, uint32_t tsMs);
    void onTouchUpdate(bool touched, uint32_t tsMs);

    // Helper for the metrics-side consumer: keep its snapshot of HR in
    // sync with what the trackers say.
    float currentAvgHr() const;

    // Whether currentAvgHr() still describes the present: a valid HR
    // sample landed within the last two measurement periods.
    bool hasRecentHr() const;

    // Polled by Activity::Service after each FSM tick. Integrates the
    // elapsed active time into the accumulator and returns the running
    // total in minutes.
    uint32_t currentActivityDuration();

    // Reset the activity accumulator to @p baselineMinutes (e.g. on an
    // externally driven day rollover); pairs with @ref resetStepCounter.
    void resetActivityDuration(uint32_t baselineMinutes, uint32_t tsMs);

    // Update the HR-path moderate-intensity cutoff (bpm) from the user's
    // configured HR zones. Safe to call at runtime; forwarded under the
    // feature lock.
    void setActivityHrThresholdBpm(uint8_t bpm);

    // ----- IFeatureSource (called from the Activity::Service task by Fsm)
    uint32_t nowMs()                    const override;
    uint32_t stepsLastMinute()          const override;
    bool     isHrValidLastMinute()      const override;
    bool     isActuallyTouched()        const override;
    bool     isMotion(uint32_t sec)     const override;
    bool     isNoMotion(uint32_t sec)   const override;
    bool     touchRaw()                 const override;
    bool     motionRaw()                const override;

private:
    mutable OS::Mutex   mMutex;
    StepCounterTracker  mSteps;
    HrTracker           mHr;
    MotionTracker       mMotion;
    TouchTracker        mTouch;
    ActivityTracker     mActivity;

    IFsmTrigger* mTrigger = nullptr;
};

} // namespace Activity

#endif // COMPONENT_ACTIVITY_FEATURE_CENTER_HPP
