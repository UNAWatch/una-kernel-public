/**
 ******************************************************************************
 * @file    SessionFileWriter.hpp
 * @brief   Push session-file writer: PushFileWriter + SessionNaming bundle.
 *
 * Convenience for push loggers: a PushFileWriter you feed with write(), wrapped
 * with the <base>_<id>_<NNNN>_<ts>.<ext> naming, count-based retention and
 * size-based rollover (the rollover mechanism lives in the writer; this only
 * supplies the next name). The owning logger stays the policy owner (keepNewest
 * / maxFileBytes); this class is the mechanism bundle.
 *
 * Used directly by raw-byte loggers (GPS, PPG) and composed by CSVLogger.
 ******************************************************************************
 */

#ifndef SINK_SESSION_FILE_WRITER_HPP
#define SINK_SESSION_FILE_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Sink/PushFileWriter.hpp"
#include "Sink/SessionNaming.hpp"

namespace Sink {

/**
 * @brief Rolling session-file writer over a PushFileWriter.
 */
class SessionFileWriter {
public:
    using SegmentOpenedCb = std::function<void()>;

    SessionFileWriter()  = default;
    // However long it takes, and here rather than in mWriter's own destructor:
    // the worker uses mNaming and mOnSegmentOpened, destroyed before mWriter.
    ~SessionFileWriter() { stop(AsyncFileWriter::kWaitForever); }

    SessionFileWriter(const SessionFileWriter&)            = delete;
    SessionFileWriter& operator=(const SessionFileWriter&) = delete;

    /**
     * @brief Configure naming and policy. Call before start().
     *
     * @param dir             Directory, e.g. "2:/BatteryLog/".
     * @param base            Logger base name, e.g. "battery".
     * @param id              Device id string.
     * @param ext             Extension without the dot, e.g. "csv".
     * @param keepNewest      Session files to keep incl. the live one; 0 = keep all.
     * @param maxFileBytes    Roll to a new session at this size; 0 = no rollover.
     * @param flushIntervalMs Backing-writer flush coalescing.
     */
    void configure(const char* dir, const char* base, const char* id,
                   const char* ext, uint32_t keepNewest,
                   uint32_t maxFileBytes, uint32_t flushIntervalMs);

    /**
     * @brief Callback fired right after each segment is opened (the first one
     *        and every rollover) so a formatted logger can re-emit its header.
     */
    void onSegmentOpened(SegmentOpenedCb cb) { mOnSegmentOpened = std::move(cb); }

    bool start();                                  ///< Prune + open the first session.
    /// Close the current session. See AsyncFileWriter::stop(): false if the
    /// worker is still busy after @p timeoutMs (left to finish).
    bool stop(uint32_t timeoutMs = 5000);
    bool isRunning() const { return mWriter.isRunning(); }

    bool write(const void* data, size_t size) { return mWriter.write(data, size); }
    bool write(const char* str)               { return mWriter.write(str); }

    /**
     * @brief Direct-to-file write for the onSegmentOpened callback (worker
     *        thread only) - lands ahead of any queued data. See
     *        AsyncFileWriter::writeDirect.
     */
    bool writeDirect(const void* data, size_t size) { return mWriter.writeDirect(data, size); }
    /// See PushFileWriter::setWriteTimeout().
    void setWriteTimeout(uint32_t timeoutMs) { mWriter.setWriteTimeout(timeoutMs); }

    void requestFlush() { mWriter.requestFlush(); }

    const char* currentPath() const { return mWriter.currentPath(); }

private:
    static void onSegmentOpenedTrampoline(void* ctx);

    PushFileWriter  mWriter;
    SessionNaming   mNaming;
    SegmentOpenedCb mOnSegmentOpened;
    uint32_t        mMaxFileBytes    = 0;
    uint32_t        mFlushIntervalMs = 0;
    char            mFirstPath[128]  = {};
};

}  // namespace Sink

#endif  // SINK_SESSION_FILE_WRITER_HPP
