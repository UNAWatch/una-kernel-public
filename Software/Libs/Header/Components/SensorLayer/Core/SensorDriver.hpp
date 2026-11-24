/**
 ******************************************************************************
 * @file    Sensor.cpp
 * @brief   SensorDriver class
 *
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef SENSOR_DRIVER_HPP
#define SENSOR_DRIVER_HPP

#include "SensorLayer/Core/SensorDataSample.hpp"
#include "SensorLayer/Core/SensorDataQueue.hpp"
#include "SensorLayer/Core/LatencyAggregate.hpp"
#include "SensorLayer/Core/ISensor.hpp"
#include "SensorLayer/Core/SourceStepTrack.hpp"
#include "SensorLayer/Core/SourceTick.hpp"
#include "SDK/SensorLayer/SensorTypes.hpp"
#include "SDK/Interfaces/ISensorDataListener.hpp"
#include "OS.hpp"

#include "etl/list.h"
#include "etl/pool.h"

#include <atomic>

namespace Sensor
{
    class Driver;

    class ISensorDriverCtrl
    {
    public:
        virtual float sdcStart(Sensor::Driver* driver, float period)        = 0;
        virtual void  sdcStop(Sensor::Driver* driver)                       = 0;

        // Period handling is meaningful only for PERIODIC_BASED drivers.
        // Event-based sensors deliver on their own events; they can leave
        // these at the defaults instead of implementing them.
        virtual float sdcUpdatePeriod(Sensor::Driver* driver, float period)
        {
            (void) driver;
            return period;
        }
        virtual float sdcGetMinPeriod(Sensor::Driver* driver)
        {
            (void) driver;
            return 0.0f;
        }

        // The tightest delivery latency among the driver's listeners, in ms.
        // A source that gathers readings in hardware before handing them over
        // sizes that batch from it; one that hands each reading over as it is
        // taken has nothing to do. For the first listener it comes before
        // sdcStart(), so the source starts with the batch it needs. Periodic
        // drivers only.
        virtual void  sdcUpdateLatency(Sensor::Driver* driver, uint32_t latency)
        {
            (void) driver;
            (void) latency;
        }

        virtual void  sdcNewConnection(Sensor::Driver* driver) { (void) driver; };
        virtual const char* sdcGetDescription(Sensor::Driver* driver)       = 0;
    }; /* class ISensorDriverCtrl */

    class Driver {
    public:
        enum class Mode {
            PERIODIC_BASED,
            EVENT_BASED,
        };

        /// Every driver's listener queues come out of one pool, rather than
        /// each driver reserving its own worst case. The drivers cannot all
        /// hold their maximum at once, so reserving per driver pays for a
        /// combination that cannot occur: at eight queues each it is 19 KB
        /// against this pool's 6 KB, and it still caps any single driver at
        /// eight. What is genuinely bounded is the number of listeners in
        /// flight across the whole sensor layer, and that is what this
        /// counts - any one driver may take all of them if it needs to.
        ///
        /// 96 is a headroom figure, NOT a derived count, and it is worth
        /// being plain about that because the comment here used to read like
        /// arithmetic.
        ///
        /// The kernel half can be counted, and drifts even so. It is about
        /// thirty: twenty-six Sensor::Connection members plus four
        /// sub-sensors that connect as their own listener without holding
        /// one (ImuSubSensorAccel, ImuSubSensorFusion, ImuSubSensorGyro,
        /// MagSubSensorField). That correction arrived during review of the
        /// change that introduced the number, which is what any hand count
        /// does. It is also a ceiling rather than a resting level - the
        /// activity FSM, Compass, the accel loggers and ErrorMonitor connect
        /// on demand - and the sensor test commands hold up to eleven more
        /// while one runs.
        ///
        /// The app half cannot be counted at all. A field app may subscribe
        /// to a dozen sensors in one call and nothing in this repository
        /// will ever know it happened, so that side is a runtime condition,
        /// not a quantity. What is known: a loaded app has exactly two
        /// listeners, one per process, each spending a slot per driver it
        /// subscribes to, and a suspended app keeps them, since
        /// disconnectListener() runs on the stop and terminate paths and not
        /// on suspend. So the budget covers every resident app, not just the
        /// one in front of the user.
        ///
        /// Which is why the pool logs its high-water mark. The number that
        /// settles this is measured on a device, not argued here; a normal
        /// session peaked at 30 of 96.
        ///
        /// The trade this makes is that the drivers share a limit. A
        /// subscription-heavy driver can leave nothing for an unrelated
        /// one, which a per-driver reservation could not do. Running out
        /// refuses the connect and logs it - it never overruns - so the
        /// symptom is a subscription that did not happen and said so.
        static constexpr size_t skListenerPoolSize = 96;

        using ListenerList = etl::list_ext<Sensor::DataQueue>;

        Driver(Interface::ISensor&        sensor,
               SDK::Sensor::Type          t,
               uint16_t                   fieldCount,
               Sensor::ISensorDriverCtrl& control,
               Mode                       mode = Mode::PERIODIC_BASED);

        Interface::ISensor& getSensor();

        void pushDataSample();

        /**
         * @brief Called by a source that gives its readings in batches, after
         *        the last reading of a batch.
         *
         * Listeners are handed what they hold if it cannot wait for the
         * next batch within their latency.
         *
         * @param nextBatchMs Time until the next batch at the most.
         */
        void endOfBatch(uint32_t nextBatchMs);

        /**
         * @brief Called by the manager on every sweep: hand over what has
         *        reached its latency, or will within sSweepLeadMs. Covers a
         *        source that stopped, or gave its next batch later than it
         *        said.
         * @return Time until the next reading held reaches its latency, for
         *         the manager to wake by; 0 when something was handed over,
         *         so that a listener passing readings on gets a sweep to do
         *         it in; DataQueue::kNothingHeld for none.
         */
        uint32_t flushOverdue();

        /// Whether the driver has listeners to refresh for.
        bool isActive() const;

        /**
         * @brief Report the nominal sampling step of the source.
         *
         * The step the part was set to for the sample pushed next, not one
         * measured from timestamps and not the period a listener asked for;
         * 0 when the source has none. Each listener queue decimates against
         * it (see SampleRateAdapter). A source that holds samples before it
         * pushes them writes it for each one, from the thread that pushes;
         * one that does not, from whichever thread applies the rate. Read on
         * every sample pushed.
         *
         * @param us Step in microseconds.
         */
        void setSourceStepUs(uint32_t us);

        /// Nominal sampling step of the source in microseconds, 0 if unknown.
        uint32_t getSourceStepUs() const;

        /**
         * @brief Give the driver the record of the steps its source was at.
         *
         * For a source that holds samples before it pushes them: a listener
         * handed several samples at once can then ask which step each was
         * taken at. Set once, before the sensor is registered.
         */
        void setSourceStepTrack(const SourceStepTrack* track);

        /**
         * @brief The step the source was at for a sample it published.
         * @param tsUs The sample's stamp, us.
         * @return The step, us; the one reported last where the source keeps
         *         no record.
         */
        uint32_t getSourceStepUsAt(uint64_t tsUs) const;

        /**
         * @brief Give the driver the clock of a source sampled on the sweep.
         *
         * The sweep then wakes for its deadlines as well as for the
         * listeners' latencies. Set once, before the sensor is registered.
         */
        void setSourceTick(SourceTick* tick);

        /// Whether the source's clock has a sample due; see SourceTick::check().
        /// Sweep thread only.
        bool tickDue();

        // ISensorDriver interface
        bool connect(SDK::Interface::ISensorDataListener* listener,
                     float                                period  = 0,
                     uint32_t                             latency = 0);
        void disconnect(SDK::Interface::ISensorDataListener* listener);
        SDK::Sensor::Type getType() const;
        Mode getMode() const;
        float getRefreshPeriod() const;

        uint32_t getCountOfListeners();

        Sensor::DataSample& getDataSample();

        float getMinPeriod();

        void    setHandle(uint8_t value);
        uint8_t getHandle();

        const char* getDescription();

        /**
         * @brief  Publish given values in place of the source's (developer mode).
         *
         * @details Until stopInjection(), the source's own samples are dropped
         *          and the Manager's sweep publishes these values instead: at
         *          the fastest listener's sampling period for a periodic
         *          driver, once per call for an event-based one. Listeners get
         *          them through the usual queues, so apps and kernel consumers
         *          alike see them. Nothing is published while no listener is
         *          connected. On an event-based driver, waits up to a second
         *          for the previous injected event to be published, so that
         *          events injected close together are all delivered.
         *
         * @param  values  One value per field, in the type's field order.
         * @param  count   Must equal the driver's field count.
         *
         * @return false if count is wrong or there is no memory.
         */
        bool inject(const SDK::Sensor::Data::Field* values, uint16_t count);

        /// @brief Go back to the source's own samples.
        void stopInjection();

        bool isInjected() const;

        uint16_t getFieldCount() const;

        /**
         * @brief  Publish the injected values if they are due. Sweep thread only.
         *
         * @return Milliseconds until the next is due, UINT32_MAX for none.
         */
        uint32_t publishInjected(uint32_t nowMs);

    private:
        /// Hands mDataSample to the listeners, each periodic one decimating it
        /// as a stream of this step (us). Called with mMutex held.
        void deliverLocked(uint32_t stepUs);

        /// The shared node pool. Function-local inside the accessor so it is
        /// built before the first driver takes a reference to it, whatever
        /// order the sensor objects happen to be constructed in.
        static etl::ipool& listenerPool();

        /// Guards the pool itself. Each driver protects its own list with its
        /// own mMutex, which is enough while the nodes are its own - but the
        /// pool is shared, so two drivers connecting at once would otherwise
        /// race on its free list, and a full() answered for one of them could
        /// be acted on by the other. Every pool-touching operation takes this
        /// across the test and the operation together.
        ///
        /// Innermost of this file's locks: mConnMutex -> mMutex -> this.
        ///
        /// It is NOT a leaf, and do not treat it as one. Taking a node
        /// constructs a DataQueue, whose constructor allocates its sample
        /// buffer and logs if that fails; giving one back destroys it and
        /// frees the buffer. So the heap lock - and on the failure path the
        /// logger - nest inside this one, and every driver's connect and
        /// disconnect serialise behind them. Nothing in the allocator or the
        /// logger reaches back into the sensor layer, so there is no cycle;
        /// anything added here that could must not be.
        static OS::Mutex& listenerPoolMutex();

        static constexpr float sMinPeriod = 0.5f;

        /// How far ahead of its latency the sweep hands a reading over. The
        /// sweep waits for a deadline in ticks a little shorter than a
        /// millisecond, rounded down, and a wait ends anywhere in its last
        /// tick, so it can wake nearly two ms short; the millisecond clock
        /// the age is read on can lose one more. Asking this far ahead keeps
        /// the latency from being overrun without a second wake.
        ///
        /// A latency shorter than the lead is handed over at every sweep that
        /// finds anything held, and each hand-over asks for another sweep at
        /// once. That is one more pass, not a spin: the pass after it finds
        /// the queue empty unless the source has pushed again, and a source
        /// that pushes says at the end of its batch when the next one is due,
        /// which for such a latency empties the queue before the sweep looks.
        static constexpr uint32_t sSweepLeadMs = 3;

        bool connectEventBased(SDK::Interface::ISensorDataListener* listener);

        /// Undo the start that a first connect() performed, when the
        /// listener it was starting for could not be stored. Same sequence
        /// as the last-listener path of disconnect(), and for the same
        /// reasons - keep the two together if either changes.
        void stopAfterFailedFirstListener();

        /// Put the sampling period back where the surviving listeners want
        /// it, when a listener that had already tightened it could not be
        /// stored. The mirror of the non-last path of disconnect(), just as
        /// stopAfterFailedFirstListener() mirrors the last-listener one.
        void restorePeriodAfterFailedListener();

        /// Whether the shared pool has a node to give. Advisory only - the
        /// authoritative test is the guarded one at the append, since another
        /// driver can take the last node in between. Its job is to refuse
        /// before any hardware is touched.
        static bool listenerPoolFull();

        /// Raise the high-water mark to the pool's current level, and
        /// return the new mark if it moved or 0 if it did not.
        ///
        /// CALL WITH listenerPoolMutex() ALREADY HELD, in the same critical
        /// section as the take it is measuring. Reading the level after that
        /// lock is dropped lets a concurrent disconnect give a node back
        /// first, so the mark under-reports and can miss the pool being
        /// briefly full - which is the one reading worth having.
        ///
        /// Does not log. The caller does that once it has let the lock go.
        static size_t noteListenerPoolPeakLocked();

        /// Log a mark returned by noteListenerPoolPeakLocked(). Call with no
        /// pool lock held: that mutex already nests the heap lock and must
        /// not also nest the logger.
        static void logListenerPoolPeak(size_t peak);

        float applyPeriod(float requiredPeriod, uint32_t latency);
        float updatePeriod(float value, uint32_t latency);
        float renegotiatePeriod(const SDK::Interface::ISensorDataListener* listener,
                                float                                      requestedPeriod,
                                uint32_t                                   latency);
        void  setPeriod(float value);
        Sensor::DataQueue* findListenerWithMinPeriod();

        float calcUpdatePeriod(float period, uint32_t latency) const;

        uint32_t listenersLatency() const;
        void     applyLatency(uint32_t latency);

        Interface::ISensor&          mSensor;

        // Lock order (outermost first):
        //   mConnMutex -> control callbacks (sdcStart/sdcStop/sdcUpdatePeriod,
        //   which may nest into other drivers' connect/disconnect along the
        //   sub-sensor dependency chain) -> Sensor::Manager mutex.
        //
        // mConnMutex serialises the connection control plane (connect /
        // disconnect / period renegotiation) of this driver. mMutex is a LEAF:
        // it protects only mListeners access and must never be held across a
        // control callback or a Sensor::Manager call - holding it there
        // deadlocks against the manager's refresh sweep, which takes the
        // manager mutex first and then this mutex via pushDataSample().
        //
        // mConnMutex waits are UNBOUNDED (it is held across control callbacks
        // that do real chip I/O). Never call connect()/disconnect() from an
        // ISR, an OS timer callback, or from inside data delivery
        // (ISensorDataListener::onSdlNewData) - those contexts must not block
        // on the control plane.
        OS::Mutex                    mConnMutex;
        OS::Mutex                    mMutex;

        /// A Driver must not be destroyed while the system is running.
        /// ~ListenerList hands its nodes back to the shared pool without
        /// taking listenerPoolMutex(), so a teardown racing another driver's
        /// connect would corrupt the free list for every driver at once -
        /// far worse than anything this pool was introduced to avoid.
        ///
        /// Nothing does today: every Driver is a member of a sensor owned by
        /// the Instance::SensorLayer singleton, and lives until power off.
        /// That is a fact about the current arrangement rather than
        /// something the type enforces, so keep it true.
        ListenerList                 mListeners;
        SDK::Sensor::Type            mType;
        Sensor::ISensorDriverCtrl&   mControl;
        Sensor::DataSample           mDataSample;
        Sensor::DataQueue*           mNewConnectionListener;
        Mode                         mMode;
        float                        mRefreshPeriod;
        uint8_t                      mHandle;
        uint32_t                     mAppliedLatency;   ///< Last told to the source, 0 for none
        /// Has listeners. Set on the connection path and read by the manager's
        /// sweep; what it gates is ordered by the locks each side takes, not by
        /// this flag, so every access is relaxed.
        std::atomic<bool>            mActive;
        /// Nominal step of the source, us; 0 when it reports none. Standalone
        /// value with no ordering against anything else, so relaxed.
        std::atomic<uint32_t>        mSourceStepUs;
        /// Clock of a source sampled on the sweep, null for the others.
        SourceTick*                  mSourceTick;
        const SourceStepTrack*       mSourceSteps;

        // Developer-mode injection. The values are allocated on first use and
        // kept, guarded by mMutex.
        SDK::Sensor::Data::Field*    mInjectedValues;
        std::atomic<bool>            mInjected;
        bool                         mInjectPending;    ///< Not yet published
        uint32_t                     mInjectLastMs;
    }; /* class Driver */

} // namespace Sensor

#endif // SENSOR_DRIVER_HPP
