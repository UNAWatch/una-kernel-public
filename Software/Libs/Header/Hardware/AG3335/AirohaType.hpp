/**
 ******************************************************************************
 * @file    AirohaType.hpp
 * @brief   Common types definition for Airoha chip.
 ******************************************************************************
 *
 ******************************************************************************
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>


namespace Airoha
{

/**
 * @brief: GNSS constellation system types
 */
enum class SystemType : uint8_t {
    GPS     = 0,
    GLONASS = 1,
    GALILEO = 2,
    BEIDOU  = 3,
    QZSS    = 4,
    UNKNOWN = 255
};

enum class StartType : uint8_t
{
    HOT = 0,
    WARM,
    COLD,
};

enum class GpsNavMode : uint8_t {
    NORMAL     = 0,
    FITNESS    = 1, // Default
    BALLON     = 3,
    STATIONARY = 4,
    DRONE      = 5,
    SWIMMING   = 7,
    BIKE       = 8
};

enum class DebugLog : uint8_t {
    DISABLE     = 0,
    ENABLEFULL  = 1,
    ENABLELITE  = 2
};

struct IndicationSystemMessage {
    enum Type {
       STARTUP = 1, // Notification for GNSS system startup
    };
    Type type;
};


struct ReqAiding {
    enum Type {
       EPO              = 0, // Need to update EPO data.
       TIME             = 1, // Need to update the time.
       LOCATION         = 2, // Need to update the location.
       IONOSPHERIC_MAP  = 6, // Need to update the global ionospheric map.
    };
    Type request;
    SystemType system;
    uint32_t week;
    uint32_t tow;
    float lat;
    float lon;
};


struct ReferenceLocation {
    float latitude  = 0.0f;     // Reference latitude in degrees
    float longitude = 0.0f;     // Reference longitude in degrees
    float height    = 0.0f;     // Reference height in meters
    float accMaj    = 50.0f;    // Semi-major RMS accuracy [m]
    float accMin    = 50.0f;    // Semi-minor RMS accuracy [m]
    float bear      = 0.0f;     // Bearing in degrees [Range: 0~179 degrees]
    float accVert   = 100.0f;   // Vertical RMS accuracy [m]
};

/**
 * @brief Whether a reference location is the "no location" marker, (0, 0, 0).
 *
 * Saving the marker clears the stored location (Backend does this on reset).
 * Latitude and longitude are signed (south and west are negative), so the test
 * is on magnitudes: a real fix in the southern and western hemispheres at or
 * below sea level is not the marker.
 */
inline bool isUnsetReferenceLocation(float lat, float lon, float alt)
{
    constexpr float kEpsilon = 0.000001f;
    return std::fabs(lat) < kEpsilon && std::fabs(lon) < kEpsilon && std::fabs(alt) < kEpsilon;
}

struct EpoGnssStatus {
    SystemType SysId;   // The GNSS system ID.
    uint32_t Set;       // Total number sets of EPO data stored in chip.
    uint32_t FWN;       // GPS week number &
    uint32_t FTOW;      // TOW of the first set of EPO data stored in chip respectively (flash)
    uint32_t LWN;       // GPS week number &
    uint32_t LTOW;      // TOW of the last set of EPO data stored in chip respectively (flash)
    uint32_t FCWN;      // GPS week number &
    uint32_t FCTOW;     // TOW of the first set of EPO data that are currently used respectively
    uint32_t LCWN;      // GPS week number &
    uint32_t LCTOW;     // TOW of the last set of EPO data that are currently used respectively
};


/**
 * @brief: Owning byte buffer for EPO segments.
 *
 * Deliberately not a std::vector. The firmware is built with -fno-exceptions,
 * so a failed allocation inside vector::reserve() reaches libstdc++'s throwing
 * operator new, which calls abort() and resets the watch. EPO aiding is
 * optional, and the correct response to a heap that cannot serve it is to skip
 * aiding, not to reboot. This buffer therefore allocates with the nothrow
 * operator new and reports failure to the caller.
 *
 * That guarantee rests on the project's own override of
 * operator new[](size_t, const nothrow_t&) in heap_useNewlib_ST.cpp, which
 * returns malloc()'s result. Without it the nothrow form would still reach
 * abort() -- the exact failure this class exists to prevent -- so the two
 * belong together.
 */
class EpoBuffer {
public:
    EpoBuffer() = default;
    ~EpoBuffer() { release(); }

    EpoBuffer(const EpoBuffer&) = delete;
    EpoBuffer& operator=(const EpoBuffer&) = delete;

    EpoBuffer(EpoBuffer &&other) noexcept
        : mData(other.mData), mCapacity(other.mCapacity), mSize(other.mSize)
    {
        other.mData = nullptr;
        other.mCapacity = 0;
        other.mSize = 0;
    }

    EpoBuffer& operator=(EpoBuffer &&other) noexcept
    {
        if (this != &other) {
            release();
            mData = other.mData;
            mCapacity = other.mCapacity;
            mSize = other.mSize;
            other.mData = nullptr;
            other.mCapacity = 0;
            other.mSize = 0;
        }
        return *this;
    }

    /**
     * @brief: Allocate storage for at least @p capacity bytes.
     * @retval: false if the heap cannot serve the request, in which case the
     *          buffer is left exactly as it was.
     *
     * On success the existing contents and size() are preserved, as
     * std::vector::reserve does.
     */
    bool reserve(size_t capacity)
    {
        if (capacity <= mCapacity) {
            return true;
        }

        uint8_t *mem = new (std::nothrow) uint8_t[capacity];
        if (mem == nullptr) {
            return false;
        }

        // Carry the contents across. Without this a grow would silently empty
        // the buffer, because release() clears mSize -- and a method called
        // reserve() emptying its buffer is a trap to leave lying around, even
        // though no current caller grows twice.
        const size_t keep = mSize;
        if (keep > 0) {
            std::memcpy(mem, mData, keep);
        }

        release();
        mData = mem;
        mCapacity = capacity;
        mSize = keep;
        return true;
    }

    /**
     * @brief: Set the used length. Never allocates.
     * @retval: false if @p size exceeds the reserved capacity.
     */
    bool resize(size_t size)
    {
        if (size > mCapacity) {
            return false;
        }
        mSize = size;
        return true;
    }

    uint8_t* data() { return mData; }
    const uint8_t* data() const { return mData; }
    size_t size() const { return mSize; }
    bool empty() const { return mSize == 0; }

private:
    void release()
    {
        delete[] mData;
        mData = nullptr;
        mCapacity = 0;
        mSize = 0;
    }

    uint8_t *mData = nullptr;
    size_t mCapacity = 0;
    size_t mSize = 0;
};

/**
 * @brief: What one EPO injection sends: the header, if the constellation has
 *         one, and the segments for the current 6-hour window, already
 *         selected (see EpoParser::selectSegments()). Never the whole file:
 *         that is 64 KB for GPS, of which a single block is sent.
 */
struct EpoData {
    static constexpr size_t kSegmentSize = 72;

    bool hasHeader = false;             ///< Galileo/BeiDou: header must go first
    /// Valid when hasHeader. Word-aligned, like the segments, because a
    /// segment is eighteen 32-bit words; after the bool it would not be.
    alignas(4) uint8_t header[kSegmentSize] {};
    EpoBuffer segments;                 ///< 72-byte segments back to back, in file order

    size_t segmentCount() const { return segments.size() / kSegmentSize; }

    /// Nothing to inject. A header on its own is not worth sending.
    bool empty() const { return segments.empty(); }
};



} // namespace Airoha
