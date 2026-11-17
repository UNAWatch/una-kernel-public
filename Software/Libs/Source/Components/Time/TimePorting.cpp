/**
 ******************************************************************************
 * @file    TimePorting.hpp
 * @brief   Here we port the newlib callback to the platform. 
 *          This port is also thread-safe due to the use of a mutex.
 *
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "Time/TimePorting.hpp"
#include "Time/TimeUtils.h"
#include "Rtc.hpp"
#include "OS.hpp"

#include <time.h>

////////////////////////////////////////////////////////////////////////////////
/// Private data
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Mutex used to synchronize access to the RTC.
 *
 * In an RTOS environment (e.g. FreeRTOS), multiple tasks may try
 * to access the RTC at the same time. This mutex ensures safe,
 * serialized access.
 */
static OS::Mutex* mMutex;

////////////////////////////////////////////////////////////////////////////////
/// Private code
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Read current UTC time into a timeval structure.
 *
 * Reads date/time from the hardware RTC (via @c rtc_read_bcd_datetime),
 * converts it into UNIX epoch seconds (since 1970-01-01 UTC), and
 * stores the result in a @c timeval structure.
 *
 * @param[out] tv Output buffer to store current time.
 *
 * @return 0 on success, -1 on failure (e.g., @p tv is NULL or RTC read fails).
 */
static int gettimeofday_unsafe(struct timeval *tv)
{
    if (!tv) {
        return -1;
    }

    Interface::IRtc::DateTime utc = Driver::Rtc::GetInstance().get();

//    if (!utc.time_reliable) {
//        return -1;
//    }

    tv->tv_sec  = TimeUtils_tm2epoch(&utc);
    tv->tv_usec = (suseconds_t) (utc.ms * 1000);

    return 0;
}

/**
 * @brief Acquire the RTC access mutex.
 *
 * Should be called before reading/writing RTC.
 */
static bool lock()
{
    if (!mMutex) {
        return false;
    }

    return mMutex->lock();
}

/**
 * @brief Release the RTC access mutex.
 *
 * Should be called after reading/writing RTC.
 */
static void unLock()
{
    if (mMutex) {
        mMutex->release();
    }
}

/**
 * @brief Newlib system hook: reentrant gettimeofday().
 *
 * Newlib calls this function internally when @c time(), @c gettimeofday(),
 * @c localtime_r(), etc. are used. This implementation provides the
 * actual time from the hardware RTC.
 *
 * @param[in]  r   Reentrancy structure (unused).
 * @param[out] tv  Output buffer to store current time.
 * @param[in]  tz  Timezone information (unused).
 *
 * @return 0 on success, -1 on failure.
 */
extern "C" int gettimeofday(struct timeval *tv)
{
    if (!lock()) {
        return -1;
    }

    int status = gettimeofday_unsafe(tv);

    unLock();

    return status;
}

//extern "C" int _gettimeofday (struct timeval *__restrict __p,
//              void *__restrict __tz)    // __tz deprecated parameter
//{
//    if (!lock()) {
//        return -1;
//    }
//
//    int status = gettimeofday_unsafe(__p);
//
//    unLock();
//
//    return status;
//}

////////////////////////////////////////////////////////////////////////////////
/// Public code
////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize time porting layer.
 *
 * Creates the mutex for RTC access synchronization.
 * Must be called once during system startup (after OS init).
 */
void TimePorting_init()
{
    if (!mMutex) {
        mMutex = new OS::Mutex;
        mMutex->init("time");

        TimePorting_updateTimeZone();
    }
}

/**
 * @brief Assigns the time syscall to the given port structure.
 *
 * This function installs the local implementation of @c gettimeofday()
 * into the provided @c libc_syscalls_time_t structure. It allows the
 * libc/syscalls layer to use the board-specific RTC-backed time function.
 *
 * @param[in,out] port Pointer to a time syscalls structure to be initialized.
 */
void TimePorting_assignTime(struct libc_syscalls_time_t* port)
{
    port->getTime = gettimeofday;
}

/**
 * @brief Update the system timezone based on RTC configuration.
 *
 * Reads the current Time Zone (TZ) and Daylight Saving Time (DST) offset
 * values from the RTC driver. The TZ and DST value is expressed in seconds.
 * These are converted to minutes, combined, and applied via
 * TimePorting_setTimeZone().
 */
void TimePorting_updateTimeZone()
{
    // Raw data from the RTC
    int tz  = Driver::Rtc::GetInstance().getTimeZone();     // in seconds
    int dst = Driver::Rtc::GetInstance().getDstOffset();    // in seconds

    // Convert TZ to minutes (step is 15 minutes)
    int tz_minutes = tz / 60;

    // Convert DST to minutes
    int dst_minutes = dst / 60;

    TimePorting_setTimeZone(tz_minutes + dst_minutes);
}

/**
 * @brief Apply a fixed timezone offset in minutes.
 *
 * Builds a POSIX TZ string of the form "UNA±H[:MM]" and applies it using
 * @c setenv("TZ", ...) followed by @c tzset(). This ensures that
 * functions like @c localtime_r() and @c strftime() reflect the current
 * offset including DST if it has been applied beforehand.
 *
 * @param[in] tz_minutes Timezone offset in minutes relative to UTC.
 *                       (e.g. +180 = UTC+3, -330 = UTC-5:30).
 *
 * @note POSIX TZ format uses inverted sign: "UNA-3" means UTC+3.
 * @note The prefix "UNA" is arbitrary and will be returned by strftime("%Z").
 */
void TimePorting_setTimeZone(int16_t tz_minutes)
{
    if (!lock()) {
        return;
    }

    int h = tz_minutes / 60;
    int m = abs(tz_minutes % 60);

    char tzstr[64];
    if (m == 0) {
        if (h != 0) {
            snprintf(tzstr, sizeof(tzstr), "UNA%+d", -h);
        } else {
            snprintf(tzstr, sizeof(tzstr), "UNA");
        }
    } else {
        snprintf(tzstr, sizeof(tzstr), "UNA%+d:%02d", -h, m);
    }

    setenv("TZ", tzstr, 1);
    tzset();

    unLock();
}

void TimePorting_setTimeZoneRule(const char* posixTz)
{
    if (!posixTz || !lock()) {
        return;
    }

    setenv("TZ", posixTz, 1);
    tzset();

    unLock();
}

/**
 * @brief Set the current UTC time into the hardware RTC.
 *
 * This function updates the RTC with a new time value expressed in UTC.
 * The access is protected with a mutex to ensure thread safety when
 * multiple tasks attempt to update or read the RTC simultaneously.
 *
 * @param[in] utctime A reference to a @c struct tm containing the
 *            UTC date and time fields (year, month, day, hour,
 *            minute, second).
 *
 * @note The caller must provide the time in UTC, not local time.
 *       To convert from local time to UTC, use @c mktime() and
 *       @c gmtime_r() as appropriate before calling this function.
 */
void TimePorting_setTime(const struct tm &utctime)
{
    if (!lock()) {
        return;
    }

    Driver::Rtc::GetInstance().set(utctime);

    unLock();
}
