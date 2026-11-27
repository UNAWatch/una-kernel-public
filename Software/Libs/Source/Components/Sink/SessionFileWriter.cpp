/**
 ******************************************************************************
 * @file    SessionFileWriter.cpp
 * @brief   Push session-file writer: PushFileWriter + SessionNaming bundle.
 ******************************************************************************
 */

#include "Sink/SessionFileWriter.hpp"

#define LOG_MODULE_PRX      "SessionFileWriter"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

namespace Sink {

void SessionFileWriter::configure(const char* dir, const char* base, const char* id,
                                  const char* ext, uint32_t keepNewest,
                                  uint32_t maxFileBytes, uint32_t flushIntervalMs)
{
    mNaming.configure(dir, base, id, ext, keepNewest);
    mMaxFileBytes    = maxFileBytes;
    mFlushIntervalMs = flushIntervalMs;
}

void SessionFileWriter::onSegmentOpenedTrampoline(void* ctx)
{
    auto* self = static_cast<SessionFileWriter*>(ctx);
    if (self->mOnSegmentOpened) {
        self->mOnSegmentOpened();   // e.g. CSV header into the fresh segment
    }
}

bool SessionFileWriter::start()
{
    // Capture the session start time once; every segment of this session reuses
    // it, so rolled files stay visibly one session even after the earliest
    // segments are pruned. Per-record wall time still lives in the rows.
    mNaming.captureSession();
    if (!mNaming.nextPath(mFirstPath, sizeof(mFirstPath))) {
        LOG_ERROR("failed to build the first session path\n");
        return false;
    }

    // Size rollover is the writer's job; naming supplies the next segment path.
    mWriter.setRollover(mMaxFileBytes, &SessionNaming::nextPathTrampoline, &mNaming);
    mWriter.setOnSegmentOpened(&SessionFileWriter::onSegmentOpenedTrampoline, this);

    return mWriter.start(mFirstPath, true /* override */, mFlushIntervalMs);
}

bool SessionFileWriter::stop(uint32_t timeoutMs)
{
    return mWriter.stop(timeoutMs);
}

}  // namespace Sink
