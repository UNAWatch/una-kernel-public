/**
 ******************************************************************************
 * @file    SystemLogger.hpp
 * @brief   Optional system-log drain: LogBuffer -> rolling session files.
 *
 * Threadless: a PullFileWriter's own worker pulls straight from the no-init
 * LogBuffer (no second thread, no second queue). The persisted drain cursor is
 * committed only after each f_sync (onFlushed), so a reset never loses the
 * read-but-unflushed tail - it is re-drained from the surviving LogBuffer.
 *
 * UNA-only and optional (disabled by default; gated by a debug setting). Not
 * part of the portable LogBuffer+Crash core.
 ******************************************************************************
 */

#ifndef COMPONENT_LOGGERS_SYSTEM_LOGGER_HPP
#define COMPONENT_LOGGERS_SYSTEM_LOGGER_HPP

#include <cstddef>
#include <cstdint>

#include "main.h"
#include "Sink/PullFileWriter.hpp"
#include "Sink/SessionNaming.hpp"

namespace Component {

/**
 * @brief System-log drainer (singleton, threadless).
 */
class SystemLogger {
public:
    static SystemLogger& GetInstance();

    /** @brief Configure the writer, hook the LogBuffer notify, start draining. */
    bool start();

    /** @brief Flush, write the shutdown marker and close the session file. */
    void stop();

    /**
     * @brief Write out what the LogBuffer holds and close the file, keeping
     *        the session. Lines logged from here on wait in the LogBuffer
     *        (a gap marker covers any it drops) until resume().
     * @return false if the writer is still in a file call after the stop
     *         timeout (it is left to finish, and may still write until it has;
     *         call again to wait for it again); true once it has finished, or
     *         if not running.
     */
    bool suspend();

    /**
     * @brief Carry on draining into a new segment of the session.
     * @return false if the segment could not be named or the writer not
     *         restarted (still suspended: call again); true otherwise,
     *         including when there was nothing to resume.
     */
    bool resume();

    bool isRunning() const { return mRunning; }

    /** @brief Whether started, without constructing the singleton (shutdown use). */
    static bool isStarted();

    SystemLogger(const SystemLogger&)            = delete;
    SystemLogger& operator=(const SystemLogger&) = delete;

private:
    SystemLogger() = default;

    // Pull source (worker context): pending marker bytes, then LogBuffer data,
    // then the one-shot shutdown marker once the buffer is drained.
    size_t fill(uint8_t* dst, size_t maxLen);
    static size_t fillTrampoline(void* ctx, uint8_t* dst, size_t maxLen);

    // Peek for the writer: whether fill() would currently return >0 (no consume).
    bool hasData();
    static bool hasDataTrampoline(void* ctx);

    // Commit the persisted cursor after an f_sync (worker context).
    void onFlushed();
    static void onFlushedTrampoline(void* ctx);

    // LogBuffer "not empty" notify -> wake the writer (any context, ISR-safe).
    static void notifyTrampoline(void* ctx);

    void queueMarker(const char* s);

    // Session naming + retention/rollover policy. Files are named
    // <dir>syslog_<id>_<NNNN>_<ts>.log; keep the newest skKeepNewest sessions
    // (oldest pruned by NNNN) and roll the live file to a new segment at
    // skMaxFileBytes.
    static constexpr char     mFolderName[]     = SYSTEM_LOG_DIR_PATH;
    static constexpr uint32_t skKeepNewest      = 10;
    static constexpr uint32_t skMaxFileBytes    = 1024 * 1024;
    static constexpr uint32_t skFlushIntervalMs = 1000;

    Sink::PullFileWriter mWriter;
    Sink::SessionNaming  mNaming;

    uint64_t mReadPos        = 0;     // volatile LogBuffer read position
    char     mMarker[64]     = {};
    uint32_t mMarkerLen      = 0;
    uint32_t mMarkerPos      = 0;
    bool     mShutdownPending = false;
    bool     mShutdownDone    = false;
    bool     mRunning         = false;
    bool     mSuspended       = false;

    // Serialises stop(), suspend() and resume(): a shutdown stops the logger
    // from the Power thread, the drive suspends it from the Backend's.
    OS::Mutex mLifecycle;
};

}  // namespace Component

#endif  // COMPONENT_LOGGERS_SYSTEM_LOGGER_HPP
