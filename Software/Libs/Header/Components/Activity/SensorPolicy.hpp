/**
 ******************************************************************************
 * @file    SensorPolicy.hpp
 * @brief   Sensor adapter for the Activity subsystem.
 *
 *          Owns five Sensor::Connection objects (PEDO, FLOOR, HR,
 *          MOTION, TOUCH), parses incoming frames, and forwards
 *          normalised events to FeatureCenter (which owns the per-
 *          feature trackers and exposes IFeatureSource to the FSM).
 *
 *          Exposes two outward contracts to consumers in higher
 *          layers:
 *           - ISensorControl  - sensor (de)activation, used by the
 *                               FSM through IFeatureSource pairing.
 *           - IActivityDataSource - cumulative-from-boot read view of
 *                               the latest sensor snapshot, consumed
 *                               by a higher layer to derive per-day
 *                               deltas on its own cadence.
 *
 *          Also implements IDurationListener: ActivityTracker is
 *          ticked by Service on the FSM cadence; the resulting
 *          minute total is cached here so IActivityDataSource queries
 *          are cheap reads instead of mutating tracker ticks.
 *
 *          Threading model:
 *           - onSdlNewData runs on the SensorLayer task. Updates the
 *             snapshot under mDataMutex, then forwards to
 *             FeatureCenter (which takes its own internal lock).
 *           - onActivityDurationChanged fires on Service's worker.
 *           - IActivityDataSource queries can come from any task
 *             (typically a periodic worker in a higher layer).
 ******************************************************************************
 */

#ifndef COMPONENT_ACTIVITY_SENSOR_POLICY_HPP
#define COMPONENT_ACTIVITY_SENSOR_POLICY_HPP

#include <cstdint>
#include <cstdbool>

#include "OS.hpp"

#include "SensorLayer/Core/SensorConnection.hpp"
#include "SDK/Interfaces/ISensorDataListener.hpp"

#include "Activity/FsmInterfaces.hpp"
#include "Activity/FeatureCenter.hpp"
#include "Activity/IActivityDataSource.hpp"
#include "Activity/IDurationListener.hpp"

namespace Activity
{

class SensorPolicy : public SDK::Interface::ISensorDataListener,
                    public Activity::ISensorControl,
                    public Activity::IDurationListener,
                    public Activity::IActivityDataSource
{
public:
    explicit SensorPolicy(Activity::FeatureCenter& features);

    ~SensorPolicy() override = default;

    // ISensorControl
    void connect(Activity::FsmSensor s) override;
    void disconnect(Activity::FsmSensor s) override;
    bool isConnected(Activity::FsmSensor s) override;

    // IDurationListener - cache the activity-minute value Service
    // produces on its FSM-tick cadence, so IActivityDataSource queries
    // become cheap reads instead of mutating tracker ticks.
    void onActivityDurationChanged(uint32_t minutesSinceBoot) override;
    void onWornChanged(bool worn) override;

    // IActivityDataSource - cumulative-from-boot accessors.
    uint32_t stepsSinceBoot() const override;
    void     floorsSinceBoot(uint32_t& up, uint32_t& down) const override;
    uint32_t activityMinutesSinceBoot() const override;
    float    currentAvgHr() const override;

private:
    void onSdlNewData(uint16_t                 handle,
                      const SDK::Sensor::Data* base,
                      uint16_t                 count,
                      uint16_t                 stride) override;

    Activity::FeatureCenter& mFeatures;

    // Latest cumulative-from-boot snapshot. Fields are written from
    // onSdlNewData on the sensor task and read by the IActivityDataSource
    // accessors from other tasks; mDataMutex protects them all.
    uint32_t mSteps      = 0;
    uint32_t mFloorsUp   = 0;
    uint32_t mFloorsDown = 0;
    float    mHr         = 0.0f;
    /// Latest activity-minute total pushed by Service through
    /// IDurationListener.
    uint32_t mActivityMinutes = 0;
    /// FSM wear state pushed by Service. Not worn until the FSM says so.
    bool     mWorn = false;

    mutable OS::Mutex mDataMutex;

    // Sensor connections owned by SensorPolicy. ~Connection
    // disconnects automatically on destruction.
    Sensor::Connection mDriverPedo;
    Sensor::Connection mDriverFloor;
    Sensor::Connection mDriverHR;
    Sensor::Connection mDriverMotion;
    Sensor::Connection mDriverTouch;

    Sensor::Connection& driverFor(Activity::FsmSensor s);

    static uint32_t getTick();
};

} // namespace Activity

#endif // COMPONENT_ACTIVITY_SENSOR_POLICY_HPP
