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

#ifndef __TIME_PORTING_HPP
#define __TIME_PORTING_HPP

#include "Libc/ImageMaker.h"

void TimePorting_init();
void TimePorting_assignTime(struct libc_syscalls_time_t* port);
void TimePorting_updateTimeZone();
void TimePorting_setTimeZone(int16_t tz_minutes);
void TimePorting_setTime(const struct tm &dateTime);

void TimePorting_setTimeZoneRule(const char* posixTz);

#endif /* __TIME_PORTING_HPP */

