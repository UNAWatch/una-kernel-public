/**
 ******************************************************************************
 * @file    CSVLogger.cpp
 * @brief   CSV record formatter over a rolling session file.
 *
 * @details
 * Accumulates field values in an internal string buffer and writes them as one
 * CSV record. Naming, retention and rollover live in the backing
 * SessionFileWriter; the header is re-emitted on every segment via the
 * onSegmentOpened callback.
 *
 ******************************************************************************/
#define LOG_MODULE_PRX      "CSVLogger"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Loggers/CSVLogger.hpp"

namespace Component {

CSVLogger::CSVLogger(const char* header)
    : mHeader(header)
{
    mString.reserve(128);
}

void CSVLogger::setHeader(const char* header)
{
    mHeader = header;
}

void CSVLogger::setNaming(const char* dir, const char* base, const char* id)
{
    mDir  = dir;
    mBase = base;
    mId   = id;
}

void CSVLogger::setRetention(uint32_t keepNewest)
{
    mKeepNewest = keepNewest;
}

void CSVLogger::setMaxFileBytes(uint32_t maxBytes)
{
    mMaxFileBytes = maxBytes;
}

void CSVLogger::setFlushInterval(uint32_t ms)
{
    mFlushIntervalMs = ms;
}

/**
 * @brief Open a fresh session file and write its header.
 *
 * @return true on success.
 *
 * @details
 * Allocates the next session index (pruning old sessions per the retention
 * policy) and opens the file. The header is written through the
 * onSegmentOpened callback, so it is also re-emitted on every rollover.
 */
bool CSVLogger::open()
{
    if (mOpened) {
        return true;
    }

    // A close() that timed out left its worker to finish, and it uses the
    // naming and callback configured below: not before it has. Returns at
    // once either way; the caller tries again later.
    if (!mWriter.stop(0)) {
        return false;
    }

    mWriter.configure(mDir, mBase, mId, "csv", mKeepNewest, mMaxFileBytes,
                      mFlushIntervalMs);
    mWriter.onSegmentOpened([this] { writeHeader(); });

    // CSV records are queued from sensor data callbacks, which run inside
    // the sensor manager's refresh sweep. A full buffer (wedged file
    // backend) must drop the record, never block the sweep — a blocked
    // sweep stalls every sensor and every connect/disconnect behind it.
    mWriter.setWriteTimeout(0);

    if (!mWriter.start()) {
        return false;
    }

    mOpened = true;
    return true;
}

bool CSVLogger::isOpened()
{
    return mOpened;
}

bool CSVLogger::close()
{
    bool ok = true;
    if (mOpened) {
        // False: the worker is still in a stalled file call, and open() fails
        // until it has finished.
        ok = mWriter.stop();
        mOpened = false;
    }
    return ok;
}

/**
 * @brief Emit the CSV header as the first record of a segment.
 */
void CSVLogger::writeHeader()
{
    if (mHeader == nullptr || mHeader[0] == '\0') {
        return;
    }

    // Runs from onSegmentOpened, on the writer's worker thread - a different
    // thread from the one that fills records via startRecord()/append(). Two
    // reasons to write straight to the file here instead of the usual path:
    //   1) The segment is opened lazily on the first queued byte, so by now a
    //      data record is already waiting in the async queue; a queued header
    //      would drain out second and land on the file's second line. Direct-to-
    //      file puts it at the top of the segment, ahead of that record.
    //   2) Going through startRecord()/mString would touch the record buffer
    //      from this worker thread while the owning thread may be mid-append on
    //      the next record. Writing only the constant mHeader keeps mString
    //      single-threaded (owned by the caller).
    mWriter.writeDirect(mHeader, std::strlen(mHeader));
    mWriter.writeDirect("\n", 1);
}

/**
 * @brief Append a CSV field separator if the record buffer is not empty.
 */
void CSVLogger::separator()
{
    if (!mString.empty()) {
        mString.push_back(',');
    }
}

/**
 * @brief Start a new CSV record (clears the internal record buffer).
 */
bool CSVLogger::startRecord()
{
    mString.clear();

    return true;
}

/**
 * @brief Finalize the record and write it to the file.
 *
 * @param forceFlush When true, arm the writer to flush this record immediately,
 *        bypassing the flush-interval coalescing.
 */
bool CSVLogger::stopRecord(bool forceFlush)
{
    LOG_DEBUG("write data: [%s]\n", mString.c_str());

    mString.push_back('\n');

    // Arm the flush before queuing the bytes: the writer drains on its own
    // thread, so the request must be set before the data is visible to it,
    // otherwise the worker could write and skip the flush on the same drain.
    if (forceFlush) {
        mWriter.requestFlush();
    }

    // Use raw write, not writef: the record is already formatted, and writef
    // runs it through a 256-byte vsnprintf buffer that silently truncates
    // longer rows (header overflow). Going direct also closes a format-string
    // hole if a CSV field ever contains '%'.
    mWriter.write(mString.c_str());

    return true;
}

/**
 * @brief Append a string field to the current record.
 */
void CSVLogger::append(const char* str)
{
    if (!str) {
        return;
    }

    separator();

    mString.append(str);
}

/**
 * @brief Append a boolean field ('1' or '0') to the current record.
 */
void CSVLogger::append(bool v)
{
    separator();

    mString.push_back(v ? '1' : '0');
}

/**
 * @brief Append an unsigned 16-bit integer field to the current record.
 */
void CSVLogger::append(uint16_t v)
{
    separator();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v));
    mString.append(buf);
}

/**
 * @brief Append a signed 16-bit integer field to the current record.
 */
void CSVLogger::append(int16_t v)
{
    separator();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d", v);
    mString.append(buf);
}

/**
 * @brief Append an unsigned 32-bit integer field to the current record.
 */
void CSVLogger::append(uint32_t v)
{
    separator();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(v));
    mString.append(buf);
}

/**
 * @brief Append a 16-bit value as hexadecimal (e.g. "0x00AF").
 */
void CSVLogger::appendHex(uint16_t v)
{
    separator();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(v));
    mString.append(buf);
}

/**
 * @brief Append a floating point field to the current record.
 *
 * @param value      Floating point value.
 * @param precision  Number of digits after the decimal point (clamped to [0..6]).
 */
void CSVLogger::append(float value, int precision)
{
    separator();

    precision = std::clamp(precision, 0, 6);

    char fmt[8];
    std::snprintf(fmt, sizeof(fmt), "%%.%df", precision);

    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, static_cast<double>(value));
    mString.append(buf);
}

} // namespace Component
