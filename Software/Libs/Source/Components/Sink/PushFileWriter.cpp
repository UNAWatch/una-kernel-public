/**
 ******************************************************************************
 * @file    PushFileWriter.cpp
 * @brief   Async file writer fed by pushing bytes into a stream buffer.
 ******************************************************************************
 */

#include "Sink/PushFileWriter.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Sink {

PushFileWriter::PushFileWriter()
{
    mStreamBuffer.init("PushFileWriter");
}

PushFileWriter::~PushFileWriter()
{
    // Stop the worker before our stream buffer is torn down (and before the base
    // destructor, so produce() is never dispatched on a half-destroyed object).
    // However long it takes: a worker left running would use freed memory.
    stop(kWaitForever);
    mStreamBuffer.deinit();
}

size_t PushFileWriter::produce(uint8_t* dst, size_t maxLen)
{
    return mStreamBuffer.receive(dst, static_cast<uint32_t>(maxLen), 0);
}

bool PushFileWriter::write(const void* data, size_t size)
{
    if (!isRunning()) {
        return false;
    }

    // The non-blocking mode must be all-or-nothing: with a finite timeout
    // xStreamBufferSend copies whatever fits, which would splice a truncated
    // record into the stream. Stream buffers are single-writer, and the
    // drain only frees space, so this precheck cannot be invalidated before
    // the send.
    if (mWriteTimeoutMs == 0 && mStreamBuffer.getFreeSpace() < size) {
        return false;
    }

    uint32_t sent = mStreamBuffer.send(static_cast<const uint8_t*>(data), size,
                                       mWriteTimeoutMs);
    notifyData();   // wake the worker to drain (event-driven, no polling)
    return sent == size;
}

bool PushFileWriter::write(const char* str)
{
    if (str == nullptr) {
        return false;
    }
    return write(static_cast<const void*>(str), std::strlen(str));
}

bool PushFileWriter::writef(const char* format, ...)
{
    if (format == nullptr || !isRunning()) {
        return false;
    }

    char tempBuff[256];
    va_list args;
    va_start(args, format);
    int len = std::vsnprintf(tempBuff, sizeof(tempBuff), format, args);
    va_end(args);

    if (len <= 0) {
        return false;
    }

    size_t writeLen = (static_cast<size_t>(len) < sizeof(tempBuff)) ? static_cast<size_t>(len)
                                                                    : sizeof(tempBuff) - 1;
    uint32_t sent = mStreamBuffer.send(reinterpret_cast<const uint8_t*>(tempBuff), writeLen);
    notifyData();
    return sent != 0;
}

bool PushFileWriter::write(int32_t value)
{
    char tempBuff[16];
    int len = std::snprintf(tempBuff, sizeof(tempBuff), "%d", static_cast<int>(value));
    if (len > 0 && static_cast<size_t>(len) < sizeof(tempBuff)) {
        return write(static_cast<const void*>(tempBuff), len);
    }
    return false;
}

bool PushFileWriter::write(uint32_t value)
{
    char tempBuff[16];
    int len = std::snprintf(tempBuff, sizeof(tempBuff), "%u", static_cast<unsigned int>(value));
    if (len > 0 && static_cast<size_t>(len) < sizeof(tempBuff)) {
        return write(static_cast<const void*>(tempBuff), len);
    }
    return false;
}

bool PushFileWriter::write(float value, uint8_t precision)
{
    char tempBuff[32];
    char formatBuff[8];
    std::snprintf(formatBuff, sizeof(formatBuff), "%%.%df", precision);
    int len = std::snprintf(tempBuff, sizeof(tempBuff), formatBuff, value);
    if (len > 0 && static_cast<size_t>(len) < sizeof(tempBuff)) {
        return write(static_cast<const void*>(tempBuff), len);
    }
    return false;
}

bool PushFileWriter::write(std::string_view sv)
{
    if (sv.empty()) {
        return true;   // nothing to write is not an error
    }
    return write(static_cast<const void*>(sv.data()), sv.size());
}

}  // namespace Sink
