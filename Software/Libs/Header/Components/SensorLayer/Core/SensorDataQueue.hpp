/**
 ******************************************************************************
 * @file    SensorDataQueue.hpp
 * @brief   Queue for SensorData
 * 
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef SENSOR_DATA_QUEUE_HPP
#define SENSOR_DATA_QUEUE_HPP

#include "SensorLayer/Core/SensorDataHolder.hpp"
#include "SDK/Interfaces/ISensorDataListener.hpp"
#include "SensorLayer/Core/SampleRateAdapter.hpp"

#include <stdint.h>

namespace Sensor {
    class Driver;
}

namespace Sensor
{
    class DataQueue
    {
    public:
        DataQueue(Sensor::Driver&                      driver,
                  uint16_t                             fieldCount,
                  SDK::Interface::ISensorDataListener* listener,
                  float                                period,
                  uint32_t                             latency);
        ~DataQueue();

        void reinit(float period, uint32_t latency);

        /// Keep a reading if it is this listener's turn, counted in the
        /// step the driver reports for its source.
        void pushData(const SDK::Sensor::Data& d);
        /// The same, counted in the given step: for injected values, which
        /// come at a step of their own rather than the source's.
        void pushData(const SDK::Sensor::Data& d, uint32_t stepUs);
        void forceData(const SDK::Sensor::Data& d);

        /**
         * @brief Hand over what is held, if it cannot wait for the next chance.
         *
         * A listener's latency is how old the oldest reading it is given may
         * be. The readings are handed over when the queue is full, and a
         * source that gives its readings in batches does not fill it evenly:
         * what a batch leaves in the queue waits for the next batch, and if
         * the source stops, for ever. So a source says at the end of a batch
         * when its next one is due, and the readings go now if the oldest of
         * them would have reached the latency by then. All of them go, in
         * order; the latency is a bound, not an appointment. Asked with next
         * to no time to the next chance, the queue hands over only what has
         * reached its latency or is about to: that is the manager's question,
         * which it puts when dueInMs() said to, for a source that gave
         * nothing more.
         *
         * @param nowMs        The clock the readings are stamped from.
         * @param nextChanceMs Time until the next chance to hand over.
         * @return Whether anything was handed over.
         */
        bool flushIfDue(uint32_t nowMs, uint32_t nextChanceMs);

        /// Time until the oldest reading held reaches the latency, 0 if it
        /// has; kNothingHeld when the queue is empty.
        uint32_t dueInMs(uint32_t nowMs);

        static constexpr uint32_t kNothingHeld = UINT32_MAX;

        SDK::Interface::ISensorDataListener* getListener() const;

        float    getPeriod() const;
        uint32_t getLatency() const;

        static constexpr uint16_t kMaxCapacity = 500;

        /**
         * @brief How many readings a queue holds: those that lie within the
         *        latency, which is the latency over the period, rounded up.
         *
         * @param period  Listener's period in ms.
         * @param latency Listener's latency in ms.
         * @return 1..kMaxCapacity; 1 when nothing can be held back.
         */
        static uint16_t computeCapacity(float period, uint32_t latency);

        /**
         * @brief How many whole periods go into the latency.
         *
         * Not the capacity: a time paced by it does not pass a latency of
         * a period or more.
         *
         * @param period  Listener's period in ms.
         * @param latency Listener's latency in ms.
         * @return 1..kMaxCapacity.
         */
        static uint16_t wholePeriods(float period, uint32_t latency);

    private:
        SDK::Sensor::Data& operator[](size_t i);

        /**
         * @brief  Allocate the queue storage and publish the new geometry.
         * @retval false if the allocation failed. Nothing is published in that
         *         case: capacity, size and the buffer pointer are left exactly
         *         as they were, so the caller still owns whatever it had.
         */
        bool initQueue(uint16_t capacity);

    public:
        /// False when construction could not allocate a queue. Such a queue
        /// accepts no samples; see forceData().
        bool isValid() const { return mQueue != nullptr; }

    private:

        /// Give the listener the readings gathered so far and start over.
        void deliver();

        /// Age of the oldest reading held. The queue must not be empty.
        uint32_t oldestAgeMs(uint32_t nowMs);

        static uint16_t calcStride(uint16_t fieldCount);

        Sensor::Driver&                      mDriver;
        SDK::Interface::ISensorDataListener* mListener;
        const uint16_t                       mFieldCount;
        const uint16_t                       mStride;
        SDK::Sensor::Data*                   mQueue;
        uint16_t                             mIndex;
        uint16_t                             mSize;
        uint16_t                             mCapacity;
        float                                mPeriod;
        uint32_t                             mLatency;
        Sensor::SampleRateAdapter            mSRA;
    };

} // namespace Sensor

#endif // SENSOR_DATA_QUEUE_HPP
