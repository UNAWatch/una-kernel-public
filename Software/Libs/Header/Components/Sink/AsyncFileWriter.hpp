/**
 ******************************************************************************
 * @file    AsyncFileWriter.hpp
 * @brief   Base async file writer: worker thread, flush policy, size rollover.
 *
 * Owns everything common to a background file writer - the worker thread, the
 * file handle, the flush (f_sync) policy and size-based segment rollover. The
 * only variation point is where the bytes come from: subclasses implement
 * @ref produce(), fed either by a queue (PushFileWriter) or by a pull source
 * (PullFileWriter).
 *
 * Naming on rollover is delegated outward via a @ref NextPathFn callback, so
 * this class stays free of any session/naming policy (see Sink::SessionNaming).
 *
 * @warning produce() is called from the worker thread and is pure virtual, so
 *          every subclass MUST call stop() in its own destructor - otherwise the
 *          worker may dispatch produce() while the subclass is already destroyed
 *          (a pure-virtual call). The base destructor calls stop() only as a
 *          backstop.
 ******************************************************************************
 */

#ifndef SINK_ASYNC_FILE_WRITER_HPP
#define SINK_ASYNC_FILE_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "OS.hpp"
#include "File.hpp"

namespace Sink {

/**
 * @brief Abstract background file writer with flush policy and size rollover.
 */
class AsyncFileWriter {
public:
    /**
     * @brief Provide the next segment path on rollover.
     * @param[in]  ctx     Opaque context (see setRollover).
     * @param[out] out     Buffer to receive the next file path.
     * @param[in]  outSize Size of @p out.
     * @return true if a path was produced; false keeps the current file open.
     */
    using NextPathFn = bool (*)(void* ctx, char* out, size_t outSize);

    /** @brief Generic worker-context event callback (flushed / segment opened). */
    using EventFn = void (*)(void* ctx);

    AsyncFileWriter() = default;
    virtual ~AsyncFileWriter();

    AsyncFileWriter(const AsyncFileWriter&)            = delete;
    AsyncFileWriter& operator=(const AsyncFileWriter&) = delete;

    /**
     * @brief Enable size-based rollover. Call before start().
     * @param maxSegmentBytes Roll to a new segment at this size; 0 disables.
     * @param nextPath        Callback that names the next segment.
     * @param ctx             Passed back to @p nextPath.
     */
    void setRollover(uint32_t maxSegmentBytes, NextPathFn nextPath, void* ctx);

    /**
     * @brief Fired on the worker right after each f_sync. A drain consumer uses
     *        it to advance a persisted cursor (everything written is now durable).
     */
    void setOnFlushed(EventFn fn, void* ctx);

    /**
     * @brief Fired on the worker right after a segment is opened (first one and
     *        every rollover), e.g. to re-emit a header.
     */
    void setOnSegmentOpened(EventFn fn, void* ctx);

    /**
     * @brief Start the worker and open the first file.
     * @param path            First file path.
     * @param override        Overwrite if true, else append.
     * @param flushIntervalMs Minimum spacing between f_sync calls; 0 flushes
     *                        after every chunk.
     */
    bool start(const char* path, bool override = true, uint32_t flushIntervalMs = 0);

    /// stop() timeout that waits for as long as the worker takes.
    static constexpr uint32_t kWaitForever = UINT32_MAX;

    /**
     * @brief Stop the worker, flushing and closing the file cleanly.
     *
     * A worker still busy after @p timeoutMs (a file system call that stalls)
     * is left to finish, never killed: it may hold the FatFs volume lock, and a
     * deleted task never gives a mutex back. It exits once that call returns.
     * Until then start() fails, and a later stop() waits for it again.
     *
     * @param timeoutMs How long to wait for the worker; kWaitForever (as the
     *                  destructors use, as the worker uses the object) waits
     *                  until it has finished.
     * @return true once the worker has finished, false if it is still busy.
     */
    bool stop(uint32_t timeoutMs = 5000);

    bool isRunning() const;

    /** @brief Path of the current segment (worker-owned; best-effort for logs). */
    const char* currentPath() const { return mFile.getPath(); }

    /** @brief Arm an immediate f_sync on the next drain (one-shot). */
    void requestFlush() { mFlushRequested.store(true, std::memory_order_relaxed); }

    /**
     * @brief Write bytes straight to the current segment file, bypassing the
     *        async queue.
     *
     * ONLY valid from a worker-thread callback (onSegmentOpened / onFlushed),
     * where the file is open and owned by this thread; calling it from any other
     * context races the FatFs handle. Its purpose is to lay a header down at the
     * top of a freshly opened segment, ahead of the queued record that lazily
     * triggered the open - going through the queue would drain the header out
     * second. Counts toward the segment size so rollover stays accurate.
     *
     * @return true if all bytes were written.
     */
    bool writeDirect(const void* data, size_t size);

    /** @brief Wake the worker (ISR-safe). Pull producers call this on new data. */
    void notifyData();

protected:
    /**
     * @brief Fill @p dst with up to @p maxLen bytes to write; return 0 when the
     *        source is drained for now. Runs on the worker thread.
     */
    virtual size_t produce(uint8_t* dst, size_t maxLen) = 0;

    /**
     * @brief Whether the source currently has bytes to write.
     *
     * Lets the worker open a segment lazily (only when there is data) without
     * consuming, so an idle/rollover/stop boundary never leaves an empty file
     * and a failed open never drops data. Override when the source can be peeked
     * without consuming; the default assumes data may exist and lets produce()
     * decide (pull sources that cannot be peeked keep the previous behaviour).
     */
    virtual bool sourceHasData() { return true; }

    /** @brief Non-ISR wake, for subclass push paths. */
    void wake() { mWork.give(); }

private:
    void thread();
    void drainOnce(uint32_t& lastFlushTick);
    bool openSegment(uint32_t& lastFlushTick);
    void flushIfDue(uint32_t& lastFlushTick, bool force);
    void rolloverIfNeeded();

    // 4 KB: a name-resolving FatFs call (f_open) puts ~1.1 KB of LFN/exFAT
    // scratch on this stack; less left too little headroom on the rotate path.
    OS::Thread<AsyncFileWriter, 4 * 1024> mThread;
    FS::File mFile;
    char     mBuff[512];
    char     mPath[256] = {};

    std::atomic<bool> mFlushRequested{false};
    OS::Semaphore     mWork{1, 0};        // worker wake-up

    NextPathFn mNextPath          = nullptr;  void* mNextPathCtx          = nullptr;
    EventFn    mOnFlushed         = nullptr;  void* mOnFlushedCtx         = nullptr;
    EventFn    mOnSegmentOpened   = nullptr;  void* mOnSegmentOpenedCtx   = nullptr;

    uint32_t mFlushIntervalMs = 0;
    uint32_t mMaxSegmentBytes = 0;
    uint32_t mBytesInSegment  = 0;
    bool     mOverride        = true;
    bool     mStopPending     = false;   // a stop() timed out; the worker may still run
};

}  // namespace Sink

#endif  // SINK_ASYNC_FILE_WRITER_HPP
