/**
 * Host unit tests for Sensor::DataQueue: what a listener is handed, how much
 * of it and how old.
 *
 * A listener's latency bounds the age of the oldest reading it is given. The
 * queue hands over when it is full, and when it is asked whether what it
 * holds can wait for the next chance and it cannot.
 */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "SensorLayer/Core/SensorDataQueue.hpp"
#include "SensorLayer/Core/SensorDriver.hpp"

namespace
{

struct Delivery {
    uint16_t count;
    uint32_t firstMs;
    uint32_t lastMs;
    uint8_t  handle;
};

class Listener : public SDK::Interface::ISensorDataListener
{
public:
    void onSdlNewData(uint16_t handle, const SDK::Sensor::Data* base, uint16_t count, uint16_t stride) override
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(base);
        const auto* first = reinterpret_cast<const SDK::Sensor::Data*>(bytes);
        const auto* last  = reinterpret_cast<const SDK::Sensor::Data*>(bytes + (count - 1) * stride);

        for (uint16_t i = 1; i < count; ++i) {
            const auto* prev = reinterpret_cast<const SDK::Sensor::Data*>(bytes + (i - 1) * stride);
            const auto* cur  = reinterpret_cast<const SDK::Sensor::Data*>(bytes + i * stride);
            inOrder = inOrder && (cur->mTimeStamp > prev->mTimeStamp);
        }

        deliveries.push_back({ count, first->mTimeStamp, last->mTimeStamp, static_cast<uint8_t>(handle) });
    }

    std::vector<Delivery> deliveries;
    bool                  inOrder = true;
};

// One field a reading, so SDK::Sensor::Data is a whole reading as it stands.
constexpr uint16_t kFields = 1;

SDK::Sensor::Data reading(uint32_t stampMs)
{
    SDK::Sensor::Data d {};
    d.mTimeStamp   = stampMs;
    d.mTimeStampUs = 0;
    d.mValue[0].u32 = stampMs;
    return d;
}

// A batch of `count` readings `periodMs` apart, the newest stamped `newestMs`.
void pushBatch(Sensor::DataQueue& queue, uint32_t newestMs, uint32_t periodMs, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        queue.pushData(reading(newestMs - (count - 1 - i) * periodMs));
    }
}

} // namespace

TEST(SensorDataQueue, AFullQueueIsHandedOverWhole)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 100);   // five readings

    pushBatch(queue, 1'000, 20, 4);
    EXPECT_TRUE(listener.deliveries.empty());

    queue.pushData(reading(1'020));
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 5u);
    EXPECT_EQ(listener.deliveries[0].firstMs, 940u);
    EXPECT_EQ(listener.deliveries[0].lastMs, 1'020u);
    EXPECT_EQ(listener.deliveries[0].handle, 7u);
}

// The queue decimates against the step its driver reports, whatever the
// stamps say: a 40 ms listener on a nominal 20 ms source keeps every second
// reading of a stream running 0.5 % slow. With no step reported it keeps all.
TEST(SensorDataQueue, DecimatesAgainstTheStepTheDriverReports)
{
    Sensor::Driver driver;
    driver.sourceStepUs = 20'000;

    Listener          decimated;
    Sensor::DataQueue queue(driver, kFields, &decimated, 40, 400);  // ten readings

    Sensor::Driver    silent;
    Listener          whole;
    Sensor::DataQueue all(silent, kFields, &whole, 40, 400);

    for (uint32_t i = 0; i < 200; ++i) {
        const uint32_t stampMs = 1'000 + (i * 201) / 10;   // 20.1 ms apart
        queue.pushData(reading(stampMs));
        all.pushData(reading(stampMs));
    }

    uint32_t kept = 0;
    for (const Delivery& d : decimated.deliveries) {
        kept += d.count;
    }
    EXPECT_EQ(kept, 100u);

    uint32_t passed = 0;
    for (const Delivery& d : whole.deliveries) {
        passed += d.count;
    }
    EXPECT_EQ(passed, 200u);
}

// Readings that come at a step of their own (injected values) are counted in
// that step, not the source's: a 40 ms listener on a 20 ms source keeps every
// reading of a 40 ms stream, where counted in 20 ms steps it would keep every
// second one.
TEST(SensorDataQueue, DecimatesAgainstAGivenStep)
{
    Sensor::Driver driver;
    driver.sourceStepUs = 20'000;

    Listener          ownStep;
    Sensor::DataQueue queue(driver, kFields, &ownStep, 40, 400);   // ten readings

    Listener          sourceStep;
    Sensor::DataQueue bySource(driver, kFields, &sourceStep, 40, 400);

    for (uint32_t i = 0; i < 20; ++i) {
        queue.pushData(reading(1'000 + i * 40), 40'000);
        bySource.pushData(reading(1'000 + i * 40));
    }

    uint32_t kept = 0;
    for (const Delivery& d : ownStep.deliveries) {
        kept += d.count;
    }
    EXPECT_EQ(kept, 20u);

    uint32_t keptBySource = 0;
    for (const Delivery& d : sourceStep.deliveries) {
        keptBySource += d.count;
    }
    EXPECT_EQ(keptBySource, 10u);
}

// A period float arithmetic leaves a hair under a whole multiple of the step
// still divides by it: cut rather than rounded, 39.99999 ms would give a
// divisor of 1 and pass every reading.
TEST(SensorDataQueue, APeriodAHairUnderAMultipleOfTheStepStillDividesByIt)
{
    Sensor::Driver driver;
    driver.sourceStepUs = 20'000;

    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 39.99999f, 400);

    for (uint32_t i = 0; i < 200; ++i) {
        queue.pushData(reading(1'000 + i * 20));
    }

    uint32_t kept = 0;
    for (const Delivery& d : listener.deliveries) {
        kept += d.count;
    }
    EXPECT_EQ(kept, 100u);
}

TEST(SensorDataQueue, NothingHeldIsNothingToHandOver)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 100);

    EXPECT_FALSE(queue.flushIfDue(5'000, 1'000));

    // Nor after a delivery that emptied it: the slots still hold what went.
    pushBatch(queue, 1'000, 20, 5);
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_FALSE(queue.flushIfDue(900'000, 1'000));
    EXPECT_EQ(listener.deliveries.size(), 1u);
}

TEST(SensorDataQueue, WhatCanWaitForTheNextChanceWaits)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);   // fifty readings

    pushBatch(queue, 10'000, 20, 5);        // the oldest stamped 9'920

    // 180 ms old, the next batch in 100 ms: 280 of the 1000 allowed.
    EXPECT_FALSE(queue.flushIfDue(10'100, 100));
    EXPECT_TRUE(listener.deliveries.empty());
}

TEST(SensorDataQueue, WhatCannotWaitGoesAllOfItAndInOrder)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);

    pushBatch(queue, 10'000, 20, 45);       // the oldest stamped 9'120

    // 899 ms old with 100 ms to go is a millisecond short of the latency.
    EXPECT_FALSE(queue.flushIfDue(10'019, 100));

    // 900 ms old with 100 ms to go reaches it.
    EXPECT_TRUE(queue.flushIfDue(10'020, 100));
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 45u);
    EXPECT_EQ(listener.deliveries[0].firstMs, 9'120u);
    EXPECT_EQ(listener.deliveries[0].lastMs, 10'000u);
    EXPECT_TRUE(listener.inOrder);

    // And the queue starts over: fifty more fill it exactly.
    pushBatch(queue, 11'000, 20, 50);
    ASSERT_EQ(listener.deliveries.size(), 2u);
    EXPECT_EQ(listener.deliveries[1].count, 50u);
    EXPECT_EQ(listener.deliveries[1].firstMs, 10'020u);
}

// The tightest listener of a source is told the next batch comes in its own
// latency. Whatever a batch leaves in its queue cannot wait for that.
TEST(SensorDataQueue, TheTightestListenerIsLeftNothing)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 40, 100);    // three readings

    pushBatch(queue, 5'000, 40, 5);         // three, and two left
    ASSERT_EQ(listener.deliveries.size(), 1u);

    EXPECT_TRUE(queue.flushIfDue(5'000, 100));
    ASSERT_EQ(listener.deliveries.size(), 2u);
    EXPECT_EQ(listener.deliveries[1].count, 2u);
    EXPECT_EQ(listener.deliveries[1].firstMs, 4'960u);
}

// A queue holds as many readings as lie within the latency: the latency over
// the period, rounded up. One fewer and the queue would fill and go a period
// before it has to; one more and its oldest reading would pass the latency
// before it filled.
TEST(SensorDataQueue, HoldsTheReadingsThatLieWithinTheLatency)
{
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(20, 100), 5u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(40, 1000), 25u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(40, 100), 3u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(200, 500), 3u);

    // A period float arithmetic leaves a hair off a whole part of the latency
    // is that part of it still.
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(19.99999f, 100), 5u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(20.00001f, 100), 5u);

    // So is one that is not a whole number of microseconds: 30, 60 and
    // 120 Hz go into 100 ms three, six and twelve times.
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(1000.0f / 30, 100), 3u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(1000.0f / 60, 100), 6u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(1000.0f / 120, 100), 12u);

    // What is left over is a reading more however many periods went before
    // it: 250 ms is 485.9 periods of 1943.6 Hz.
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(1000.0f / 1943.6f, 250), 486u);

    // No room to hold anything back: every reading goes as it comes.
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(40, 40), 1u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(100, 40), 1u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(40, 0), 1u);
    EXPECT_EQ(Sensor::DataQueue::computeCapacity(0, 100), 1u);

    EXPECT_EQ(Sensor::DataQueue::computeCapacity(1, 100'000), 500u);
}

// What paces the sweep is counted in whole periods, so that it comes round
// within a latency of a period or more: 80 ms for a 40 ms listener with
// 100 ms of it.
TEST(SensorDataQueue, WholePeriodsAreRoundedDown)
{
    EXPECT_EQ(Sensor::DataQueue::wholePeriods(20, 100), 5u);
    EXPECT_EQ(Sensor::DataQueue::wholePeriods(40, 100), 2u);
    EXPECT_EQ(Sensor::DataQueue::wholePeriods(200, 500), 2u);

    EXPECT_EQ(Sensor::DataQueue::wholePeriods(40, 40), 1u);
    EXPECT_EQ(Sensor::DataQueue::wholePeriods(100, 40), 1u);
    EXPECT_EQ(Sensor::DataQueue::wholePeriods(40, 0), 1u);
    EXPECT_EQ(Sensor::DataQueue::wholePeriods(0, 100), 1u);

    EXPECT_EQ(Sensor::DataQueue::wholePeriods(1, 100'000), 500u);
}

// A 40 ms listener with 100 ms of latency on a source that gives five
// readings 20 ms apart every 100 ms: two or three of each batch are its own.
// It is handed them once a batch, not a full queue and then what was left.
TEST(SensorDataQueue, ABatchThatIsNotAWholePartOfTheLatencyGoesInOneDelivery)
{
    Sensor::Driver driver;
    driver.sourceStepUs = 20'000;

    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 40, 100);

    constexpr uint32_t kBatches = 40;

    uint32_t now = 70'000;

    for (uint32_t batch = 0; batch < kBatches; ++batch) {
        const std::size_t before = listener.deliveries.size();

        now += 100;
        pushBatch(queue, now, 20, 5);
        queue.flushIfDue(now, 100);

        EXPECT_EQ(listener.deliveries.size() - before, 1u) << "batch " << batch;
    }

    uint32_t kept = 0;
    for (const Delivery& d : listener.deliveries) {
        EXPECT_GE(d.count, 2u);
        EXPECT_LE(d.count, 3u);
        kept += d.count;
    }
    EXPECT_EQ(kept, kBatches * 5 / 2);
    EXPECT_TRUE(listener.inOrder);
}

// The same listener on a source that gives its readings one at a time. The
// queue fills before its oldest reading is as old as the latency, so it is
// the queue filling that hands them over and the sweep finds nothing overdue.
TEST(SensorDataQueue, AQueueFillsBeforeItsOldestReadingReachesTheLatency)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 40, 100);

    for (uint32_t i = 0; i < 30; ++i) {
        const uint32_t now = 80'000 + i * 40;

        queue.pushData(reading(now));
        EXPECT_FALSE(queue.flushIfDue(now + 39, 3)) << "reading " << i;   // just before the next
    }

    ASSERT_EQ(listener.deliveries.size(), 10u);
    for (const Delivery& d : listener.deliveries) {
        EXPECT_EQ(d.count, 3u);
        EXPECT_LT(d.lastMs - d.firstMs, 100u);
    }
}

// A listener at 20 ms and a second, beside one that kept the source to batches
// of five every 100 ms. That one leaves, and the source goes back to fifty a
// second with forty-five readings lying in the queue. Handed over only when
// full, the queue would give forty-five readings a second old with every
// batch from then on, the oldest nearly two seconds old.
TEST(SensorDataQueue, AChangeOfBatchSizeDoesNotLeaveTheListenerASecondBehind)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);

    uint32_t now = 100'000;

    for (int i = 0; i < 9; ++i) {           // nine batches of five: forty-five held
        now += 100;
        pushBatch(queue, now, 20, 5);
        queue.flushIfDue(now, 100);
    }
    ASSERT_TRUE(listener.deliveries.empty());

    for (int i = 0; i < 5; ++i) {           // and now fifty at a time, a second apart
        now += 1'000;
        pushBatch(queue, now, 20, 50);
        queue.flushIfDue(now, 1'000);

        const Delivery& last = listener.deliveries.back();
        EXPECT_EQ(last.lastMs, now) << "batch " << i;
        EXPECT_LE(now - listener.deliveries[listener.deliveries.size() - 1].firstMs, 1'900u);
    }

    // After the first of them the queue is in step: a delivery a batch, the
    // oldest reading under a second old.
    for (std::size_t i = 2; i < listener.deliveries.size(); ++i) {
        EXPECT_EQ(listener.deliveries[i].count, 50u) << "delivery " << i;
        EXPECT_EQ(listener.deliveries[i].lastMs - listener.deliveries[i].firstMs, 980u) << "delivery " << i;
    }
    EXPECT_TRUE(listener.inOrder);
}

// The source stopped with readings in the queue. The sweep asks when they
// are due, sleeps no longer than that, and hands them over at their latency:
// not before, whatever woke it in between, and not after.
TEST(SensorDataQueue, ASourceThatStoppedDoesNotKeepWhatItGave)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 40, 1000);

    pushBatch(queue, 20'000, 40, 10);       // the oldest stamped 19'640

    // Woken for other reasons every 80 ms: nothing is overdue yet.
    uint32_t now = 20'000;
    for (int i = 0; i < 5; ++i) {
        now += 80;
        EXPECT_FALSE(queue.flushIfDue(now, 0));
    }
    EXPECT_EQ(queue.dueInMs(now), 1'000u - (now - 19'640u));

    // Slept for as long as dueInMs() said.
    now += queue.dueInMs(now);
    EXPECT_EQ(queue.dueInMs(now), 0u);
    EXPECT_TRUE(queue.flushIfDue(now, 0));

    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 10u);
    EXPECT_EQ(now - listener.deliveries[0].firstMs, 1'000u);
    EXPECT_EQ(queue.dueInMs(now), Sensor::DataQueue::kNothingHeld);
}

// A sweep that runs often, because other sensors wake it, must not empty a
// queue that is only waiting for its source: the listener asked for a second's
// worth at a time.
TEST(SensorDataQueue, FrequentSweepsDoNotBreakUpTheBatches)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 40, 1000);

    uint32_t now = 50'000;

    for (int batch = 0; batch < 60; ++batch) {
        now += 80;
        pushBatch(queue, now, 40, 2);
        queue.flushIfDue(now, 80);          // the source: the next batch in 80 ms
        queue.flushIfDue(now + 1, 0);       // the sweep, a moment later
    }

    ASSERT_GE(listener.deliveries.size(), 2u);
    for (const Delivery& d : listener.deliveries) {
        EXPECT_GE(d.count, 22u);
        EXPECT_LE(d.lastMs - d.firstMs, 1'000u);
    }
}

// A clock that runs to the end of its type starts over at zero, and a reading
// stamped just before that is aged across it like any other.
TEST(SensorDataQueue, AReadingFromBeforeTheClockStartedOverIsAgedAcrossIt)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);

    pushBatch(queue, 4'294'967'246u, 20, 3);    // the oldest 90 ms short of the end

    EXPECT_EQ(queue.dueInMs(5), 1'000u - 95u);
    EXPECT_FALSE(queue.flushIfDue(5, 100));     // 95 ms old, 100 to go
    EXPECT_TRUE(listener.deliveries.empty());

    EXPECT_TRUE(queue.flushIfDue(910, 0));      // 1000 ms old
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 3u);
}

// A clock that starts over before its type runs out makes a reading stamped
// before that look far older than it is. It goes at once: early, rather than
// held until the clock comes round to its stamp again.
TEST(SensorDataQueue, AReadingFromBeforeAShortClockStartedOverGoesAtOnce)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);

    pushBatch(queue, 4'194'303'990u, 20, 3);    // just short of 4'194'304'000

    EXPECT_EQ(queue.dueInMs(5), 0u);
    EXPECT_TRUE(queue.flushIfDue(5, 0));
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 3u);
}

// Carried over to a queue of another size, the readings keep their place in
// the line and their claim on the latency.
TEST(SensorDataQueue, ReadingsCarriedOverByANewLatencyAreStillDue)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);

    pushBatch(queue, 30'000, 20, 8);        // the oldest stamped 29'860

    queue.reinit(20, 500);                  // twenty-five readings now
    EXPECT_TRUE(listener.deliveries.empty());

    EXPECT_FALSE(queue.flushIfDue(30'000, 100));    // 140 ms old, 100 to go
    EXPECT_TRUE(queue.flushIfDue(30'260, 100));     // 400 ms old, 100 to go
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 8u);
    EXPECT_EQ(listener.deliveries[0].firstMs, 29'860u);
}

// A new period closes the stream the listener had: what was held of it is
// handed over at once and whole, and none of it waits to go among readings
// thinned to the new period.
TEST(SensorDataQueue, ANewPeriodHandsOverWhatIsHeldFirst)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);   // fifty readings

    pushBatch(queue, 50'000, 20, 8);        // stamped 49'860 to 50'000
    ASSERT_TRUE(listener.deliveries.empty());

    queue.reinit(40, 1000);                 // twenty-five readings now
    ASSERT_EQ(listener.deliveries.size(), 1u);
    EXPECT_EQ(listener.deliveries[0].count, 8u);
    EXPECT_EQ(listener.deliveries[0].firstMs, 49'860u);
    EXPECT_EQ(listener.deliveries[0].lastMs, 50'000u);

    // Nothing of it is left to go with what comes next.
    queue.pushData(reading(50'020));
    EXPECT_TRUE(queue.flushIfDue(51'020, 0));
    ASSERT_EQ(listener.deliveries.size(), 2u);
    EXPECT_EQ(listener.deliveries[1].count, 1u);
    EXPECT_EQ(listener.deliveries[1].firstMs, 50'020u);
}

// A new latency does not touch which readings are kept: the count goes on
// from where it was, and no reading is handed over out of turn.
TEST(SensorDataQueue, ANewLatencyAloneKeepsTheDecimation)
{
    Sensor::Driver driver;
    driver.sourceStepUs = 20'000;

    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 40, 40);    // one reading

    queue.pushData(reading(1'000));
    queue.pushData(reading(1'020));
    queue.pushData(reading(1'040));
    ASSERT_EQ(listener.deliveries.size(), 2u);

    queue.reinit(40, 80);                   // two readings now

    queue.pushData(reading(1'060));         // not this listener's turn
    queue.pushData(reading(1'080));
    queue.pushData(reading(1'100));
    queue.pushData(reading(1'120));
    ASSERT_EQ(listener.deliveries.size(), 3u);
    EXPECT_EQ(listener.deliveries[2].count, 2u);
    EXPECT_EQ(listener.deliveries[2].firstMs, 1'080u);
    EXPECT_EQ(listener.deliveries[2].lastMs, 1'120u);
}

// Carried over to a queue smaller than what it holds, the readings fill it,
// go over whole as it fills, and what is left over stays for its turn: every
// reading once, in order, and none of the empty slots of the old queue with
// them.
TEST(SensorDataQueue, ReadingsCarriedIntoASmallerQueueGoOnceAndInOrder)
{
    Sensor::Driver    driver;
    Listener          listener;
    Sensor::DataQueue queue(driver, kFields, &listener, 20, 1000);   // fifty readings

    pushBatch(queue, 40'000, 20, 12);       // stamped 39'780 to 40'000

    queue.reinit(20, 100);                  // five readings now
    ASSERT_EQ(listener.deliveries.size(), 2u);
    EXPECT_EQ(listener.deliveries[0].count, 5u);
    EXPECT_EQ(listener.deliveries[0].firstMs, 39'780u);
    EXPECT_EQ(listener.deliveries[0].lastMs, 39'860u);
    EXPECT_EQ(listener.deliveries[1].count, 5u);
    EXPECT_EQ(listener.deliveries[1].firstMs, 39'880u);
    EXPECT_EQ(listener.deliveries[1].lastMs, 39'960u);

    EXPECT_TRUE(queue.flushIfDue(40'000, 100));
    ASSERT_EQ(listener.deliveries.size(), 3u);
    EXPECT_EQ(listener.deliveries[2].count, 2u);
    EXPECT_EQ(listener.deliveries[2].firstMs, 39'980u);
    EXPECT_EQ(listener.deliveries[2].lastMs, 40'000u);
    EXPECT_TRUE(listener.inOrder);
}
