/**
 ******************************************************************************
 * @file    Service.cpp
 * @brief   Activity service implementation.
 ******************************************************************************
 */

#include "Activity/Service.hpp"

#define LOG_MODULE_PRX      "Activity.Service"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

namespace Activity
{

Service::Service(FeatureCenter& features, ISensorControl& sensors)
    : mFeatures(features)
    , mFsm(features, sensors)
{
    mCmdQueue.init(LOG_MODULE_PRX);
    mFsmTimer.init(LOG_MODULE_PRX, this, &Service::fsmTimerCallback,
                   /*periodic*/ true, this);
}

Service::~Service()
{
    deinit();
}

bool Service::init()
{
    return mThread.init("Activity", this, &Service::thread,
                        osPriorityNormal, 5000);
}

bool Service::deinit()
{
    if (!mThread.isInitialized()) {
        return true;
    }

    mThread.deinit();
    return true;
}

bool Service::isInitialized() const
{
    return mThread.isInitialized();
}

void Service::requestFsmTick()
{
    // The cmd queue carries at most one outstanding FSM_TICK in steady
    // state (the worker pops it before scheduling another); a burst of
    // edges therefore collapses into a single extra tick.
    postCmd(Cmd::Id::FSM_TICK);
}

void Service::attachDurationListener(IDurationListener* listener)
{
    mDurationListeners.add(listener);
}

void Service::detachDurationListener(IDurationListener* listener)
{
    mDurationListeners.del(listener);
}

void Service::thread()
{
    mThread.setInitialized();

    OS::CancellableGuard guard(&mThread, &mCmdQueue);

    LOG_INFO("Initialized\n");

    // Clean state + periodic ticks. The very first tick is fired
    // through the queue immediately so the FSM does not stay in IDLE
    // for a full skFsmTickMs after boot.
    mFsm.reset();
    mFsmTimer.start(skFsmTickMs);
    postCmd(Cmd::Id::FSM_TICK);

    while (!mThread.shouldStop()) {
        Cmd cmd{};
        mCmdQueue.pop(cmd);

        if (mThread.shouldStop()) {
            break;
        }

        if (cmd.id == Cmd::Id::FSM_TICK) {
            mFsm.tick();

            // Tell listeners when the watch goes on or comes off the
            // wrist, as judged by the FSM, so they can stop trusting
            // wrist-only readings (HR) the moment it comes off.
            const Fsm::State st = mFsm.current();
            const bool worn = (st != Fsm::State::IDLE &&
                               st != Fsm::State::UNWORN);
            if (worn != mLastWorn) {
                mLastWorn = worn;
                mDurationListeners.notify(
                    &IDurationListener::onWornChanged, worn);
            }

            // Pull the current activity-minute total and notify
            // listeners on this worker thread whenever it advances.
            // ActivityTracker integrates time inside the call, so
            // ticking here is also the cadence at which Activity's
            // running total moves forward.
            const uint32_t cur = mFeatures.currentActivityDuration();
            if (cur != mLastDuration) {
                mLastDuration = cur;
                mDurationListeners.notify(
                    &IDurationListener::onActivityDurationChanged, cur);
            }
        }
    }

    LOG_INFO("Deinitialized\n");
}

void Service::fsmTimerCallback(void* /*arg*/)
{
    postCmd(Cmd::Id::FSM_TICK);
}

bool Service::postCmd(Cmd::Id id)
{
    Cmd cmd{};
    cmd.id = id;
    return mCmdQueue.push(cmd, 100);
}

} // namespace Activity
