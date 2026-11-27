/**
 ******************************************************************************
 * @file    PullFileWriter.hpp
 * @brief   Async file writer that pulls bytes from a source callback.
 ******************************************************************************
 */

#ifndef SINK_PULL_FILE_WRITER_HPP
#define SINK_PULL_FILE_WRITER_HPP

#include <cstddef>
#include <cstdint>

#include "Sink/AsyncFileWriter.hpp"

namespace Sink {

/**
 * @brief AsyncFileWriter that fetches its data from a source callback instead of
 *        a queue. The worker calls the source on each wake until it returns 0.
 *
 * For a consumer whose data already lives in its own buffer (e.g. a ring), this
 * avoids a second queue and a second thread: notifyData() wakes the writer, the
 * source hands over bytes, the writer's own thread persists them.
 */
class PullFileWriter : public AsyncFileWriter {
public:
    /** @brief Fill @p dst with up to @p maxLen bytes; return 0 when drained. */
    using SourceFn = size_t (*)(void* ctx, uint8_t* dst, size_t maxLen);

    /** @brief Peek: whether the source has bytes to write, without consuming. */
    using AvailFn = bool (*)(void* ctx);

    ~PullFileWriter() override { stop(kWaitForever); }

    /** @brief Set the data source. Call before start(). */
    void setSource(SourceFn fn, void* ctx)
    {
        mSource    = fn;
        mSourceCtx = ctx;
    }

    /**
     * @brief Optional peek (shares the source context). Lets the writer open a
     *        segment only when there is data, so an idle/rollover/stop wake never
     *        creates an empty file. If unset, the writer assumes data may exist.
     */
    void setAvailable(AvailFn fn) { mAvail = fn; }

protected:
    size_t produce(uint8_t* dst, size_t maxLen) override
    {
        return mSource ? mSource(mSourceCtx, dst, maxLen) : 0;
    }

    bool sourceHasData() override
    {
        return mAvail ? mAvail(mSourceCtx) : true;
    }

private:
    SourceFn mSource    = nullptr;
    AvailFn  mAvail     = nullptr;
    void*    mSourceCtx = nullptr;
};

}  // namespace Sink

#endif  // SINK_PULL_FILE_WRITER_HPP
