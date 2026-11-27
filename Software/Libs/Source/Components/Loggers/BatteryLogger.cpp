/**
 ******************************************************************************
 * @file    BatteryLogger.cpp
 * @brief   Battery telemetry logger - self-contained service.
 *
 * @details
 * All I2C and file I/O happens on the logger's own worker thread. Event
 * sources (Power::StatusCallback overrides, the internal heartbeat timer)
 * post a Reason onto the queue; the thread is the single consumer and the
 * only place that runs writeRecord().
 *
 ******************************************************************************/
#include "Loggers/BatteryLogger.hpp"
#include "System.hpp"

#define LOG_MODULE_PRX      "BatteryLogger"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include <ctime>

namespace Component {

const char* BatteryLogger::reasonToStr(Reason r)
{
    switch (r) {
        case Reason::PERIODIC:    return "periodic";
        case Reason::USB_ON:       return "usb_on";
        case Reason::USB_OFF:      return "usb_off";
        case Reason::CHARGE_START: return "charge_start";
        case Reason::CHARGE_TERM:  return "charge_term";
        case Reason::POR:         return "por";
        case Reason::BOOT:        return "boot";
        case Reason::SHUTDOWN:    return "shutdown";
        case Reason::DRIVE_OPEN:   return "drive_open";
        case Reason::DRIVE_CLOSED: return "drive_closed";
    }
    return "unknown";
}

BatteryLogger::~BatteryLogger()
{
    // Guarantee a clean teardown even if the owner just resets the
    // unique_ptr without an explicit deinit() - otherwise the Power
    // subscription would outlive us and call into freed memory.
    deinit();
}

BatteryLogger::BatteryLogger()
    : mLogger(mHeader)
    , mPmic(Pmic::GetInstance())
    , mFuelGauge(FuelGauge::GetInstance())
{
    mLogger.setFlushInterval(skFlushIntervalMs);
}

void BatteryLogger::init(uint32_t periodSec)
{
    if (mThread.getState() == OS::__Thread::State::Running) {
        LOG_WARNING("Already running\n");
        return;
    }
    if (periodSec == 0) {
        LOG_ERROR("Invalid period\n");
        return;
    }
    mPeriodSec = periodSec;

    // Bring up only the infrastructure the thread needs to exist: queue,
    // timer object (not started), and the thread itself last. The thread
    // does the rest (rotation, file open, Power subscription, timer start)
    // in its own context.
    if (!mQueue.init(LOG_MODULE_PRX)) {
        LOG_ERROR("Queue init failed\n");
        return;
    }
    if (!mPeriodicTimer.init(LOG_MODULE_PRX, this, &BatteryLogger::periodicTick,
                             true /* periodic */, nullptr))
    {
        LOG_ERROR("Timer init failed\n");
        mQueue.deinit();
        return;
    }

    // Blocking init: returns true only after the thread reached
    // ThreadInitGuard::success() in its body, i.e. file and subscription
    // are up. False on osThreadNew failure or setup error inside the thread.
    constexpr uint32_t kInitTimeoutMs = 2000;
    if (!mThread.init(LOG_MODULE_PRX, this, &BatteryLogger::thread,
                      osPriorityNormal, kInitTimeoutMs))
    {
        LOG_ERROR("Thread init failed\n");
        mPeriodicTimer.deinit();
        mQueue.deinit();
        return;
    }

    LOG_INFO("started, period %u s\n", periodSec);
}

void BatteryLogger::deinit()
{
    using ThreadState = OS::__Thread::State;
    const ThreadState s = mThread.getState();

    if (s == ThreadState::Idle) {
        // init() was never called or its failure paths already cleaned up
        // queue + timer. Nothing left here.
        return;
    }

    // If the thread is still alive, signal it to exit. It writes a final
    // record, stops the timer, detaches from Power and closes the file
    // inside its own body before returning - join() then unblocks.
    if (s == ThreadState::Starting || s == ThreadState::Running || s == ThreadState::Stopping) {
        mQueue.push(Reason::SHUTDOWN);
        mThread.join();
    }

    // Release OS-level handles. These are no-ops on already-released handles,
    // which covers the Stopped / Failed cases where the thread is gone but
    // we still own the queue and timer objects.
    mThread.deinit();
    mPeriodicTimer.deinit();
    mQueue.deinit();

    LOG_INFO("stopped\n");
}

bool BatteryLogger::suspend()
{
    if (mThread.getState() != OS::__Thread::State::Running) {
        return false;
    }

    constexpr uint32_t kTimeoutMs = 2000;
    if (!mQueue.push(Reason::DRIVE_OPEN, kTimeoutMs)) {
        LOG_ERROR("suspend dropped (queue full)\n");
        return false;
    }
    // Requests are handled in order, so this one is done once this many are.
    const uint32_t mine = ++mDriveOpenQueued;

    // A request that timed out earlier can be handled (and given) at any
    // point, so each give only prompts another look at the count.
    // FreeRTOS ticks, which wrap cleanly modulo 2^32 (GetTimeMs() does not).
    const TickType_t start = xTaskGetTickCount();
    while (static_cast<int32_t>(mDriveOpenDone.load() - mine) < 0) {
        const uint32_t waited = (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        if (waited >= kTimeoutMs || !mDriveOpenHandled.take(kTimeoutMs - waited)) {
            LOG_ERROR("suspend timed out\n");
            return false;
        }
    }
    return true;
}

void BatteryLogger::resume()
{
    if (mThread.getState() != OS::__Thread::State::Running) {
        return;
    }

    // The flag, not the record, is what reopens the file: if the queue is full
    // the record is lost, and the next record of any kind reopens it.
    mReopenPending = true;
    constexpr uint32_t kTimeoutMs = 2000;
    if (!mQueue.push(Reason::DRIVE_CLOSED, kTimeoutMs)) {
        LOG_WARNING("drive_closed record dropped (queue full)\n");
    }
}

void BatteryLogger::onUsbDetected(bool state)
{
    mQueue.push(state ? Reason::USB_ON : Reason::USB_OFF, 0);
}

void BatteryLogger::onCharging(bool state)
{
    mQueue.push(state ? Reason::CHARGE_START : Reason::CHARGE_TERM, 0);
}

void BatteryLogger::periodicTick(void* /*arg*/)
{
    mQueue.push(Reason::PERIODIC, 0);
}

void BatteryLogger::thread()
{
    OS::ThreadInitGuard initGuard(mThread);

    // Setup (thread-owned). On any failure we just return; initGuard's
    // destructor calls setInitFailed() so the caller's mThread.init() reports
    // failure and can clean up the queue/timer it created.
    // Configure the session file (battery_<id>_<NNNN>_<ts>.csv) and retention,
    // then open. The directory scan + prune runs inside open() on this thread.
    mLogger.setNaming(mFolderName, "battery", Driver::System::GetDeviceIdStr());
    mLogger.setRetention(skKeepNewest);
    mLogger.setMaxFileBytes(skMaxFileBytes);
    if (!mLogger.open()) {
        LOG_ERROR("Logger open failed\n");
        return;
    }
    Power::GetInstance().attachStatusCallback(this);
    if (!mPeriodicTimer.start(mPeriodSec * 1000)) {
        Power::GetInstance().detachStatusCallback(this);
        mLogger.close();
        return;
    }

    initGuard.success();   // state -> Running; caller's init() unblocks with true

    // Seed initial sample(s) so the file starts with current state.
    // POR is logged when this firmware boot found the gauge freshly reset
    // (battery had been disconnected); the live hardware flag is cleared in
    // FuelGauge::init() so we have to ask the sticky captured value.
    writeRecord(Reason::BOOT);
    if (mFuelGauge.wasPowerOnReset()) {
        writeRecord(Reason::POR);
    }

    // Main loop.
    while (true) {
        Reason r;
        if (!mQueue.pop(r)) {
            continue;
        }

        // Not on DRIVE_OPEN: one whose suspend() timed out can come after the
        // resume() that set the flag, and would take it, reopen the file and
        // close it again, leaving nothing to open it after the session.
        if (r != Reason::DRIVE_OPEN && mReopenPending.exchange(false)) {
            // Closed for DRIVE_OPEN, or left open with an object the volume's
            // remount invalidated if that suspend failed: start a new file
            // either way.
            mLogger.close();
            if (!mLogger.open()) {
                LOG_ERROR("Logger reopen failed\n");
            }
        }

        writeRecord(r);

        if (r == Reason::DRIVE_OPEN) {
            mLogger.close();
            ++mDriveOpenDone;
            mDriveOpenHandled.give();
            continue;
        }

        if (r == Reason::SHUTDOWN) {
            break;
        }
    }

    // Teardown (thread-owned, mirror of setup in reverse).
    mPeriodicTimer.stop();
    Power::GetInstance().detachStatusCallback(this);
    mLogger.close();
}

void BatteryLogger::writeRecord(Reason reason)
{
    if (!mLogger.isOpened()) {
        return;
    }

    Hardware::MAX17262::Telemetry t;
    if (!mFuelGauge.getTelemetry(t)) {
        return;
    }

    mLogger.startRecord();

    mLogger.append(static_cast<uint32_t>(time(nullptr)));
    mLogger.append(mSeq++);
    mLogger.append(static_cast<uint32_t>(Driver::System::GetTimeMs()));
    mLogger.append(reasonToStr(reason));
    mLogger.append(mPmic.isBatteryDetected());
    mLogger.append(mPmic.isUsbDetected());
    mLogger.append(mPmic.isCharging());
    mLogger.append(t.vCell,      3);
    mLogger.append(t.avgVCell,   3);
    mLogger.append(t.current,    3);
    mLogger.append(t.avgCurrent, 3);
    mLogger.append(t.repSoc,     3);
    mLogger.append(t.fullCapRep, 3);
    mLogger.append(t.fullCapNom, 3);
    mLogger.append(t.qResidual,  3);
    mLogger.append(t.qh,         3);
    mLogger.append(t.cycles);
    mLogger.appendHex(t.status);
    mLogger.appendHex(t.fStat);
    mLogger.appendHex(t.rComp0);
    mLogger.appendHex(t.tempCo);
    mLogger.appendHex(t.dQAcc);
    mLogger.appendHex(t.dPAcc);

    // Flush events (boot, POR, USB, charge transitions, shutdown) immediately so
    // they survive a power loss inside the flush interval; periodic samples stay
    // coalesced to spare the flash.
    mLogger.stopRecord(reason != Reason::PERIODIC);
}

} // namespace Component
