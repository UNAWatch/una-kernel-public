/**
 ******************************************************************************
 * @file    SensorDriver.hpp
 * @brief   Sensor Driver class
 *
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "SensorLayer/Core/SensorDriver.hpp"
#include "SensorLayer/Core/SensorManager.hpp"
#include "System.hpp"

#include <algorithm>
#include <new>

#define LOG_MODULE_PRX      "Sensor.Driver"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

etl::ipool& Sensor::Driver::listenerPool()
{
    static etl::pool<ListenerList::pool_type, skListenerPoolSize> pool;

    return pool;
}

namespace {

    /// High-water mark of the shared listener pool. Written under the pool
    /// mutex, read for logging just outside it.
    size_t gListenerPoolPeak = 0;

} // namespace

bool Sensor::Driver::listenerPoolFull()
{
    OS::MutexCS pool(listenerPoolMutex());

    return listenerPool().full();
}

size_t Sensor::Driver::noteListenerPoolPeakLocked()
{
    const size_t used = listenerPool().size();

    if (used <= gListenerPoolPeak) {
        return 0;
    }

    gListenerPoolPeak = used;

    return used;
}

void Sensor::Driver::logListenerPoolPeak(size_t peak)
{
    if (peak == 0) {
        return;
    }

    // This is the number that says whether skListenerPoolSize was derived
    // correctly, so it is worth a line each time it moves rather than only
    // when the pool runs out.
    //
    // Emitted after the pool lock is dropped, so two threads that both raise
    // the mark can print out of order - 13 before 11 and 12 has been seen on
    // a device. The stored mark is monotonic and the largest number printed
    // is the peak; only the sequence is unreliable. Read the maximum, not the
    // trace.
    LOG_INFO("listener pool high-water %u/%u\n",
             static_cast<unsigned>(peak),
             static_cast<unsigned>(skListenerPoolSize));
}

OS::Mutex& Sensor::Driver::listenerPoolMutex()
{
    // Wrapped so init() runs exactly once, as part of constructing the
    // static - OS::Mutex has no constructor that takes a name.
    struct Guarded {
        Guarded() { mutex.init(LOG_MODULE_PRX); }
        OS::Mutex mutex;
    };

    static Guarded guarded;

    return guarded.mutex;
}

/**
 * @brief Construct a new Sensor::Driver.
 *
 * @param sensor        Reference to the physical/virtual sensor implementation.
 * @param t             Sensor type identifier (SDK::Sensor::Type).
 * @param sampleLength  Number of data fields in one sensor sample.
 * @param control       Reference to sensor control callbacks (start/stop/update period/etc.).
 * @param mode          Delivery mode:
 *                      - Mode::PERIOD_BASED: sensor is polled on a periodic timer,
 *                        listeners request sampling period.
 *                      - Mode::EVENT_BASED: sensor produces data on events; period/latency are ignored.
 *
 * @details
 * The driver owns:
 * - A list of DataQueue objects, one per listener.
 * - A shared Sensor::Data buffer (mDataSample), reused for publishing.
 * - The synchronization primitive (mMutex).
 *
 * The driver does NOT own:
 * - The sensor itself (`sensor` reference).
 * - The control interface (`control` reference).
 */
Sensor::Driver::Driver(Interface::ISensor&        sensor,
                       SDK::Sensor::Type          t,
                       uint16_t                   fieldCount,
                       Sensor::ISensorDriverCtrl& control,
                       Mode                       mode)
    : mSensor(sensor)
    , mConnMutex()
    , mMutex()
    , mListeners(listenerPool())
    , mType(t)
    , mControl(control)
    , mDataSample(fieldCount)
    , mNewConnectionListener(nullptr)
    , mMode(mode)
    , mRefreshPeriod(1000)
    , mHandle(0)
    , mAppliedLatency(0)
    , mActive(false)
    , mSourceStepUs(0)
    , mSourceTick(nullptr)
    , mSourceSteps(nullptr)
    , mInjectedValues(nullptr)
    , mInjected(false)
    , mInjectPending(false)
    , mInjectLastMs(0)
{
    mConnMutex.init(LOG_MODULE_PRX);
    mMutex.init(LOG_MODULE_PRX);

    // Force the shared pool and its mutex into existence here, on the single
    // thread that constructs the drivers, rather than leaving it to whichever
    // task happens to connect first. Function-local statics are guarded, but
    // the guard is only atomic when the toolchain thinks threads exist, and
    // this build gives it no reason to: a lost race would hand a caller a
    // mutex whose osMutexNew had not returned, and OS::MutexCS does not check
    // that lock() succeeded. mListeners above already touched the pool; this
    // covers the mutex.
    (void) listenerPoolMutex();
}

/**
 * @brief Get the underlying sensor object.
 *
 * @return Reference to the ISensor implementation registered with this driver.
 */
Interface::ISensor& Sensor::Driver::getSensor()
{
    return mSensor;
}

/**
 * @brief Publish the current contents of mDataSample to listeners.
 *
 * @details
 * This is a convenience wrapper which calls pushData(mDataSample).
 * Expected usage: sensor updates mDataSample (timestamp + values) and then calls pushData().
 */
void Sensor::Driver::pushDataSample()
{
    if (!mMutex.lock(2000)) {
        LOG_ERROR("failed to lock the mutex\n");
        return;
    }

    OS::MutexUnlocker unlocker(mMutex);

    // Injected values stand in for the source's while injection is on.
    if (mInjected.load(std::memory_order_relaxed)) {
        return;
    }

    deliverLocked();
}

void Sensor::Driver::deliverLocked()
{
    if (mNewConnectionListener) {
        mNewConnectionListener->forceData(mDataSample.getData());
    } else if (mMode == Mode::EVENT_BASED) {
        // Event-based data is discrete: every event matters, so it must not
        // pass through the per-listener rate adapter (which would drop a
        // second event arriving within the sample period of the first).
        for (auto& l : mListeners) {
            l.forceData(mDataSample.getData());
        }
    } else {
        for (auto& l : mListeners) {
            l.pushData(mDataSample.getData());
        }
    }
}

/**
 * @brief Connect a new listener.
 *
 * @param listener   Pointer to the consumer that will receive sensor data.
 * @param newPeriod  Requested sampling period in ms (0 = default).
 * @param latency    Requested delivery latency in ms (0 = default = same as period).
 *
 * @return true on success, false on error.
 *
 * @details
 * Serialised against other connect/disconnect calls by mConnMutex. On the
 * first listener the sensor is started and registered with Sensor::Manager;
 * an existing listener is renegotiated (period/latency) instead.
 *
 * A listener that is connected already and asks for another period is handed
 * what its queue holds from inside this call: its onSdlNewData() runs on the
 * calling thread, before connect() returns, with this driver's mMutex held as
 * for any delivery. A listener must not take in onSdlNewData() a lock it
 * holds around connect().
 */
bool Sensor::Driver::connect(SDK::Interface::ISensorDataListener* listener,
                             float                                newPeriod,
                             uint32_t                             latency)
{
    if (mMode == Mode::EVENT_BASED) {
        return connectEventBased(listener);
    }

    // Serialise the connection control plane. Control callbacks and
    // Sensor::Manager calls below run WITHOUT mMutex held (lock order in
    // SensorDriver.hpp): holding mMutex across them deadlocks against the
    // manager's refresh sweep (manager mutex -> mMutex via pushDataSample);
    // both sides then abandon their 2 s timed locks and drop the operation,
    // leaving a stale Manager registration behind.
    OS::MutexCS conn(mConnMutex);

    // Reject negative requested period.
    if (newPeriod < 0.0f) {
        return false;
    }

    // Default period if caller gave 0.
    if (newPeriod == 0) {
        newPeriod = 1000;
    }

    // Enforce sensor's own min period.
    if (newPeriod < mControl.sdcGetMinPeriod(this)) {
        newPeriod = mControl.sdcGetMinPeriod(this);
    }

    // Enforce global lower bound.
    if (newPeriod < sMinPeriod) {
        newPeriod = sMinPeriod;
    }

    // Default latency: same as period.
    if (latency == 0) {
        latency = newPeriod;
    }

    ///////////////////////////////////////////////////////////////////////////////
    //// Check if this listener is already registered.
    //// This means it is reconnecting with new parameters, not a new connection.
    //// The snapshot stays valid after mMutex is released: every mListeners
    //// mutation happens under mConnMutex, which we hold.
    ///////////////////////////////////////////////////////////////////////////////

    bool   exists    = false;
    bool   first     = false;
    bool   connected = true;
    size_t newPeak   = 0;

    {
        // Control-plane mMutex takes are blocking: a timed-out-and-dropped
        // connect/disconnect corrupts subscription state (the field bug this
        // file's lock order exists to prevent). Waiting cannot deadlock -
        // no mMutex holder ever takes mConnMutex or the manager mutex; the
        // only long hold is data delivery, which is a leaf.
        OS::MutexCS cs(mMutex);

        exists = std::find_if(mListeners.begin(), mListeners.end(),
                              [listener](const DataQueue& q) {
                                  return q.getListener() == listener;
                              }) != mListeners.end();

        first = mListeners.empty();
    }

    // Refuse before anything is started. The test at the append below is still
    // the one that decides - the pool can fill between here and there - but
    // without this an exhausted pool costs a full hardware start and stop for
    // every refusal, because the start happens first and has to be wound back.
    //
    // Measured on a 3-node build: 165 GPS chip enable/disable cycles in four
    // minutes, each disable taking up to a second, all of it under mConnMutex.
    // The watch stayed up and refused every connect correctly, and took
    // several seconds to answer a button. Losing a subscription is the
    // documented cost of an exhausted pool; becoming unusable is not.
    if (!exists && listenerPoolFull()) {
        LOG_ERROR("%s: listener pool exhausted (%u) - not connected\n",
                  getDescription(),
                  static_cast<unsigned>(skListenerPoolSize));

        return false;
    }

    if (exists) {
        // Listener already exists - update the driver period (if its
        // listeners now ask for another one) and then its queue.
        newPeriod = renegotiatePeriod(listener, newPeriod, latency);

        if (newPeriod <= 0.0f) {
            return false;
        }

        {
            // Blocking: see the snapshot block above.
            OS::MutexCS cs(mMutex);

            auto queue = std::find_if(mListeners.begin(), mListeners.end(),
                                      [listener](const DataQueue& q) {
                                          return q.getListener() == listener;
                                      });

            if (queue != mListeners.end()) {
                queue->reinit(newPeriod, latency);
            }
        }

        applyLatency(listenersLatency());

        return true;
    }

    ///////////////////////////////////////
    //// New listener case
    ///////////////////////////////////////

    if (first) {
        ///////////////////////////////////////
        //// It is the first listener
        ///////////////////////////////////////

        Sensor::LatencyAggregate wanted;
        wanted.add(latency);
        applyLatency(wanted.value());

        newPeriod = mControl.sdcStart(this, newPeriod);
        if (newPeriod <= 0) {
            LOG_ERROR("failed to start the driver\n");
            applyLatency(0);
            return false;
        }

        mRefreshPeriod = calcUpdatePeriod(newPeriod, latency);

        mActive.store(true, std::memory_order_relaxed);
        Sensor::Manager::getInstance().regSensor(&mSensor);
    } else {
        ///////////////////////////////////////
        //// Sensor is already active: maybe
        //// tighten the global period.
        ///////////////////////////////////////

        newPeriod = updatePeriod(newPeriod, latency);

        if (newPeriod <= 0.0f) {
            return false;
        }
    }

    // Actually append a new DataQueue for this listener. A sample pushed
    // between sdcStart()/regSensor() above and this append sees no listeners
    // and is dropped; the next one arrives a period later.
    {
        // Blocking: a failure here after sdcStart()/regSensor() succeeded
        // would leak a started, registered sensor with no listener.
        OS::MutexCS cs(mMutex);

        // Test and take together under the pool lock: the capacity belongs to
        // every driver, so answering full() for this one and acting on it
        // afterwards would let another driver take the last node in between.
        OS::MutexCS pool(listenerPoolMutex());

        if (mListeners.full()) {
            connected = false;
        } else {
            mListeners.emplace_back(*this,
                                    mDataSample.getFieldCount(),
                                    listener,
                                    newPeriod,
                                    latency);

            // In the same critical section as the take: reading the level
            // after the lock is dropped lets a disconnect give a node back
            // first, which loses the peak this call created.
            newPeak = noteListenerPoolPeakLocked();
        }
    }

    if (!connected) {
        // Logged out here, not above: getDescription() goes through mControl,
        // and mMutex is a leaf that must not be held across a control
        // callback (see the lock-order note in SensorDriver.hpp).
        //
        // Refusing rather than carrying on is deliberate. A silent no-op
        // would leave the caller believing it is subscribed to a sensor that
        // will never deliver.
        LOG_ERROR("%s: listener pool exhausted (%u) - not connected\n",
                  getDescription(),
                  static_cast<unsigned>(skListenerPoolSize));

        // Undo what this call set up. For the first listener that is the
        // start itself; for any later one it is the period, which was
        // tightened for a listener that is not going to arrive and would
        // otherwise pace both this driver and the refresh sweep until the
        // next connect or disconnect happened to recompute it.
        if (first) {
            stopAfterFailedFirstListener();
        } else {
            restorePeriodAfterFailedListener();
        }

        return false;
    }

    logListenerPoolPeak(newPeak);

    if (!first) {
        applyLatency(listenersLatency());
    }

    return true;
}

/**
 * @brief Connect a listener to an event-based driver.
 *
 * @details
 * Event-based delivery has no sampling period: each event is published as it
 * arrives and the per-listener queue holds a single slot. There is nothing to
 * negotiate, so this path skips the period machinery entirely.
 */
bool Sensor::Driver::connectEventBased(SDK::Interface::ISensorDataListener* listener)
{
    // Serialise the control plane; see connect() and the lock-order note in
    // SensorDriver.hpp. sdcStart and regSensor must not run under mMutex.
    OS::MutexCS conn(mConnMutex);

    bool first = false;

    {
        // Blocking: see connect() - dropped control-plane operations corrupt
        // subscription state.
        OS::MutexCS cs(mMutex);

        // Already connected: nothing to renegotiate for event-based delivery.
        auto queue = std::find_if(mListeners.begin(), mListeners.end(),
                                  [listener](const DataQueue& q) {
                                      return q.getListener() == listener;
                                  });
        if (queue != mListeners.end()) {
            return true;
        }

        first = mListeners.empty();
    }

    // See connect(): refuse before starting the source, not after.
    if (listenerPoolFull()) {
        LOG_ERROR("%s: listener pool exhausted (%u) - not connected\n",
                  getDescription(),
                  static_cast<unsigned>(skListenerPoolSize));

        return false;
    }

    // Stable under mConnMutex: mutations require it.
    if (first) {
        mControl.sdcStart(this, 0.0f);
        mRefreshPeriod = 0.0f;
        mActive.store(true, std::memory_order_relaxed);
        Sensor::Manager::getInstance().regSensor(&mSensor);
    }

    // An event fired between sdcStart() above and the append below sees no
    // listeners and is dropped.
    Sensor::DataQueue* newConnectionListener = nullptr;
    size_t             newPeak               = 0;

    {
        // Blocking: a failure here after sdcStart()/regSensor() succeeded
        // would leak a started, registered sensor with no listener.
        OS::MutexCS cs(mMutex);

        // See connect(): the test and the take are one operation on a pool
        // every driver draws from.
        OS::MutexCS pool(listenerPoolMutex());

        if (!mListeners.full()) {
            // period and latency are not applicable; the queue holds one slot.
            newConnectionListener = &mListeners.emplace_back(*this,
                                                             mDataSample.getFieldCount(),
                                                             listener,
                                                             0.0f,
                                                             0);

            // See connect(): measured in the same critical section as the take.
            newPeak = noteListenerPoolPeakLocked();
        }
    } // <- both mutexes released here

    if (newConnectionListener == nullptr) {
        // See connect(): refuse loudly, and undo the start this call made.
        LOG_ERROR("%s: listener pool exhausted (%u) - not connected\n",
                  getDescription(),
                  static_cast<unsigned>(skListenerPoolSize));

        if (first) {
            stopAfterFailedFirstListener();
        }

        return false;
    }

    logListenerPoolPeak(newPeak);

    // Hand the new listener the current state right away.
    mNewConnectionListener = newConnectionListener;
    mControl.sdcNewConnection(this);
    mNewConnectionListener = nullptr;

    return true;
}

/**
 * @brief Disconnect a listener from this driver.
 *
 * @param listener Pointer to the listener that should stop receiving data.
 *
 * @details
 * Serialised against other connect/disconnect calls by mConnMutex. Removes
 * the listener's DataQueue; on the last listener the sensor is unregistered
 * from Sensor::Manager and then stopped, otherwise the sampling period is
 * relaxed if the departing listener was the fastest one.
 */
void Sensor::Driver::disconnect(SDK::Interface::ISensorDataListener* listener)
{
    // Serialise the control plane; see connect() and the lock-order note in
    // SensorDriver.hpp. unRegSensor, sdcStop and applyPeriod must not run
    // under mMutex: the manager's refresh sweep holds the manager mutex and
    // takes mMutex via pushDataSample, so holding mMutex while waiting for
    // the manager mutex deadlocks. Both sides then abandon their 2 s timed
    // locks - the dropped unRegSensor leaves this sensor registered while
    // its listeners and hardware are torn down, and every later reconnect
    // is rejected with "the sensor is already registered".
    OS::MutexCS conn(mConnMutex);

    bool  last         = false;
    float oldMinPeriod = 0.0f;

    {
        // Blocking: returning without erasing would leave the listener
        // registered while the caller (typically a destructor) frees it -
        // the next sweep would then deliver into freed memory.
        OS::MutexCS cs(mMutex);

        auto it = std::find_if(mListeners.begin(), mListeners.end(),
                               [listener](const DataQueue& q) {
                                   return q.getListener() == listener;
                               });

        if (it == mListeners.end()) {
            // Listener not found - nothing to do.
            return;
        }

        // Remember the period currently associated with this queue.
        oldMinPeriod = it->getPeriod();

        {
            // Returning a node to the shared pool, so the same lock as the
            // take. Scoped tight: nothing below it touches the pool.
            OS::MutexCS pool(listenerPoolMutex());

            mListeners.erase(it);
        }

        last = mListeners.empty();
    }

    // Last listener gone? Unregister first: unRegSensor returns only once any
    // in-progress refresh sweep has finished (it waits on the manager mutex),
    // so the sensor cannot be refreshed after sdcStop.
    if (last) {
        Sensor::Manager::getInstance().unRegSensor(&mSensor);
        mControl.sdcStop(this);

        if (mMode == Mode::PERIODIC_BASED) {
            applyLatency(0);
        }

        // A driver nobody listens to must not pace the sweep: the period it
        // was last refreshed at would hold the sweep to it for good.
        mActive.store(false, std::memory_order_relaxed);
        Sensor::Manager::getInstance().updatePeriod();

        return;
    }

    // There are still listeners.
    // Check if we can increase the sampling period.
    // Safe without mMutex: list mutations and reinit require mConnMutex.
    Sensor::DataQueue* queue = findListenerWithMinPeriod();
    float newMinPeriod = queue->getPeriod();
    if (oldMinPeriod < newMinPeriod) {
        applyPeriod(newMinPeriod, queue->getLatency());
    }

    if (mMode == Mode::PERIODIC_BASED) {
        applyLatency(listenersLatency());
    }
}

/**
 * @brief The tightest latency among the listeners connected now.
 *
 * @return Latency in ms, 0 when there is no listener.
 *
 * @details
 * Reads mListeners without mMutex, which is safe under mConnMutex: every
 * change to the list and to a queue's latency is made with mConnMutex held.
 */
uint32_t Sensor::Driver::listenersLatency() const
{
    Sensor::LatencyAggregate wanted;

    for (const auto& l : mListeners) {
        wanted.add(l.getLatency());
    }

    return wanted.value();
}

/**
 * @brief Tell the source the latency it now owes, if that changed.
 *
 * @param latency Tightest latency in ms, 0 when no listener is left.
 *
 * @details
 * A control callback: called under mConnMutex and never under mMutex.
 */
void Sensor::Driver::applyLatency(uint32_t latency)
{
    if (latency == mAppliedLatency) {
        return;
    }

    mAppliedLatency = latency;

    mControl.sdcUpdateLatency(this, latency);
}

void Sensor::Driver::endOfBatch(uint32_t nextBatchMs)
{
    if (mMode != Mode::PERIODIC_BASED) {
        return;
    }

    if (!mMutex.lock(2000)) {
        LOG_ERROR("failed to lock the mutex\n");
        return;
    }

    OS::MutexUnlocker unlocker(mMutex);

    const uint32_t now = ::Driver::System::GetTimeMs();

    for (auto& l : mListeners) {
        l.flushIfDue(now, nextBatchMs);
    }
}

uint32_t Sensor::Driver::flushOverdue()
{
    uint32_t dueIn = Sensor::DataQueue::kNothingHeld;

    if (mMode != Mode::PERIODIC_BASED || !mActive.load(std::memory_order_relaxed)) {
        return dueIn;
    }

    if (!mMutex.lock(2000)) {
        LOG_ERROR("failed to lock the mutex\n");
        return dueIn;
    }

    OS::MutexUnlocker unlocker(mMutex);

    const uint32_t now = ::Driver::System::GetTimeMs();

    // A source sampled on the sweep is due at its own deadlines, which no
    // listener's latency knows of.
    if (mSourceTick != nullptr) {
        dueIn = mSourceTick->dueInMs(now);
    }

    bool handedOver = false;

    for (auto& l : mListeners) {
        if (l.flushIfDue(now, sSweepLeadMs)) {
            handedOver = true;
        }

        const uint32_t due = l.dueInMs(now);
        if (due < dueIn) {
            dueIn = due;
        }
    }

    // A listener that passes readings on does so on its own turn in the
    // sweep, and in this sweep that turn has gone by.
    return handedOver ? 0 : dueIn;
}

bool Sensor::Driver::isActive() const
{
    return mActive.load(std::memory_order_relaxed);
}

uint16_t Sensor::Driver::getFieldCount() const
{
    return static_cast<uint16_t>(mDataSample.getFieldCount());
}

bool Sensor::Driver::inject(const SDK::Sensor::Data::Field* values, uint16_t count)
{
    if (values == nullptr || count != getFieldCount()) {
        return false;
    }

    if (!mMutex.lock(2000)) {
        LOG_ERROR("failed to lock the mutex\n");
        return false;
    }
    OS::MutexUnlocker unlocker(mMutex);

    if (mInjectedValues == nullptr) {
        mInjectedValues = new (std::nothrow) SDK::Sensor::Data::Field[count];
        if (mInjectedValues == nullptr) {
            return false;
        }
    }

    for (uint16_t i = 0; i < count; ++i) {
        mInjectedValues[i] = values[i];
    }
    mInjectPending = true;
    mInjected.store(true, std::memory_order_relaxed);
    Sensor::Manager::getInstance().wake();
    return true;
}

void Sensor::Driver::stopInjection()
{
    mInjected.store(false, std::memory_order_relaxed);
}

bool Sensor::Driver::isInjected() const
{
    return mInjected.load(std::memory_order_relaxed);
}

uint32_t Sensor::Driver::publishInjected(uint32_t nowMs)
{
    if (!mInjected.load(std::memory_order_relaxed) || !isActive()) {
        return UINT32_MAX;
    }

    if (!mMutex.lock(2000)) {
        LOG_ERROR("failed to lock the mutex\n");
        return UINT32_MAX;
    }
    OS::MutexUnlocker unlocker(mMutex);

    // An event-based driver publishes each injected value once; a periodic
    // one repeats the latest at its period, as its source would.
    const bool periodic = (mMode == Mode::PERIODIC_BASED);
    uint32_t   period   = static_cast<uint32_t>(mRefreshPeriod);
    if (period == 0) {
        period = 1000;
    }

    const bool due = mInjectPending || (periodic && (nowMs - mInjectLastMs >= period));
    if (due && mInjectedValues != nullptr) {
        const uint16_t count = getFieldCount();
        SDK::Sensor::Data& data = mDataSample.getData();
        for (uint16_t i = 0; i < count; ++i) {
            data.mValue[i] = mInjectedValues[i];
        }
        mDataSample.setTimestamp(nowMs);
        deliverLocked();
        mInjectPending = false;
        mInjectLastMs  = nowMs;
    }

    if (!periodic) {
        return UINT32_MAX;
    }
    const uint32_t elapsed = nowMs - mInjectLastMs;
    return (elapsed >= period) ? 0 : (period - elapsed);
}

void Sensor::Driver::setSourceStepUs(uint32_t us)
{
    mSourceStepUs.store(us, std::memory_order_relaxed);
}

uint32_t Sensor::Driver::getSourceStepUs() const
{
    return mSourceStepUs.load(std::memory_order_relaxed);
}

void Sensor::Driver::setSourceStepTrack(const SourceStepTrack* track)
{
    mSourceSteps = track;
}

uint32_t Sensor::Driver::getSourceStepUsAt(uint64_t tsUs) const
{
    return (mSourceSteps != nullptr) ? mSourceSteps->stepAt(tsUs) : getSourceStepUs();
}

void Sensor::Driver::setSourceTick(SourceTick* tick)
{
    mSourceTick = tick;
}

bool Sensor::Driver::tickDue()
{
    return (mSourceTick != nullptr)
        && mSourceTick->check(::Driver::System::GetTimeMs(), sSweepLeadMs);
}

float Sensor::Driver::calcUpdatePeriod(float period, uint32_t latency) const
{
    // The whole periods in the latency, not the capacity of the queue: that
    // is rounded up, and a sweep paced by it would come round after the
    // latency has passed.
    uint32_t periods       = Sensor::DataQueue::wholePeriods(period, latency);
    float    refreshPeriod = periods * period;

    LOG_DEBUG("P=%u N=%u\n", static_cast<uint32_t>(period), periods);

    return refreshPeriod;
}

/**
 * @brief Get the SDK sensor type associated with this driver.
 *
 * @return Sensor type enum value.
 */
SDK::Sensor::Type Sensor::Driver::getType() const
{
    return mType;
}

Sensor::Driver::Mode Sensor::Driver::getMode() const
{
    return mMode;
}

float Sensor::Driver::getRefreshPeriod() const
{
    return mRefreshPeriod;
}

/**
 * @brief Get the number of registered listeners.
 *
 * @return Current size of mListeners as uint32_t.
 */
uint32_t Sensor::Driver::getCountOfListeners()
{
    if (!mMutex.lock(2000)) {
        LOG_ERROR("failed to lock the mutex\n");
        return 0;
    }

    OS::MutexUnlocker unlocker(mMutex);

    return static_cast<uint32_t>(mListeners.size());
}

/**
 * @brief Access the shared Sensor::Data buffer used for publishing.
 *
 * @return Reference to the internal mDataSample buffer.
 *
 * @details
 * Typical usage pattern:
 * - Sensor writes into getData(): timestamp + value fields.
 * - Sensor then calls pushData() to broadcast it.
 */
Sensor::DataSample& Sensor::Driver::getDataSample()
{
    return mDataSample;
}

/**
 * @brief Get the minimum allowed period for this sensor.
 *
 * @return Minimum period in ms, as reported by the control interface.
 *
 * @details
 * This delegates to ISensorDriverCtrl::sdcGetMinPeriod().
 * It represents the fastest sampling the sensor is allowed to run at.
 */
float Sensor::Driver::getMinPeriod()
{
    return mControl.sdcGetMinPeriod(this);
}

void Sensor::Driver::setHandle(uint8_t value)
{
    mHandle = value;
}

uint8_t Sensor::Driver::getHandle()
{
    return mHandle;
}

const char* Sensor::Driver::getDescription()
{
    return mControl.sdcGetDescription(this);
}

/**
 * @brief Put the period back after a listener could not be stored.
 *
 * @details
 * Mirror of the non-last path of disconnect(): the surviving listeners decide
 * the period, and the fastest of them is what the driver runs at.
 *
 * Applied unconditionally rather than only when something looks changed.
 * There is no cheap way to ask whether the refused listener tightened the
 * sensor: mRefreshPeriod is the whole periods in the latency times the
 * period, so it comes out at roughly the latency whatever the
 * period does - a listener that halves the sampling rate can leave it
 * identical. Guarding on it would skip the restore in exactly the case that
 * needs it, and the cost of not guarding is one hardware round trip on a
 * path only reached when the pool is exhausted.
 *
 * Reads mListeners without mMutex, which is safe under mConnMutex: every
 * mutation requires it, and this runs holding it.
 */
void Sensor::Driver::restorePeriodAfterFailedListener()
{
    Sensor::DataQueue* queue = findListenerWithMinPeriod();

    applyPeriod(queue->getPeriod(), queue->getLatency());

    if (mMode == Mode::PERIODIC_BASED) {
        applyLatency(listenersLatency());
    }
}

/**
 * @brief Wind back the start that a failed first connect() performed.
 *
 * @details
 * Mirror of the last-listener path in disconnect(), which is the only other
 * place a started driver is wound back - keep the two together if either
 * changes. Unregister before stopping: unRegSensor returns only once an
 * in-progress refresh sweep has finished, so the sensor cannot be refreshed
 * after sdcStop. Must be called under mConnMutex and never under mMutex.
 */
void Sensor::Driver::stopAfterFailedFirstListener()
{
    Sensor::Manager::getInstance().unRegSensor(&mSensor);
    mControl.sdcStop(this);

    if (mMode == Mode::PERIODIC_BASED) {
        applyLatency(0);
    }

    // A driver nobody listens to must not pace the sweep: the period it was
    // started at would hold the sweep to it for good.
    mActive.store(false, std::memory_order_relaxed);
    Sensor::Manager::getInstance().updatePeriod();
}

/**
 * @brief Find the listener with the smallest sampling period request.
 *
 * @return Pointer to the DataQueue with the minimal period, or nullptr if no listeners.
 *
 * @details
 * The "minimum period" is effectively the highest-priority / fastest
 * refresh request. The driver tries to honor the fastest requested rate
 * among all listeners.
 */
Sensor::DataQueue* Sensor::Driver::findListenerWithMinPeriod()
{
    if (mListeners.empty()) {
        assert(false);
        return nullptr;
    }

    auto minIt = mListeners.begin();
    for (auto it = std::next(mListeners.begin()); it != mListeners.end(); ++it) {
        if (it->getPeriod() < minIt->getPeriod()) {
            minIt = it;
        }
    }

    return &(*minIt);
}

float Sensor::Driver::applyPeriod(float requestedPeriod, uint32_t latency)
{
    const float adjustedPeriod = mControl.sdcUpdatePeriod(this, requestedPeriod);
    if (adjustedPeriod <= 0.0f) {
        LOG_ERROR("failed to update the driver period\n");
        return 0.0f;
    }

    const float refreshPeriod = calcUpdatePeriod(adjustedPeriod, latency);

    setPeriod(refreshPeriod);

    return adjustedPeriod;
}

/**
 * @brief Update the driver's global sampling period if a better value is requested.
 *
 * @param requestedPeriod Desired new sampling period in milliseconds.
 * @param latency         Maximum allowed latency in milliseconds used to derive
 *                        the internal refresh period.
 *
 * @return The effective period accepted by the driver (in milliseconds),
 *         or 0.0f if the update failed.
 *
 * @details
 * The method performs the following steps:
 * - Finds the listener with the minimal currently configured period.
 * - If the requested period is not shorter than this minimal listener period,
 *   no changes are applied and the requested value is returned.
 * - Otherwise, calls mControl.sdcUpdatePeriod() to let the sensor control
 *   logic validate and possibly adjust the requested period.
 * - If the control logic rejects the request (returns <= 0.0f), an error is
 *   logged and 0.0f is returned without modifying the current period.
 * - If the adjusted period matches the minimal listener period, the current
 *   configuration is already optimal and the adjusted value is returned.
 * - Otherwise, calcUpdatePeriod() is used with the adjusted period and
 *   the given latency to obtain the internal refresh period, which is then
 *   applied via setPeriod().
 */
float Sensor::Driver::updatePeriod(float requestedPeriod, uint32_t latency)
{
    Sensor::DataQueue* minListener = findListenerWithMinPeriod();

    const float minListenerPeriod = minListener->getPeriod();

    if (requestedPeriod >= minListenerPeriod) {
        return requestedPeriod;
    }

    return applyPeriod(requestedPeriod, latency);
}

/**
 * @brief Update the driver's sampling period for a listener that changes the
 *        period of the subscription it holds.
 *
 * @param listener        The listener asking; it is in mListeners already.
 * @param requestedPeriod Period it now asks for, in milliseconds.
 * @param latency         Latency it now asks for, in milliseconds.
 *
 * @return The period for the listener's queue (in milliseconds), or 0.0f if
 *         the source refused the change.
 *
 * @details
 * The source runs at the shortest period among its listeners, so the period
 * is taken over all of them with this one's request in place of what its
 * queue still holds, and applied when that differs from the shortest one now -
 * whichever way it moved. A listener that was the fastest and asks for a
 * longer period slows the source to whoever is the fastest after it, as
 * disconnect() does when the fastest one leaves.
 *
 * The queue is left for the caller to change afterwards, so a refusal leaves
 * the listener as it was.
 *
 * Reads mListeners without mMutex, which is safe under mConnMutex: every
 * mutation requires it, and this runs holding it.
 */
float Sensor::Driver::renegotiatePeriod(const SDK::Interface::ISensorDataListener* listener,
                                        float                                      requestedPeriod,
                                        uint32_t                                   latency)
{
    float    oldMinPeriod  = requestedPeriod;
    float    newMinPeriod  = requestedPeriod;
    uint32_t newMinLatency = latency;
    bool     own           = true;      // The new shortest period is this listener's
    bool     first         = true;

    for (const auto& l : mListeners) {
        if (first || l.getPeriod() < oldMinPeriod) {
            oldMinPeriod = l.getPeriod();
            first        = false;
        }

        if (l.getListener() == listener) {
            continue;
        }

        if (l.getPeriod() < newMinPeriod) {
            newMinPeriod  = l.getPeriod();
            newMinLatency = l.getLatency();
            own           = false;
        }
    }

    if (newMinPeriod == oldMinPeriod) {
        return requestedPeriod;
    }

    const float appliedPeriod = applyPeriod(newMinPeriod, newMinLatency);
    if (appliedPeriod <= 0.0f) {
        return 0.0f;
    }

    // What the source made of the request goes to the queue of the listener
    // that made it; a period applied for another listener is not this one's.
    return own ? appliedPeriod : requestedPeriod;
}

/**
 * @brief Store the new current sampling period and notify Sensor::Manager.
 *
 * @param period New active period in ms for this driver/sensor.
 *
 * @details
 * After updating mPeriod, we call Sensor::Manager::updateRefreshPeriod()
 * so that the central manager can recalculate global scheduling
 * (for example, next wake-up deadlines).
 */
void Sensor::Driver::setPeriod(float period)
{
    mRefreshPeriod = period;

    Sensor::Manager::getInstance().updatePeriod();
}
