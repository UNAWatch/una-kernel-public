/**
 ******************************************************************************
 * @file    IActivityDataSource.hpp
 * @brief   Read-only view of Activity's monotonic-from-boot counters.
 *
 *          Consumed by a higher layer on its own cadence to derive
 *          per-day deltas. All values are cumulative from system boot
 *          and never reset internally - the daily cut is the caller's
 *          responsibility.
 *
 *          Implementation lives in Activity::SensorPolicy, which
 *          aggregates the raw sensor frames (steps, floors, HR) and
 *          caches the activity-minute total it receives from Service
 *          through the IDurationListener channel. Queries are
 *          therefore cheap reads, not tracker ticks.
 ******************************************************************************
 */

#ifndef COMPONENT_ACTIVITY_IACTIVITY_DATA_SOURCE_HPP
#define COMPONENT_ACTIVITY_IACTIVITY_DATA_SOURCE_HPP

#include <cstdint>

namespace Activity
{

class IActivityDataSource
{
public:
    virtual ~IActivityDataSource() = default;

    /// Cumulative step count from boot. The underlying IMU step
    /// counter is preserved across FSM-driven connect/disconnect
    /// cycles, so this value is strictly non-decreasing within a
    /// single boot.
    virtual uint32_t stepsSinceBoot() const = 0;

    /// Cumulative floor counters from boot.
    virtual void floorsSinceBoot(uint32_t& up, uint32_t& down) const = 0;

    /// Cumulative active minutes from boot, computed by
    /// ActivityTracker (cadence path + HR-evidence path).
    virtual uint32_t activityMinutesSinceBoot() const = 0;

    /// Latest rolling-average heart rate over the last minute. 0.0
    /// ("no reading") if there are no valid samples in the window, if
    /// the watch is not worn, or if the last valid sample is more than
    /// two HR measurement periods (10 min) old.
    virtual float currentAvgHr() const = 0;
};

} // namespace Activity

#endif // COMPONENT_ACTIVITY_IACTIVITY_DATA_SOURCE_HPP
