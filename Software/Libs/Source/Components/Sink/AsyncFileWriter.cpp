/**
 ******************************************************************************
 * @file    AsyncFileWriter.cpp
 * @brief   Base async file writer: worker thread, flush policy, size rollover.
 ******************************************************************************
 */

#include "Sink/AsyncFileWriter.hpp"

#define LOG_MODULE_PRX      "AsyncFileWriter"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include "FileSystem.hpp"
#include "System.hpp"

#include <cstdio>
#include <cstring>

namespace Sink {

AsyncFileWriter::~AsyncFileWriter()
{
    // Backstop only. Subclasses must stop() in their own destructor so the
    // worker stops touching produce() before the subclass part is destroyed.
    stop(kWaitForever);
}

void AsyncFileWriter::setRollover(uint32_t maxSegmentBytes, NextPathFn nextPath, void* ctx)
{
    mMaxSegmentBytes = maxSegmentBytes;
    mNextPath        = nextPath;
    mNextPathCtx     = ctx;
}

void AsyncFileWriter::setOnFlushed(EventFn fn, void* ctx)
{
    mOnFlushed    = fn;
    mOnFlushedCtx = ctx;
}

void AsyncFileWriter::setOnSegmentOpened(EventFn fn, void* ctx)
{
    mOnSegmentOpened    = fn;
    mOnSegmentOpenedCtx = ctx;
}

bool AsyncFileWriter::start(const char* path, bool override, uint32_t flushIntervalMs)
{
    if (path == nullptr) {
        LOG_ERROR("Invalid parameter\n");
        return false;
    }

    // A worker a stop() left busy owns the thread object (and mPath) until it
    // exits: not before then.
    if (mStopPending) {
        if (!mThread.join(0)) {
            LOG_ERROR("previous worker still busy: %s\n", mPath);
            return false;
        }
        mThread.deinit();
        mStopPending = false;
    }

    std::snprintf(mPath, sizeof(mPath), "%s", path);
    mOverride        = override;
    mFlushIntervalMs = flushIntervalMs;

    if (!mThread.init("FileWriter", this, &AsyncFileWriter::thread, osPriorityNormal, 1000)) {
        LOG_ERROR("thread init failed: %s\n", mPath);
        return false;
    }

    return true;
}

bool AsyncFileWriter::stop(uint32_t timeoutMs)
{
    if (mThread.getId() == nullptr) {
        return true;    // never started, or stopped already
    }

    mThread.requestStop();
    mWork.give();          // wake the worker so it observes the stop request

    // Never killed on a timeout (see the header): left to exit on its own.
    constexpr uint32_t kRoundMs = 5000;
    while (!mThread.join(timeoutMs == kWaitForever ? kRoundMs : timeoutMs)) {
        if (timeoutMs != kWaitForever) {
            LOG_ERROR("worker still busy, left to finish: %s\n", mPath);
            mStopPending = true;
            return false;
        }
        LOG_WARNING("still waiting for the worker: %s\n", mPath);
    }

    mThread.deinit();
    mStopPending = false;
    return true;
}

bool AsyncFileWriter::isRunning() const
{
    return mThread.getState() == OS::__Thread::State::Running;
}

bool AsyncFileWriter::writeDirect(const void* data, size_t size)
{
    if (!mFile.isOpen() || data == nullptr || size == 0) {
        return false;
    }

    size_t bw = 0;
    const bool ok = mFile.write(static_cast<const char*>(data), size, bw);
    mBytesInSegment += static_cast<uint32_t>(bw);
    return ok && bw == size;
}

void AsyncFileWriter::notifyData()
{
    if (xPortIsInsideInterrupt()) {
        BaseType_t woken = pdFALSE;
        mWork.giveFromISR(&woken);
        portYIELD_FROM_ISR(woken);
    } else {
        mWork.give();
    }
}

void AsyncFileWriter::flushIfDue(uint32_t& lastFlushTick, bool force)
{
    if (!mFile.isOpen()) {
        return;
    }

    const uint32_t now = Driver::System::GetTimeMs();
    const bool due = force
        || mFlushIntervalMs == 0
        || (now - lastFlushTick) >= mFlushIntervalMs
        || mFlushRequested.exchange(false, std::memory_order_relaxed);

    if (due) {
        mFile.flush();
        lastFlushTick = now;
        if (mOnFlushed) {
            mOnFlushed(mOnFlushedCtx);
        }
    }
}

void AsyncFileWriter::rolloverIfNeeded()
{
    if (mMaxSegmentBytes == 0 || mBytesInSegment < mMaxSegmentBytes || mNextPath == nullptr) {
        return;
    }

    char next[sizeof(mPath)];
    if (!mNextPath(mNextPathCtx, next, sizeof(next))) {
        return;   // policy chose to keep the current file
    }

    // Flush+close the full segment; the next one is opened lazily on its first
    // byte (openSegment), so a rollover boundary never leaves an empty file.
    if (mFile.isOpen()) {
        mFile.flush();
        if (mOnFlushed) {
            mOnFlushed(mOnFlushedCtx);
        }
        LOG_INFO("Stopped write to file: %s\n", mFile.getPath());
        mFile.close();
    }

    mFile.setPath(next);
    mBytesInSegment = 0;
}

bool AsyncFileWriter::openSegment(uint32_t& lastFlushTick)
{
    if (!mFile.open(true, mOverride)) {
        LOG_ERROR("Failed to open segment: %s\n", mFile.getPath());
        return false;
    }

    if (!mOverride) {
        // Only the first segment may append to an existing file; continue at its end.
        mFile.seek(mFile.size());
        mBytesInSegment = mFile.size();
    } else {
        mBytesInSegment = 0;
    }
    mOverride     = true;   // rolled/reopened segments always start fresh
    lastFlushTick = Driver::System::GetTimeMs();

    LOG_INFO("Started write to file: %s\n", mFile.getPath());
    if (mOnSegmentOpened) {
        mOnSegmentOpened(mOnSegmentOpenedCtx);
    }
    return true;
}

void AsyncFileWriter::drainOnce(uint32_t& lastFlushTick)
{
    // The worker is woken per push (write/notifyData) and on stop, so a wake can
    // find the source empty (semaphore over-signal, or stop). sourceHasData()
    // peeks without consuming, so the segment is opened only when there is a byte
    // to write (no empty file ever) and openSegment() runs BEFORE produce() (a
    // failed open leaves the bytes in the source, retried on the next wake).
    //
    // Pull sources that cannot be peeked keep the default sourceHasData()==true:
    // they may briefly open an empty segment in the window right after a rollover
    // (file closed) if the next wake carries no data - a rare, harmless 0-byte file.
    while (sourceHasData()) {
        if (!mFile.isOpen()) {
            if (!openSegment(lastFlushTick)) {
                return;
            }
        }

        size_t n = produce(reinterpret_cast<uint8_t*>(mBuff), sizeof(mBuff));
        if (n == 0) {
            break;
        }
        size_t bw = 0;
        mFile.write(mBuff, n, bw);
        mBytesInSegment += static_cast<uint32_t>(n);

        flushIfDue(lastFlushTick, false);
        rolloverIfNeeded();
    }
}

void AsyncFileWriter::thread()
{
    OS::ThreadInitGuard initGuard(mThread);

    char localPath[sizeof(mPath)];
    std::snprintf(localPath, sizeof(localPath), "%s", mPath);
    mFile.setPath(localPath);

    // Create the directory. The file is opened lazily on the first byte
    // (openSegment), so a writer that never gets data leaves no empty file.
    char dir[sizeof(mPath)];
    std::snprintf(dir, sizeof(dir), "%s", localPath);
    char* end = std::strrchr(dir, '/');
    if (end == nullptr) {
        return;
    }
    *(end + 1) = '\0';
    if (!FS::FileSystem::GetInstance().mkdir(dir)) {
        return;
    }

    mBytesInSegment = 0;
    initGuard.success();

    uint32_t lastFlushTick = Driver::System::GetTimeMs();

    // Event-driven loop: sleep on the work semaphore (given by write()/notifyData()/
    // stop()), then drain the source completely before sleeping again.
    while (!mThread.shouldStop()) {
        mWork.take();
        drainOnce(lastFlushTick);
    }

    // Drain whatever is left, make it durable, close cleanly.
    drainOnce(lastFlushTick);
    if (mFile.isOpen()) {
        mFile.flush();
        if (mOnFlushed) {
            mOnFlushed(mOnFlushedCtx);
        }
        mFile.close();
    }

    LOG_INFO("Stopped write to file: %s\n", mFile.getPath());
}

}  // namespace Sink
