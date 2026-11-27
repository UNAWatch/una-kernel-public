/**
 ******************************************************************************
 * @file    BatteryLogger.hpp
 * @brief   Battery telemetry logger - self-contained service.
 *
 * @details
 * Owns its worker thread, event queue and periodic heartbeat timer. The
 * thread is the only context that touches the gauge over I2C and writes the
 * file. External event sources do not call into the logger directly: they
 * arrive via the Power::StatusCallback overrides, which just post a Reason
 * onto the queue.
 *
 ******************************************************************************
 */

#ifndef COMPONENT_LOGGING_BATTERY_LOGGER_HPP
#define COMPONENT_LOGGING_BATTERY_LOGGER_HPP

#include "main.h"
#include "Pmic.hpp"
#include "FuelGauge.hpp"
#include "Power.hpp"
#include "OS.hpp"
#include "Loggers/CSVLogger.hpp"

#include <atomic>
#include <string>

namespace Component {

class BatteryLogger : public Power::StatusCallback
{
public:
    BatteryLogger();
    ~BatteryLogger() override;

    /**
     * @brief Start logging with the given heartbeat period.
     *
     * Blocking. On return the worker thread is running, a fresh session file
     * battery_<id>_<NNNN>_<YYYYMMDDTHHMMSS>.csv is open with the current
     * header, and Power callbacks are subscribed.
     *
     * @param periodSec heartbeat period in seconds. Must be > 0.
     */
    void init(uint32_t periodSec);

    /**
     * @brief Stop logging and tear everything down.
     *
     * Blocking. Unsubscribes from event sources, stops the periodic timer,
     * lets the worker write a final "shutdown" record, joins the thread,
     * and closes the file. Safe to call when not running.
     */
    void deinit();

    /**
     * @brief Write a "drive_open" record and close the file.
     *
     * Blocking: returns once the file is closed. Records due before resume()
     * are dropped. For while the volume is handed to the USB host.
     *
     * @return true once the file is closed, false if the worker did not get
     *         to it in time (or the logger is not running).
     */
    bool suspend();

    /**
     * @brief Open a new session file and write a "drive_closed" record.
     *
     * Returns at once; the worker does the work.
     */
    void resume();

    // Power::StatusCallback overrides. Invoked from the Power-module context;
    // they just post a Reason onto the queue so the heavy work runs on the
    // logger's own thread.
    void onUsbDetected(bool state) override;
    void onCharging(bool state) override;

private:
    /**
     * @brief Reason why a sample was logged. Goes into the Trigger column.
     *        Shutdown is also the signal that tells the worker thread to
     *        exit; it is queued from deinit() and never from a callback.
     */
    enum class Reason : uint8_t {
        PERIODIC = 0,
        USB_ON,
        USB_OFF,
        CHARGE_START,
        CHARGE_TERM,
        POR,
        BOOT,
        SHUTDOWN,
        DRIVE_OPEN,     // the worker closes the file after this record
        DRIVE_CLOSED,   // first record of the new file resume() asks for
    };
    static const char* reasonToStr(Reason r);

    static constexpr char     mHeader[]          = "Time,Seq,UptimeMs,Trigger,Batt Detect,USB Detected,Is Charging,VCell,AvgVCell,Current,AvgCurrent,RepSOC,FullCapRep,FullCapNom,QResidual,QH,Cycles,Status,FStat,RComp0,TempCo,dQAcc,dPAcc";
    static constexpr char     mFolderName[]      = BATTERY_LOG_DIR_PATH;
    static constexpr uint32_t skFlushIntervalMs  = 60000;
    static constexpr size_t   skQueueSize        = 16;
    static constexpr size_t   skThreadStackBytes = 2*2048;
    // Retention: keep the newest skKeepNewest session files (oldest pruned by
    // NNNN), and roll the current file to a new segment at skMaxFileBytes. This
    // bounds the BatteryLog directory while keeping each file easy to offload.
    static constexpr uint32_t skKeepNewest       = 10;
    static constexpr uint32_t skMaxFileBytes     = 1024 * 1024;

    void thread();
    void periodicTick(void* arg);
    void writeRecord(Reason reason);

    CSVLogger   mLogger;
    Pmic&       mPmic;
    FuelGauge&  mFuelGauge;

    OS::Queue<Reason, skQueueSize>                mQueue;
    OS::Thread<BatteryLogger, skThreadStackBytes> mThread;
    OS::Timer<BatteryLogger>                      mPeriodicTimer;

    // DRIVE_OPEN requests queued and handled, and a give per one handled, so
    // suspend() can tell its own request's handling from a late earlier one.
    std::atomic<uint32_t>                         mDriveOpenQueued{0};
    std::atomic<uint32_t>                         mDriveOpenDone{0};
    OS::Semaphore                                 mDriveOpenHandled{1, 0};

    // Set by resume(), taken by the worker on its next wake: a new file is
    // owed even if the DRIVE_CLOSED record could not be queued.
    std::atomic<bool>                             mReopenPending{false};

    // Monotonic record counter, resets to 0 on boot. Combined with the file's
    // sequence-numbered name this gives an unambiguous ordering even when the
    // RTC is invalid (GPS bad year, battery removed, etc.).
    uint32_t mSeq       = 0;

    // Heartbeat period passed into init(); read by the worker thread once
    // when it starts the periodic timer.
    uint32_t mPeriodSec = 0;
};

} // namespace Component

#endif // COMPONENT_LOGGING_BATTERY_LOGGER_HPP
