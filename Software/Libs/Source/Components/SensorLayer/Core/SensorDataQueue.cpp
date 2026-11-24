/**
 ******************************************************************************
 * @file    SensorDataQueue.cpp
 * @brief   Queue for SensorData
 *
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "SensorLayer/Core/SensorDataQueue.hpp"
#include "SensorLayer/Core/SensorDriver.hpp"

#include "SDK/SensorLayer/SensorDataView.hpp"

#include <assert.h>
#include <new>   // std::nothrow

#define LOG_MODULE_PRX      "SensorDataQueue"
#define LOG_MODULE_LEVEL    LOG_LEVEL_DEBUG
#include "Logger.h"

Sensor::DataQueue::DataQueue(Sensor::Driver&                      driver,
                             uint16_t                             fieldCount,
                             SDK::Interface::ISensorDataListener* listener,
                             float                                period,
                             uint32_t                             latency)
    : mDriver(driver)
    , mListener(listener)
    , mFieldCount(fieldCount)
    , mStride(calcStride(fieldCount))
    , mQueue(nullptr)
    , mIndex(0)
    , mSize(0)
    , mCapacity(0)
    , mPeriod(period)
    , mLatency(latency)
    , mSRA(Sensor::SampleRateAdapter::periodUs(period))
{
    // Event-based listeners take a single-slot queue: every event is delivered
    // immediately. Periodic listeners size the queue from period / latency.
    const uint16_t capacity = (mDriver.getMode() == Sensor::Driver::Mode::EVENT_BASED)
                            ? 1
                            : computeCapacity(mPeriod, latency);
    if (!initQueue(capacity)) {
        // The queue is left empty and rejects samples rather than writing
        // through a null buffer. Loud because the listener is still added to
        // the driver by the caller and will simply never receive anything --
        // dropping it there instead is a Driver-level policy decision.
        LOG_ERROR("no memory for a %u-sample queue; this listener will get nothing\n",
                (unsigned)capacity);
    }
}

/**
 * @brief Destroy the DataQueue, releasing the slots it holds.
 */
Sensor::DataQueue::~DataQueue()
{
    ::operator delete(mQueue);
}

uint16_t Sensor::DataQueue::computeCapacity(float period, uint32_t latency)
{
    // A non-positive period means no batching window: a single-slot queue
    // that delivers every sample immediately.
    if (period <= 0.0f) {
        return 1;
    }

    // The period as the float holds it, not rounded to a microsecond: over
    // hundreds of periods the rounding alone comes to most of one.
    const double periodUs  = static_cast<double>(period) * 1000.0;
    const double latencyUs = static_cast<double>(latency) * 1000.0;
    const double periods   = latencyUs / periodUs;

    if (periods >= static_cast<double>(kMaxCapacity)) {
        return kMaxCapacity;
    }

    // Rounded up: the readings that lie within the latency. From the first
    // of them to the last is less than the latency, so a queue of them fills
    // no later than its oldest reading is due but for the time the source
    // takes to hand a reading on, and that is the sweep's to keep. It also
    // holds what a source read once a latency brings in a batch, when the
    // period is a whole number of the source's steps. Rounded down, such a
    // batch fills the queue and leaves a reading over, which cannot wait for
    // the next batch and goes as a delivery of its own.
    //
    // What is left over counts as a reading past half a microsecond: less
    // is what float arithmetic leaves of a period that is a whole part of
    // the latency, 19.99999 ms or a thirtieth of a second in 100 ms.
    uint16_t     capacity = static_cast<uint16_t>(periods);
    const double leftUs   = latencyUs - capacity * periodUs;

    if (leftUs > 0.5) {
        ++capacity;
    }

    if (capacity == 0) {
        return 1;
    }

    if (capacity > kMaxCapacity) {
        return kMaxCapacity;
    }

    return capacity;
}

uint16_t Sensor::DataQueue::wholePeriods(float period, uint32_t latency)
{
    if (period <= 0.0f) {
        return 1;
    }

    const float periods = static_cast<float>(latency) / period;

    if (periods < 1.0f) {
        return 1;
    }

    if (periods > static_cast<float>(kMaxCapacity)) {
        return kMaxCapacity;
    }

    return static_cast<uint16_t>(periods);
}

bool Sensor::DataQueue::initQueue(uint16_t capacity)
{
    const uint16_t size = static_cast<uint16_t>(capacity * mStride);

    // Allocate BEFORE publishing any of it. Publishing first and returning on
    // failure would leave the object claiming a capacity it does not have,
    // over a buffer it no longer owns -- which is what reinit() then migrates
    // into and frees.
    //
    // Not the aligned operator: it asked for 4-byte alignment, weaker than the
    // 8 bytes malloc already guarantees here, so it only cost the nothrow form.
    void* mem = ::operator new(size, std::nothrow);

    if (mem == nullptr) {
        return false;
    }

    std::memset(mem, 0, size);

    mCapacity = capacity;
    mSize     = size;
    mQueue    = reinterpret_cast<SDK::Sensor::Data*>(mem);
    mIndex    = 0;

    return true;
}

void Sensor::DataQueue::reinit(float period, uint32_t latency)
{
    // No changes? Exit.
    if (mPeriod == period && mLatency == latency) {
        return;
    }

    // A new latency alone leaves the stream as it is: which samples are kept
    // is the period's business, and a count started over here would hand
    // one on out of turn.
    if (mPeriod != period) {
        // What is held was thinned to the period the listener had. It goes
        // now, a delivery of its own and ahead of its time, so that none of
        // it is handed over among readings thinned to the new one: a
        // listener that passes them on has one step to name for a delivery.
        // The listener is called from inside its own connect(), on the
        // thread that asked, with the driver's lock held as for any delivery.
        deliver();

        mSRA.setExpectedPeriod(Sensor::SampleRateAdapter::periodUs(period));
        mSRA.reset();
    }

    mPeriod  = period;
    mLatency = latency;

    const uint16_t newCapacity = computeCapacity(mPeriod, mLatency);

    // No changes? Exit.
    if (newCapacity == mCapacity) {
        return;
    }

    const uint8_t* oldQueue = (uint8_t*)mQueue;
    const uint16_t oldCount = mIndex;

    // Create a new queue
    if (!initQueue(newCapacity)) {
        // Keep the one we have. It stays at its old capacity, so it delivers
        // more often than the new latency asks for -- a degradation, not a
        // fault. Going on would migrate into an allocation that does not
        // exist and then free the buffer mQueue still points at.
        LOG_WARNING("no memory to resize the queue to %u; keeping %u\n",
                (unsigned)newCapacity, (unsigned)mCapacity);
        return;
    }

    // Carry over the readings gathered since the last delivery, and nothing
    // else: the slots past them hold zeros or readings delivered already.
    for (uint16_t idx = 0; idx < oldCount; ++idx) {
        SDK::Sensor::Data* item = (SDK::Sensor::Data*) &oldQueue[idx * mStride];
        forceData(*item);
    }

    ::operator delete((void*)oldQueue);
}

/**
 * @brief Push one new Sensor::Data sample into this queue (rate-limited).
 *
 * @param src Incoming sample from the sensor/driver.
 *
 * @details
 * This does NOT always emit to the listener: mSRA decimates the stream
 * against the source's nominal step, as the driver reports it now, and only
 * the samples it passes go on to forceData().
 */
void Sensor::DataQueue::pushData(const SDK::Sensor::Data& d)
{
    pushData(d, mDriver.getSourceStepUs());
}

void Sensor::DataQueue::pushData(const SDK::Sensor::Data& d, uint32_t stepUs)
{
    // Go through DataView rather than combining the two uint32_t fields here.
    // Written out, `mTimeStamp * 1000` is 32-bit arithmetic that wraps at
    // ~71.6 min of uptime; keeping the one widening conversion in DataView
    // means there is a single place for that to be got wrong. The view is
    // constructed and discarded, which optimises away - only mData is read.
    const uint64_t timestampUs = SDK::Sensor::DataView(d, mFieldCount).getTimestampUs();

    if (mSRA.shouldEmit(timestampUs, stepUs)) {
        forceData(d);
    }
}

void Sensor::DataQueue::forceData(const SDK::Sensor::Data& d)
{
    if (mQueue == nullptr) {
        return;   // construction could not allocate; nothing to buffer into
    }

    std::uint8_t* base = reinterpret_cast<std::uint8_t*>(mQueue);
    std::uint8_t* dst  = base + static_cast<std::size_t>(mIndex) * mStride;
    std::memcpy(dst, &d, mStride);

    if (++mIndex >= mCapacity) {
        deliver();
    }
}

bool Sensor::DataQueue::flushIfDue(uint32_t nowMs, uint32_t nextChanceMs)
{
    if (mIndex == 0) {
        return false;
    }

    const uint64_t ageMs = oldestAgeMs(nowMs);

    if (ageMs + nextChanceMs < mLatency) {
        return false;
    }

    deliver();

    return true;
}

uint32_t Sensor::DataQueue::dueInMs(uint32_t nowMs)
{
    if (mIndex == 0) {
        return kNothingHeld;
    }

    const uint32_t ageMs = oldestAgeMs(nowMs);

    return (ageMs < mLatency) ? mLatency - ageMs : 0;
}

uint32_t Sensor::DataQueue::oldestAgeMs(uint32_t nowMs)
{
    // The readings lie in the order they came, so the oldest is the first.
    // The age is taken modulo the width of the stamps. A clock that starts
    // over before its type runs out makes a reading stamped before that read
    // far older than it is, and it goes at once: early, never late.
    const uint32_t stampMs = (*this)[0].mTimeStamp;

    return nowMs - stampMs;
}

void Sensor::DataQueue::deliver()
{
    const uint16_t count = mIndex;

    mIndex = 0;

    if (mListener && count != 0) {
        mListener->onSdlNewData(mDriver.getHandle(),
                                mQueue,
                                count,
                                mStride);
    }
}

/**
 * @brief Return the listener associated with this queue.
 *
 * @return Pointer to the ISensorDataListener, or nullptr if none.
 */
SDK::Interface::ISensorDataListener* Sensor::DataQueue::getListener() const
{
    return mListener;
}

/**
 * @brief Get the currently configured sampling period for this listener.
 *
 * @return Period in ms.
 */
float Sensor::DataQueue::getPeriod() const
{
    return mPeriod;
}

uint32_t Sensor::DataQueue::getLatency() const
{
    return mLatency;
}

/**
 * @brief Indexed access to a slot of the queue.
 *
 * @param i Index of the slot, below the capacity.
 *
 * @return Reference to the i-th Sensor::Data entry.
 */
SDK::Sensor::Data& Sensor::DataQueue::operator[](size_t i)
{
    assert(i < mCapacity);

    std::uint8_t* base = reinterpret_cast<std::uint8_t*>(mQueue);
    auto* ptr          = reinterpret_cast<SDK::Sensor::Data*>(base + i * mStride);

    return *ptr;
}

uint16_t Sensor::DataQueue::calcStride(uint16_t fieldCount)
{
    assert(fieldCount > 0);

    const std::size_t headerSize  = sizeof(SDK::Sensor::Data); // with mValue[1] inside
    const std::size_t extraFields = (fieldCount - 1) * sizeof(SDK::Sensor::Data::Field);

    return static_cast<uint16_t>(headerSize + extraFields);
}
