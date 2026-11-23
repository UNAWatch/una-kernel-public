/**
 ******************************************************************************
 * @file    SensorManager.cpp
 * @brief   SensorManager class
 * 
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "SensorLayer/Core/SensorManager.hpp"
#include "SwTimer.hpp"
#include "System.hpp"

#define LOG_MODULE_PRX      "Sensor.Mgr"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

// Refresh-thread starvation diagnostic. When enabled, the thread emits a
// warning whenever a refresh cycle takes more than 4x its expected period -
// useful to confirm whether sensor FIFOs are backing up because this thread
// was preempted under load. Off by default; flip to 1 to re-enable.
#define SENSOR_MGR_REFRESH_OVERRUN_DIAG 0

#include <algorithm>
#include <cassert>
#include <vector>

////////////////////////////////////////////////////////////////////////////////
//// Diagnostic
////////////////////////////////////////////////////////////////////////////////

#if LOG_MODULE_LEVEL == LOG_LEVEL_DEBUG

struct SensorManagerDiagnostic {
public:
    SensorManagerDiagnostic()
        : mDurations()
        , mTimerRefresh()
        , mTimerUpdate()
    {
        mDurations.reserve(32);
        mTimerRefresh.start();
    }

    void prepare()
    {
        mDurations.clear();
    }

    void preRefresh()
    {
        mTimerUpdate.start();
    }

    void postRefresh()
    {
        mDurations.push_back(mTimerUpdate.passed());
    }

    void finalize()
    {
        LOG_INFO("[%u] - ", mTimerRefresh.passed());
        mTimerRefresh.start();

        for (auto d : mDurations) {
            LOG_INFO_WP("%4u ", d);
        }

        LOG_INFO_WP("\n");
    }

private:
    std::vector<uint32_t> mDurations;
    ::Driver::SwTimer     mTimerRefresh;
    ::Driver::SwTimer     mTimerUpdate;
};

#else

class SensorManagerDiagnostic {
public:
    void prepare()     {}
    void preRefresh()  {}
    void postRefresh() {}
    void finalize()    {}
};

#endif

////////////////////////////////////////////////////////////////////////////////
//// Sensor::Manager
////////////////////////////////////////////////////////////////////////////////
Sensor::Manager& Sensor::Manager::getInstance()
{
    static Sensor::Manager mInstance;
    
    return mInstance;
}

Sensor::Manager::Manager()
    : mDrivers()
    , mSensors()
    , mThread()
    , mMutex()
    , mSemaphore(1, 0)
    , mPeriodTicks(10)
{
    mMutex.init(LOG_MODULE_PRX);

    mThread.init(LOG_MODULE_PRX, this, &Sensor::Manager::thread, osPriorityNormal);
}

bool Sensor::Manager::regDriver(Sensor::Driver* driver)
{
    auto it = std::find_if(mDrivers.begin(), mDrivers.end(),
                           [driver](const Sensor::Driver* d) {
                               return d == driver;
                           });

    if (it != mDrivers.end()) {
        LOG_ERROR("sensor is already registered\n");
        return false; // Already exists
    }

    // The table is sized for every driver this build can register, so a
    // rejection here means one was added without raising skMaxDrivers - a
    // build-time oversight that would otherwise be a silent overrun.
    if (mDrivers.full()) {
        LOG_ERROR("driver table full (%u) - not registered\n",
                  static_cast<unsigned>(mDrivers.max_size()));

        // Not one of the 35 registration sites checks this result, so the
        // false below is swallowed and the only trace is the line above, on
        // a serial log nobody may be reading - leaving a sensor that simply
        // does not exist for the rest of the run. Assert as well, for the
        // same reason updatePeriod() does: no build config defines NDEBUG.
        assert(false);

        return false;
    }

    mDrivers.push_back(driver);

    driver->setHandle(mDrivers.size());

    return true;
}

bool Sensor::Manager::isDriverRegistered(Sensor::Driver* driver) const
{
    auto it = std::find_if(mDrivers.begin(), mDrivers.end(),
                           [driver](const Sensor::Driver* d) {
                               return d == driver;
                           });

    return it != mDrivers.end();
}

bool Sensor::Manager::isDriverRegistered(SDK::Sensor::Type type) const
{
    return std::any_of(mDrivers.begin(), mDrivers.end(),
                       [type](const Sensor::Driver* d) {
                           return d->getType() == type;
                       });
}

void Sensor::Manager::updatePeriod()
{
    // Must never run on the sweep thread itself: it blocks on mMutex, which
    // the sweep holds for the whole refresh loop - a same-thread call is a
    // permanent self-deadlock (the mutex is non-recursive and IWDG is fed
    // from the idle hook). No listener callback may reach this.
    // NOTE: enforced only while assert() is compiled in. No build config
    // defines NDEBUG today; if a Release config ever does, keep this alive
    // (hard guard) - stripping it turns a caught contract violation into a
    // silent, unrecoverable sensor wedge.
    assert(osThreadGetId() != mThread.getId());

    // Blocking on purpose: callers never hold a driver lock (lock order in
    // SensorDriver.hpp), so the wait is bounded by the refresh sweep, and a
    // dropped update would leave the poll cadence stale.
    OS::MutexCS cs(mMutex);

    updatePeriodNoLock();
}

void Sensor::Manager::wake()
{
    mSemaphore.give();
}

void Sensor::Manager::regSensor(Interface::ISensor* sensor)
{
    bool inserted = false;

    // See updatePeriod(): a sweep-thread call would self-deadlock.
    assert(osThreadGetId() != mThread.getId());

    {
        // Blocking on purpose: a registration dropped on timeout leaves the
        // sensor started with listeners attached but never swept - dead until
        // every listener disconnects. Callers hold no driver lock, so the
        // wait is bounded by the refresh sweep.
        OS::MutexCS cs(mMutex);

        auto it = std::find(mSensors.begin(), mSensors.end(), sensor);
        if (it != mSensors.end()) {
            LOG_WARNING("the sensor is already registered\n");
        } else if (mSensors.full()) {
            // Unreachable while skMaxSensors >= skMaxDrivers: a driver
            // registers its sensor once and every driver is already in the
            // other table. Refuse rather than overrun if that ever stops
            // holding - the sensor is then simply never swept.
            LOG_ERROR("sensor table full (%u) - not registered\n",
                      static_cast<unsigned>(mSensors.max_size()));
        } else {
            mSensors.push_back(sensor);
            updatePeriodNoLock();
            inserted = true;
        }
    }

    if (inserted) {
        mSemaphore.give();
    }
}

void Sensor::Manager::unRegSensor(Interface::ISensor* sensor)
{
    // See updatePeriod(): a sweep-thread call would self-deadlock.
    assert(osThreadGetId() != mThread.getId());

    // Blocking on purpose: an unregistration dropped on timeout leaves a
    // stale entry, so every later regSensor is rejected with "already
    // registered" and the sensor delivers nothing until reboot (the field
    // failure this replaces). Blocking here also guarantees the caller's
    // sdcStop cannot race an in-progress refresh sweep. Callers hold no
    // driver lock, so the wait is bounded by the sweep.
    OS::MutexCS cs(mMutex);

    auto it = std::find_if(mSensors.begin(), mSensors.end(),
                           [sensor](const Interface::ISensor* s) {
                               return s == sensor;
                           });

    if (it != mSensors.end()) {
        mSensors.erase(it);
        updatePeriodNoLock();
    }
}

void Sensor::Manager::disconnectListener(SDK::Interface::ISensorDataListener* listener)
{
    if (listener == nullptr) {
        return;
    }

    // Snapshot the driver list under the manager mutex, then release it before
    // disconnecting. Driver::disconnect calls unRegSensor, which waits on this
    // same mutex (see SensorDriver.cpp lock-order note); holding it across the
    // disconnect calls would deadlock.
    // The snapshot is inline storage now, so it costs this thread's stack
    // rather than the heap: skMaxDrivers pointers, taken on the app
    // terminate path.
    DriverList drivers;
    {
        OS::MutexCS cs(mMutex);
        drivers = mDrivers;
    }

    // disconnect() no-ops for any driver this listener never connected to, so
    // disconnecting from all is safe.
    for (auto d : drivers) {
        d->disconnect(listener);
    }
}

Sensor::Driver* Sensor::Manager::getDefaultSensor(SDK::Sensor::Type type) const
{
    for (auto d : mDrivers) {
        if (d->getType() == type) {
            return d;
        }
    }

    return nullptr;
}

uint16_t Sensor::Manager::getDefaultHandle(SDK::Sensor::Type type) const
{
    for (uint16_t idx = 0; idx < mDrivers.size(); ++idx) {
        if (mDrivers[idx]->getType() == type) {
            return mDrivers[idx]->getHandle();
        }
    }

    return 0;
}

Sensor::Manager::HandleList Sensor::Manager::getHandleList(SDK::Sensor::Type type) const
{
    // Cannot overflow: one entry per matching driver, and both hold
    // skMaxDrivers.
    HandleList drivers;

    for (uint16_t idx = 0; idx < mDrivers.size(); ++idx) {
        if (mDrivers[idx]->getType() == type) {
            uint16_t handle = mDrivers[idx]->getHandle();
            drivers.push_back(handle);
        }
    }

    return drivers;
}

Sensor::Driver* Sensor::Manager::getDriverByHandle(uint32_t handle) const
{
    if (handle == 0 || handle > mDrivers.size()) {
        return nullptr;
    }

    return mDrivers[handle - 1];
}

void Sensor::Manager::updatePeriodNoLock()
{
    // Only periodic drivers set the poll cadence. Event-based sensors wake
    // the thread on demand via wake(), so they must not pull the tick down.
    float minPeriod = -1.0f;

    for (auto d : mDrivers) {
        if (d->getMode() != Sensor::Driver::Mode::PERIODIC_BASED || !d->isActive()) {
            continue;
        }

        if (minPeriod < 0.0f || minPeriod > d->getRefreshPeriod()) {
            minPeriod = d->getRefreshPeriod();
        }
    }

    if (minPeriod < 0.0f) {
        mPeriodTicks = sIdlePeriodMs;
        LOG_DEBUG("mPeriodTicks=%u (idle)\n", mPeriodTicks);
        return;
    }

    mPeriodTicks = static_cast<uint32_t>(minPeriod);

    if (mPeriodTicks < sMinPeriodMs) {
        mPeriodTicks = sMinPeriodMs;
    }

    LOG_DEBUG("mPeriodTicks=%u\n", mPeriodTicks);
}

void Sensor::Manager::thread()
{
    SensorManagerDiagnostic diagnostic;

#if SENSOR_MGR_REFRESH_OVERRUN_DIAG
    ::Driver::SwTimer cycleTimer;
    cycleTimer.start();
#endif

    for (;;) {
        bool empty = false;

        {
            if (mMutex.lock(2000)) {
                OS::MutexUnlocker unlocker(mMutex);
                empty = mSensors.empty();
            } else {
                LOG_ERROR("FAILED TO LOCK THE MUTEX\n");
            }
        }

        if (empty) {
            mSemaphore.take();
#if SENSOR_MGR_REFRESH_OVERRUN_DIAG
            cycleTimer.start();
#endif
            continue;
        }

        uint32_t waitMs = mPeriodTicks;

        {
            if (mMutex.lock(2000)) {
                diagnostic.prepare();

                OS::MutexUnlocker unlocker(mMutex);
                LOG_DEBUG("refresh ENTRY\n");
                for (auto s : mSensors) {
                    diagnostic.preRefresh();
                    s->sensorRefresh();
                    diagnostic.postRefresh();
                }

                // Developer-mode injected values, published at the drivers'
                // own pace, as the sources above publish theirs: before the
                // overdue flush below, so it sees them.
                const uint32_t nowMs = ::Driver::System::GetTimeMs();
                for (auto d : mDrivers) {
                    const uint32_t due = d->publishInjected(nowMs);
                    if (due < waitMs) {
                        waitMs = due;
                    }
                }

                // What a listener holds must not wait on a source that has
                // stopped, or that gave its next batch later than it said:
                // it goes by the time it reaches its latency, and the sweep
                // wakes for the next such moment if nothing wakes it sooner.
                for (auto d : mDrivers) {
                    const uint32_t due = d->flushOverdue();
                    if (due < waitMs) {
                        waitMs = due;
                    }
                }

                diagnostic.finalize();

                LOG_DEBUG("refresh EXIT\n");
            } else {
                LOG_ERROR("FAILED TO LOCK THE MUTEX\n");
            }
        }

        // Nothing waits for no time at all: a driver that handed readings
        // over asks for the next pass with 0, and gets it a tick later.
        mSemaphore.take((waitMs != 0) ? waitMs : 1);

#if SENSOR_MGR_REFRESH_OVERRUN_DIAG
        const uint32_t cyclePeriod = cycleTimer.passed();
        cycleTimer.start();
        if (mPeriodTicks > 0 && cyclePeriod > mPeriodTicks * 4) {
            LOG_WARNING("refresh overrun: %u ms (period %u)\n", cyclePeriod, mPeriodTicks);
        }
#endif
    }
}
