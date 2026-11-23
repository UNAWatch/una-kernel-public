/**
 ******************************************************************************
 * @file    WatchSettingsLocal.hpp
 * @brief   Local watch settings configuration structure.
 ******************************************************************************
 * This file contains the definition of the "WatchSettingsLocal" structure,
 * which represents watch-specific settings accessible only from the watch
 * itself.
 ******************************************************************************
 */

#ifndef __WATCH_SETTINGS_LOCAL_HPP
#define __WATCH_SETTINGS_LOCAL_HPP

#include <cstdint>
#include <vector>
#include <string>

/**
 * @brief Local device settings configuration structure.
 * @note Contains settings accessible only from the watch interface, not from mobile app.
 */
struct WatchSettingsLocal {

    /// Settings version.
    static constexpr uint8_t kVersion = 5;

    /**
     * @brief GPS power mode enumeration.
     * @note Defines different GPS power consumption and accuracy modes.
     */
    enum GpsPowerMode {
        GPS_PM_STANDARD,    ///< Power saving mode with reduced GPS accuracy.
        GPS_PM_PRECISION,   ///< High precision mode with increased power consumption.

        GPS_PM_COUNT        ///< Total number of GPS power modes (for iteration).
    };

    /**
     * @brief Clock settings structure.
     * @note Controls how the watch face presents the current time.
     */
    struct Clock {

        /**
         * @brief Time format enumeration.
         */
        enum Format {
            FMT_24H,    ///< 24-hour format.
            FMT_12H,    ///< 12-hour format with an AM/PM suffix.

            FMT_COUNT   ///< Total number of time formats (for iteration).
        };

        /**
         * @brief Date order enumeration (weekday always comes first).
         */
        enum DateOrder {
            DATE_DMY,   ///< Day before month, e.g. "Mon 20 Jul".
            DATE_MDY,   ///< Month before day, e.g. "Mon Jul 20".

            DATE_ORDER_COUNT   ///< Total number of date orders (for iteration).
        };

        Format format = FMT_24H;            ///< Time format of the watch face.
        bool seconds = false;               ///< Show seconds next to hours and minutes.
        DateOrder dateOrder = DATE_DMY;     ///< Day/month order on the watch face.

        /**
         * @brief Equality operator to compare two clock configurations.
         * @param other The other Clock object to compare.
         * @return 'true' if the configurations are equal, 'false' otherwise.
         */
        bool operator==(const Clock &other) const
        {
            return (format == other.format &&
                    seconds == other.seconds &&
                    dateOrder == other.dateOrder);
        }

        /**
         * @brief Inequality operator to compare two clock configurations.
         * @param other The other Clock object to compare.
         * @return 'true' if the configurations are not equal, 'false' otherwise.
         */
        bool operator!=(const Clock &other) const
        {
            return !(*this == other);
        }
    };

    /**
     * @brief Notification settings structure.
     * @note Controls feedback settings for different device events.
     */
    struct Notification {
        bool buttons = false;   ///< Enable feedback for button presses.
        bool msgCalls = false;  ///< Enable feedback for messages and calls.
        bool alerts = false;    ///< Enable feedback for system alerts and notifications.

        /**
         * @brief Equality operator to compare two notification configurations.
         * @param other The other Notification object to compare.
         * @return 'true' if the configurations are equal, 'false' otherwise.
         */
        bool operator==(const Notification &other) const
        {
            return (buttons == other.buttons &&
                    msgCalls == other.msgCalls &&
                    alerts == other.alerts);
        }

        /**
         * @brief Inequality operator to compare two notification configurations.
         * @param other The other Notification object to compare.
         * @return 'true' if the configurations are not equal, 'false' otherwise.
         */
        bool operator!=(const Notification &other) const
        {
            return !(*this == other);
        }
    };

    /**
     * @brief Backlight settings structure.
     */
    struct Backlight {
        /// Seconds the backlight stays on after a button press, a wrist tilt,
        /// USB plug/unplug or boot. One of kTimeoutsS.
        static constexpr uint8_t kTimeoutsS[] = { 5, 10, 15 };
        static constexpr uint8_t kTimeoutCount = sizeof(kTimeoutsS) / sizeof(kTimeoutsS[0]);

        uint8_t timeoutS = kTimeoutsS[0];

        /// Lift-to-wake: a wrist tilt lights the backlight. Off saves the IMU's
        /// gesture detection for those who would rather have the battery life.
        /// Apps' own tilt detection is not affected.
        bool gesture = true;

        bool operator==(const Backlight &other) const
        {
            return (timeoutS == other.timeoutS &&
                    gesture == other.gesture);
        }

        bool operator!=(const Backlight &other) const
        {
            return !(*this == other);
        }
    };

    /**
     * @brief Sleep mode settings and state.
     * @note  Sleep mode turns lift-to-wake off and mutes alerts. It is switched
     *        by hand from the top menu, or by a per-day schedule.
     */
    struct Sleep {
        static constexpr uint8_t  kDays = 7;            ///< Index 0 is Monday.
        static constexpr uint16_t kMinutesPerDay = 24 * 60;

        /**
         * @brief One day's sleep window, in minutes after local midnight.
         * @note  A window whose end is not after its start runs past midnight
         *        and belongs to the day it starts on. Equal start and end is an
         *        empty window.
         */
        struct Window {
            uint16_t startMin = 22 * 60;
            uint16_t endMin = 7 * 60;

            bool operator==(const Window &other) const
            {
                return (startMin == other.startMin && endMin == other.endMin);
            }

            bool operator!=(const Window &other) const
            {
                return !(*this == other);
            }
        };

        bool schedule = false;          ///< Switch sleep mode by the windows below.
        Window days[kDays] {};          ///< Monday first.

        // State, owned by the backend: the GUI shows it but never writes it.
        bool active = false;            ///< Sleep mode is on now.
        /// What the schedule last asked for. Sleep mode changes only when this
        /// does, so a change made by hand holds until the next scheduled one.
        bool scheduledActive = false;
        /// alertMutedFlag as it was when sleep mode started; put back at the end.
        bool alertsMutedBefore = false;

        bool operator==(const Sleep &other) const
        {
            for (uint8_t d = 0; d < kDays; ++d) {
                if (days[d] != other.days[d]) {
                    return false;
                }
            }
            return (schedule == other.schedule &&
                    active == other.active &&
                    scheduledActive == other.scheduledActive &&
                    alertsMutedBefore == other.alertsMutedBefore);
        }

        bool operator!=(const Sleep &other) const
        {
            return !(*this == other);
        }
    };

    uint32_t version = kVersion;                    ///< Settings version
    /// Mute alerts the watch raises at the user -- phone notifications, calls
    /// and low battery -- across sound, vibration and the backlight. Feedback
    /// an app requests during a session it owns (e.g. interval phase cues) is
    /// deliberately not affected.
    bool alertMutedFlag = false;
    Notification vibro;                             ///< Vibration notification settings.
    Notification sound;                             ///< Sound notification settings.
    GpsPowerMode gpsPowerMode = GPS_PM_STANDARD;    ///< GPS power consumption mode setting.
    Clock clock;                                    ///< Watch face clock settings.

    // Added in version 3
    /// uappID of the Clockface app that draws the watch face, 0 for the built-in one.
    uint64_t watchfaceAppId = 0;

    // Added in version 4
    Backlight backlight;                            ///< Backlight settings.
    Sleep sleep;                                    ///< Sleep mode settings and state.

    // Added in version 5
    /// Developer mode: the USB serial console accepts the commands that drive
    /// the watch (button presses, screenshots, app launch, injected sensor
    /// data, log streaming). Switched on the watch, in Settings > Device.
    bool developerMode = false;

    /**
     * @brief Equality operator to compare two local settings configurations.
     * @param other The other SettingsLocal object to compare.
     * @return 'true' if the configurations are equal, 'false' otherwise.
     *
     * Compares all local settings fields to determine equality.
     */
    bool operator==(const WatchSettingsLocal &other) const
    {
        return (alertMutedFlag == other.alertMutedFlag &&
                vibro == other.vibro &&
                sound == other.sound &&
                gpsPowerMode == other.gpsPowerMode &&
                clock == other.clock &&
                watchfaceAppId == other.watchfaceAppId &&
                backlight == other.backlight &&
                sleep == other.sleep &&
                developerMode == other.developerMode);
    }

    /**
     * @brief Inequality operator to compare two local settings configurations.
     * @param other The other SettingsLocal object to compare.
     * @return 'true' if the configurations are not equal, 'false' otherwise.
     */
    bool operator!=(const WatchSettingsLocal &other) const
    {
        return !(*this == other);
    }
};

#endif // __WATCH_SETTINGS_LOCAL_HPP
