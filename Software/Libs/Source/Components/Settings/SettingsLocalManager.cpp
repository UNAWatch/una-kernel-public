/**
 ******************************************************************************
 * @file    SettingsLocalManager.cpp
 * @brief   Local settings management.
 ******************************************************************************
 *
 * @code{.json}
 * {
 *   "version":5,
 *   "alertMute":false,
 *   "watchFaceId":"A1C0F5D38B26E479",
 *   "gpsPwrMode":0,
 *   "developerMode":false,
 *   "sound":{
 *     "buttons":false,
 *     "msgCalls":false,
 *     "alerts":false
 *   },
 *   "vibro":{
 *     "buttons":false,
 *     "msgCalls":false,
 *     "alerts":false
 *   },
 *   "clock":{
 *     "format":0,
 *     "seconds":false,
 *     "dateOrder":0
 *   },
 *   "backlight":{
 *     "timeout":5,
 *     "gesture":true
 *   },
 *   "sleep":{
 *     "schedule":false,
 *     "active":false,
 *     "scheduledActive":false,
 *     "alertsMutedBefore":false,
 *     "mon":{"start":1320,"end":420},
 *     ...
 *     "sun":{"start":1320,"end":420}
 *   }
 * }
 * @endcode
 */

#include "SettingsLocalManager.hpp"

#define LOG_MODULE_PRX      "Settings.Local"
#define LOG_MODULE_LEVEL    LOG_LEVEL_DEBUG
#include "Logger.h"

#include "main.h"
#include "SDK/JSON/JsonStreamReader.hpp"
#include "SDK/JSON/JsonStreamWriter.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace Settings
{

namespace
{

/// Keys of the sleep windows, in WatchSettingsLocal::Sleep::days order.
constexpr const char *kDayKeys[WatchSettingsLocal::Sleep::kDays] = {
    "mon", "tue", "wed", "thu", "fri", "sat", "sun"
};

} // namespace

LocalManager::LocalManager() :
        Settings::ManagerBase<WatchSettingsLocal>(SETTINGS_LOCAL_FILE_FULL_PATH)
{
}

bool LocalManager::parse(const char*         buff,
                         size_t              length,
                         WatchSettingsLocal& settings)
{
    SDK::JsonStreamReader r {buff, length};
    if (!r.validate()) {
        LOG_ERROR("JSON is invalid\n");
        return false;
    }

    uint32_t oldVersion = 0;
    r.get("version", oldVersion);
    // Set current version
    settings.version = WatchSettingsLocal::kVersion;

    r.get("alertMute", settings.alertMutedFlag);

    // Empty, or absent in a file written before version 3, both leave the
    // default 0 -- the built-in watch face.
    std::string_view faceId;
    if (r.get("watchFaceId", faceId) && !faceId.empty()) {
        // The view spans the JSON buffer and is not NUL-terminated, so it has
        // to be copied out before it can be converted. Anything longer than an
        // id is not one: taking the first sixteen digits of it would name some
        // other app rather than fail, so it is left at the default instead.
        char digits[17] {};
        if (faceId.size() < sizeof(digits)) {
            std::memcpy(digits, faceId.data(), faceId.size());
            settings.watchfaceAppId = std::strtoull(digits, nullptr, 16);
        } else {
            LOG_WARNING("Watch face id is not an id: %u digits\n",
                    static_cast<unsigned>(faceId.size()));
        }
    }

    uint8_t pm = 0;
    r.get("gpsPwrMode", pm);

    // Settings version 1 reduced the GNSS mode to 2 options:
    // - Standard
    // - Precision
    if (oldVersion == 0 && WatchSettingsLocal::kVersion >= 1) {
        settings.gpsPowerMode = WatchSettingsLocal::GpsPowerMode::GPS_PM_STANDARD;
    } else {
        if (pm < WatchSettingsLocal::GpsPowerMode::GPS_PM_COUNT) {
            settings.gpsPowerMode = static_cast<WatchSettingsLocal::GpsPowerMode>(pm);
        }
    }

    r.get("sound.buttons",  settings.sound.buttons);
    r.get("sound.msgCalls", settings.sound.msgCalls);
    r.get("sound.alerts",   settings.sound.alerts);

    r.get("vibro.buttons",  settings.vibro.buttons);
    r.get("vibro.msgCalls", settings.vibro.msgCalls);
    r.get("vibro.alerts",   settings.vibro.alerts);

    // A file written before the clock settings existed carries no "clock" map,
    // so the defaults (24-hour, no seconds, day-before-month date order) are
    // left in place. Each key below is independently optional for the same reason.
    uint8_t fmt = 0;
    if (r.get("clock.format", fmt)
            && fmt < WatchSettingsLocal::Clock::Format::FMT_COUNT) {
        settings.clock.format = static_cast<WatchSettingsLocal::Clock::Format>(fmt);
    }
    r.get("clock.seconds", settings.clock.seconds);

    uint8_t dateOrder = 0;
    if (r.get("clock.dateOrder", dateOrder)
            && dateOrder < WatchSettingsLocal::Clock::DateOrder::DATE_ORDER_COUNT) {
        settings.clock.dateOrder = static_cast<WatchSettingsLocal::Clock::DateOrder>(dateOrder);
    }

    // Version 4. As with the clock map, a file written before these existed
    // leaves the defaults in place, so no migration branch is needed. A value
    // outside what the menus offer is ignored rather than clamped: it was not
    // written by this firmware.
    uint8_t timeoutS = 0;
    if (r.get("backlight.timeout", timeoutS)) {
        for (uint8_t t : WatchSettingsLocal::Backlight::kTimeoutsS) {
            if (t == timeoutS) {
                settings.backlight.timeoutS = timeoutS;
            }
        }
    }
    r.get("backlight.gesture", settings.backlight.gesture);

    // Version 5. Absent in older files, which leaves it off.
    r.get("developerMode", settings.developerMode);

    r.get("sleep.schedule",         settings.sleep.schedule);
    r.get("sleep.active",            settings.sleep.active);
    r.get("sleep.scheduledActive",   settings.sleep.scheduledActive);
    r.get("sleep.alertsMutedBefore", settings.sleep.alertsMutedBefore);

    char key[] = "sleep.xxx.start";
    for (uint8_t d = 0; d < WatchSettingsLocal::Sleep::kDays; ++d) {
        WatchSettingsLocal::Sleep::Window &w = settings.sleep.days[d];
        uint16_t minutes = 0;

        std::snprintf(key, sizeof(key), "sleep.%s.start", kDayKeys[d]);
        if (r.get(key, minutes) && minutes < WatchSettingsLocal::Sleep::kMinutesPerDay) {
            w.startMin = minutes;
        }

        std::snprintf(key, sizeof(key), "sleep.%s.end", kDayKeys[d]);
        if (r.get(key, minutes) && minutes < WatchSettingsLocal::Sleep::kMinutesPerDay) {
            w.endMin = minutes;
        }
    }

    return true;
}

size_t LocalManager::create(const WatchSettingsLocal& settings,
                            char*                     buff,
                            size_t                    buffSize)
{
    SDK::JsonStreamWriter w {buff, buffSize};
    w.startMap();

    w.add("version", WatchSettingsLocal::kVersion);

    w.add("alertMute", settings.alertMutedFlag);

    // As a hex string: add() renders a 64-bit number through a double, which
    // rounds an ID into a different one. Empty when no face is selected, so
    // that reads as "none" rather than as an app whose ID happens to be zero.
    if (settings.watchfaceAppId != 0) {
        // Written a nibble at a time rather than with a format string: the
        // reduced C library this links against is built without 64-bit integer
        // support in printf, and drops the second 'l' rather than saying so --
        // half an ID would be stored and the face would not survive a restart.
        static const char kHexDigits[] = "0123456789ABCDEF";

        char digits[17] {};
        uint64_t id = settings.watchfaceAppId;
        for (int i = 15; i >= 0; --i) {
            digits[i] = kHexDigits[id & 0xF];
            id >>= 4;
        }

        w.add("watchFaceId", digits);
    } else {
        w.add("watchFaceId", "");
    }

    w.add("gpsPwrMode", static_cast<uint8_t>(settings.gpsPowerMode));
    w.add("developerMode", settings.developerMode);

    {
        SDK::JsonStreamWriter::KeyedMapScope m{w, "sound"};
        w.add("buttons", settings.sound.buttons);
        w.add("msgCalls", settings.sound.msgCalls);
        w.add("alerts", settings.sound.alerts);
    }

    {
        SDK::JsonStreamWriter::KeyedMapScope m{w, "vibro"};
        w.add("buttons", settings.vibro.buttons);
        w.add("msgCalls", settings.vibro.msgCalls);
        w.add("alerts", settings.vibro.alerts);
    }

    {
        SDK::JsonStreamWriter::KeyedMapScope m{w, "clock"};
        w.add("format", static_cast<uint8_t>(settings.clock.format));
        w.add("seconds", settings.clock.seconds);
        w.add("dateOrder", static_cast<uint8_t>(settings.clock.dateOrder));
    }

    {
        SDK::JsonStreamWriter::KeyedMapScope m{w, "backlight"};
        w.add("timeout", settings.backlight.timeoutS);
        w.add("gesture", settings.backlight.gesture);
    }

    {
        SDK::JsonStreamWriter::KeyedMapScope m{w, "sleep"};
        w.add("schedule", settings.sleep.schedule);
        w.add("active", settings.sleep.active);
        w.add("scheduledActive", settings.sleep.scheduledActive);
        w.add("alertsMutedBefore", settings.sleep.alertsMutedBefore);

        for (uint8_t d = 0; d < WatchSettingsLocal::Sleep::kDays; ++d) {
            SDK::JsonStreamWriter::KeyedMapScope day{w, kDayKeys[d]};
            w.add("start", settings.sleep.days[d].startMin);
            w.add("end", settings.sleep.days[d].endMin);
        }
    }

    w.endMap();

    if (w.isError()) {
        LOG_ERROR("Failed to create JSON\n");
        return 0;
    }

    return strlen(buff);
}

} /* namespace Settings */
