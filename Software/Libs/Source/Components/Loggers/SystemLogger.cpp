/**
 ******************************************************************************
 * @file    SystemLogger.cpp
 * @brief   Optional system-log drain: LogBuffer -> rolling session files.
 ******************************************************************************
 */

#define LOG_MODULE_PRX      "SystemLogger"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include "Loggers/SystemLogger.hpp"
#include "LogBuffer/LogBuffer.hpp"
#include "System.hpp"

#include <cstdio>
#include <cstring>

namespace Component {

namespace {
constexpr uint32_t kCursorMagic = 0x53595343u;   // "SYSC"

/// Started flag, readable without constructing the singleton (for shutdown).
bool sStarted = false;
}  // namespace

/**
 * @brief No-init drain cursor: the LogBuffer position already made durable.
 *
 * Committed only after an f_sync, so a crash/reset resumes exactly where the
 * file left off; the read-but-unflushed tail is still in the surviving
 * LogBuffer and gets re-drained - no loss, no duplication.
 */
typedef struct {
    uint32_t magic;
    uint64_t drained;
} SysLogCursor;

static __attribute__((section(".systemlog"))) __attribute__((used))
SysLogCursor sCursor;

SystemLogger& SystemLogger::GetInstance()
{
    static SystemLogger instance;
    return instance;
}

bool SystemLogger::isStarted()
{
    return sStarted;
}

void SystemLogger::queueMarker(const char* s)
{
    size_t n = std::strlen(s);
    if (n > sizeof(mMarker)) {
        n = sizeof(mMarker);
    }
    std::memcpy(mMarker, s, n);
    mMarkerLen = static_cast<uint32_t>(n);
    mMarkerPos = 0;
}

size_t SystemLogger::fill(uint8_t* dst, size_t maxLen)
{
    // 1. Pending inline marker (reboot / gap) takes priority to keep ordering.
    if (mMarkerPos < mMarkerLen) {
        size_t n = mMarkerLen - mMarkerPos;
        if (n > maxLen) {
            n = maxLen;
        }
        std::memcpy(dst, &mMarker[mMarkerPos], n);
        mMarkerPos += static_cast<uint32_t>(n);
        return n;
    }

    // 2. Detect overrun before reading; queue a gap marker and emit it first.
    const uint64_t total  = LogBuffer::totalWritten();
    const uint32_t cap    = LogBuffer::capacity();
    const uint64_t oldest = (total > cap) ? (total - cap) : 0;
    if (mReadPos < oldest) {
        char gap[48];
        std::snprintf(gap, sizeof(gap), "\n--- gap: %lu bytes lost ---\n",
                      static_cast<unsigned long>(oldest - mReadPos));
        mReadPos = oldest;
        queueMarker(gap);
        size_t n = (mMarkerLen < maxLen) ? mMarkerLen : maxLen;
        std::memcpy(dst, mMarker, n);
        mMarkerPos = static_cast<uint32_t>(n);
        return n;
    }

    // 3. LogBuffer data.
    size_t n = LogBuffer::read(dst, static_cast<uint32_t>(maxLen), &mReadPos, nullptr);
    if (n > 0) {
        return n;
    }

    // 4. Caught up: emit the shutdown marker once, after all data.
    if (mShutdownPending && !mShutdownDone) {
        mShutdownDone = true;
        static const char kShutdown[] = "\n--- shutdown ---\n";
        size_t m = sizeof(kShutdown) - 1;
        if (m > maxLen) {
            m = maxLen;
        }
        std::memcpy(dst, kShutdown, m);
        return m;
    }

    return 0;
}

size_t SystemLogger::fillTrampoline(void* ctx, uint8_t* dst, size_t maxLen)
{
    return static_cast<SystemLogger*>(ctx)->fill(dst, maxLen);
}

bool SystemLogger::hasData()
{
    // Mirror fill()'s "returns > 0" condition without consuming, so the writer
    // opens a segment only when there is something to write: a pending marker,
    // unread LogBuffer bytes (also covers a gap), or the one-shot shutdown marker.
    if (mMarkerPos < mMarkerLen) {
        return true;
    }
    if (mReadPos < LogBuffer::totalWritten()) {
        return true;
    }
    return mShutdownPending && !mShutdownDone;
}

bool SystemLogger::hasDataTrampoline(void* ctx)
{
    return static_cast<SystemLogger*>(ctx)->hasData();
}

void SystemLogger::onFlushed()
{
    // Everything read so far is now durable; advance the persisted cursor.
    sCursor.drained = mReadPos;
}

void SystemLogger::onFlushedTrampoline(void* ctx)
{
    static_cast<SystemLogger*>(ctx)->onFlushed();
}

void SystemLogger::notifyTrampoline(void* ctx)
{
    static_cast<SystemLogger*>(ctx)->mWriter.notifyData();
}

bool SystemLogger::start()
{
    if (mRunning) {
        return true;
    }

    if (!mLifecycle.init("SysLogLife")) {
        LOG_ERROR("failed to create the lifecycle lock\n");
        return false;
    }

    // Validate/seed the no-init cursor. On a cold boot (or layout change) start
    // from the oldest byte still in the ring; otherwise keep the survived value.
    if (sCursor.magic != kCursorMagic) {
        sCursor.magic   = kCursorMagic;
        sCursor.drained = LogBuffer::totalWritten() - LogBuffer::size();
    }
    mReadPos        = sCursor.drained;
    mShutdownPending = false;
    mShutdownDone    = false;

    // Reboot marker (with reset cause) leads the first segment.
    char marker[64];
    std::snprintf(marker, sizeof(marker), "\n--- reboot (%s) ---\n",
                  DRV_SYSTEM_RESET_TO_STR(Driver::System::GetResetReason()));
    queueMarker(marker);

    mNaming.configure(mFolderName, "syslog", Driver::System::GetDeviceIdStr(), "log", skKeepNewest);
    mNaming.captureSession();

    char firstPath[128];
    if (!mNaming.nextPath(firstPath, sizeof(firstPath))) {
        LOG_ERROR("failed to build the first session path\n");
        return false;
    }

    mWriter.setSource(&SystemLogger::fillTrampoline, this);
    mWriter.setAvailable(&SystemLogger::hasDataTrampoline);
    mWriter.setOnFlushed(&SystemLogger::onFlushedTrampoline, this);
    mWriter.setRollover(skMaxFileBytes, &Sink::SessionNaming::nextPathTrampoline, &mNaming);

    if (!mWriter.start(firstPath, true /* override */, skFlushIntervalMs)) {
        LOG_ERROR("failed to start the writer\n");
        return false;
    }

    mRunning = true;
    sStarted = true;
    LogBuffer::setNotify(&SystemLogger::notifyTrampoline, this);
    mWriter.notifyData();   // kick: drain the survived pre-reset tail now

    LOG_INFO("started\n");
    return true;
}

void SystemLogger::suspend()
{
    if (!mRunning) {
        return;     // never started: the lock is not set up either
    }
    OS::MutexCS cs(mLifecycle);
    if (!mRunning || mSuspended) {
        return;
    }

    LogBuffer::setNotify(nullptr, nullptr);
    mSuspended = true;

    // The writer drains the source before it closes, so everything logged up
    // to here is written and the cursor committed.
    mWriter.stop();
    LOG_INFO("suspended\n");
}

bool SystemLogger::resume()
{
    if (!mRunning) {
        return true;
    }
    OS::MutexCS cs(mLifecycle);
    if (!mRunning || !mSuspended) {
        return true;
    }

    char path[128];
    if (!mNaming.nextPath(path, sizeof(path))) {
        LOG_ERROR("failed to build the next segment path\n");
        return false;
    }
    if (!mWriter.start(path, true /* override */, skFlushIntervalMs)) {
        LOG_ERROR("failed to restart the writer\n");
        return false;
    }

    mSuspended = false;
    LogBuffer::setNotify(&SystemLogger::notifyTrampoline, this);
    mWriter.notifyData();   // kick: drain what was logged while suspended
    LOG_INFO("resumed\n");
    return true;
}

void SystemLogger::stop()
{
    if (!mRunning) {
        return;
    }
    // A shutdown (Power) can stop the logger while the Backend suspends it.
    OS::MutexCS cs(mLifecycle);
    if (!mRunning) {
        return;
    }

    LogBuffer::setNotify(nullptr, nullptr);
    mRunning = false;

    if (mSuspended) {
        // The writer is stopped and its file closed already. What was logged
        // since stays in the LogBuffer, behind the persisted cursor.
        mSuspended = false;
        sStarted   = false;
        return;
    }

    // Ask the source to append a shutdown marker after the last data, then stop:
    // the writer's shutdown drain pulls the remaining buffer + marker, f_syncs
    // (final onFlushed commits the cursor) and closes the file cleanly.
    mShutdownPending = true;
    mWriter.stop();

    sStarted = false;
}

}  // namespace Component
