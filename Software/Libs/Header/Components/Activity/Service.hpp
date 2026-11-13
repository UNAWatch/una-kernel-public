/**
 ******************************************************************************
 * @file    Service.hpp
 * @brief   Activity service - owns the FSM and drives it on its own worker.
 *
 *          Collaborates with:
 *           - FeatureCenter  - feature trackers (HR, motion, steps,
 *                              touch, activity duration accumulator).
 *                              The FSM consumes it through
 *                              IFeatureSource; Service also queries
 *                              ActivityTracker's running total after
 *                              each FSM tick to notify duration
 *                              listeners.
 *           - ISensorControl - sensor (de)activation the FSM issues.
 *
 *          Threading model:
 *           - One internal worker task. The only command the worker
 *             handles is FSM_TICK.
 *           - The FSM is ticked on a periodic ~30 s timer plus out-of-
 *             cadence ticks via IFsmTrigger::requestFsmTick() (typically
 *             from touch/motion rising edges in FeatureCenter).
 *           - After every FSM tick the worker reads the current
 *             activity duration; if it has advanced, attached
 *             IDurationListener callbacks fire on the worker thread.
 ******************************************************************************
 */

#ifndef COMPONENT_ACTIVITY_SERVICE_HPP
#define COMPONENT_ACTIVITY_SERVICE_HPP

#include <cstdint>

#include "OS.hpp"
#include "Observer.hpp"

#include "Activity/Fsm.hpp"
#include "Activity/FeatureCenter.hpp"
#include "Activity/FsmInterfaces.hpp"
#include "Activity/IFsmTrigger.hpp"
#include "Activity/IDurationListener.hpp"

namespace Activity
{

class Service : public IFsmTrigger
{
public:
    Service(FeatureCenter& features, ISensorControl& sensors);
    ~Service();

    bool init();
    bool deinit();
    bool isInitialized() const;

    /// IFsmTrigger - wake the FSM out of cadence (e.g. on touch / motion
    /// edges from the data layer). Safe from any task.
    void requestFsmTick() override;

    /// Subscribe a listener to monotonic-from-boot activity-minute
    /// changes. Notified on the Service worker thread after each FSM
    /// tick, only when the value advances.
    void attachDurationListener(IDurationListener* listener);
    void detachDurationListener(IDurationListener* listener);

private:
    /// Activity FSM tick period (ms).
    static constexpr uint32_t skFsmTickMs = 30u * 1000u;

    struct Cmd
    {
        enum class Id : uint8_t
        {
            NONE = 0,
            FSM_TICK,
        };
        Id id;
    };

    FeatureCenter& mFeatures;
    Fsm            mFsm;

    OS::Queue<Cmd, 4>             mCmdQueue{};
    OS::Thread<Service, 3 * 1024> mThread{};
    OS::Timer<Service>            mFsmTimer{};

    /// Observer list for monotonic activity-duration changes.
    Observer::Holder<IDurationListener, 4> mDurationListeners{};

    /// Last notified value; used to suppress no-change notifications.
    uint32_t mLastDuration = 0;

    /// Last notified wear state (FSM in a worn state). Starts false to
    /// match the FSM, which boots through IDLE and UNWORN.
    bool mLastWorn = false;

    void thread();
    void fsmTimerCallback(void* arg);

    bool postCmd(Cmd::Id id);
};

} // namespace Activity

#endif // COMPONENT_ACTIVITY_SERVICE_HPP
