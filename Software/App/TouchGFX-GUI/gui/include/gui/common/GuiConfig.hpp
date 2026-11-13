/**
 ******************************************************************************
 * @file    GuiConfig.hpp
 * @brief   Configuration for GUI.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __GUI_CONFIG_HPP
#define __GUI_CONFIG_HPP

#include <cstdint>
#include <cstdbool>

#include "touchgfx/Color.hpp"
#include <texts/TextKeysAndLanguages.hpp>

// App::kFrameRate from AppTypes.hpp
#define GUI_CONFIG_MS_2_TICKS(ms) ((ms)/(1000 / App::kFrameRate))


namespace Gui
{

namespace Config
{

constexpr int32_t kMenuAnimationSteps = 4;

constexpr uint32_t kScreenTimeoutSteps      = GUI_CONFIG_MS_2_TICKS(30000);     // 30s
constexpr uint32_t kSavedScreenTimeout      = GUI_CONFIG_MS_2_TICKS(1000);      // 1s;
constexpr uint32_t kTitleInfoSwitchPeriod   = GUI_CONFIG_MS_2_TICKS(3000);      // 3s;
constexpr uint32_t kPairCodeScreenTimeout   = GUI_CONFIG_MS_2_TICKS(60000);     // 60s;
constexpr uint32_t kPairedScreenTimeout     = GUI_CONFIG_MS_2_TICKS(1000);      // 1s;
constexpr uint32_t kShutdownScreenTimeout   = GUI_CONFIG_MS_2_TICKS(500);       // 0.5s;
constexpr uint32_t kLowBattShutdownScreenTimeout = GUI_CONFIG_MS_2_TICKS(10*1000);   // 10s;
constexpr uint32_t kLowBattScreenTimeout    = GUI_CONFIG_MS_2_TICKS(5 * 1000);  // 5s;
constexpr uint32_t kGlanceScreenTimeout     = GUI_CONFIG_MS_2_TICKS(60 * 1000); // 60s;
constexpr uint32_t kErrorPlayPeriod         = GUI_CONFIG_MS_2_TICKS(1 * 1000);  // 1s;
// How long the "could not start" message stays up before returning to the
// launcher. Longer than kSavedScreenTimeout because this one has to be read
// rather than merely noticed - it is a sentence, and it is unexpected.
constexpr uint32_t kAppStartFailedTimeout   = GUI_CONFIG_MS_2_TICKS(3 * 1000);  // 3s;

// How long a recorded app-launch origin waits for the backend to confirm the
// start before it is dropped. A launch that is never confirmed must not leave
// an origin behind for some later, unrelated start to claim.
//
// MUST exceed the backend's worst case for one launch, which is NOT just the
// 3000 ms passed to Application::run: create() can spend another 1000 ms in
// AppInstance::init() first, so roughly 4 s. Below that the timer fires on a
// launch that is merely slow and the user is told it failed as it opens.
// Model::appStart() refuses to queue a second launch while one is pending,
// which is what keeps this a single launch's worst case rather than two.
constexpr uint32_t kAppLaunchOriginTimeout  = GUI_CONFIG_MS_2_TICKS(5 * 1000);  // 5s;

namespace Button
{
constexpr uint8_t L1 = '1';
constexpr uint8_t L2 = '2';
constexpr uint8_t R1 = '3';
constexpr uint8_t R2 = '4';
constexpr uint8_t L1R2 = 'z';

/// A button going down and coming up again, for a screen that acts while one
/// is held rather than on the click a short release turns into. Mirrors
/// Interface::IButtons::Code -- keep the two in step.
constexpr uint8_t L1_PRESS = '5';
constexpr uint8_t L2_PRESS = '6';
constexpr uint8_t R1_PRESS = '7';
constexpr uint8_t R2_PRESS = '8';

constexpr uint8_t L1_RELEASE = '%';
constexpr uint8_t L2_RELEASE = '^';
constexpr uint8_t R1_RELEASE = '&';
constexpr uint8_t R2_RELEASE = '*';
} // Button

namespace Color
{
inline const touchgfx::colortype WHITE          = 0xC0C0C0;
inline const touchgfx::colortype GRAY           = 0x808080;
inline const touchgfx::colortype GRAY_DARK      = 0x404040;
inline const touchgfx::colortype BLACK          = 0x000000;
inline const touchgfx::colortype YELLOW_DARK    = 0xC08000;
inline const touchgfx::colortype TEAL           = 0x008080;
inline const touchgfx::colortype TEAL_DARK      = 0x004040;
inline const touchgfx::colortype GREEN          = 0x00C000;
inline const touchgfx::colortype DARK_GREEN     = 0x004000;
inline const touchgfx::colortype BROUN          = 0xC08040;
inline const touchgfx::colortype RED            = 0xC00000;
inline const touchgfx::colortype DARK_RED       = 0x400000;
inline const touchgfx::colortype BLUE           = 0x0000C0;
inline const touchgfx::colortype DARK_BLUE      = 0x000040;
inline const touchgfx::colortype CYAN           = 0x40C0C0;
inline const touchgfx::colortype CYAN_LIGHT     = 0x80C0C0;

// Menu colors
inline const touchgfx::colortype MENU_NOT_SELECTED_TIP_ON  = YELLOW_DARK;
inline const touchgfx::colortype MENU_NOT_SELECTED_TIP_OFF = TEAL;

} // namespace Color

namespace Battery
{
// Battery Thresholds 0 - Lo - Hi - 100
constexpr uint8_t kThresholdHi = 85;
constexpr uint8_t kThresholdLo = 25;

constexpr uint8_t kLowBatteryThreshold = 5;
} // Battery

namespace DateTime
{
/**
 * @brief   Days of the week.
 * @note    The order of days is important, it is used in the menu logic.
 */
static constexpr touchgfx::TypedTextId DayShort[7] = {
        T_TEXT_SUN, T_TEXT_MON, T_TEXT_TUE, T_TEXT_WED,
        T_TEXT_THU, T_TEXT_FRI, T_TEXT_SAT };

/**
 * @brief   Months of the year.
 * @note    The order of months is important, it is used in the menu logic.
 */
static constexpr touchgfx::TypedTextId MonthShort[12] = {
        T_TEXT_JAN, T_TEXT_FEB,
        T_TEXT_MAR, T_TEXT_APR, T_TEXT_MAY,
        T_TEXT_JUN, T_TEXT_JUL, T_TEXT_AUG,
        T_TEXT_SEP, T_TEXT_OCT, T_TEXT_NOV,
        T_TEXT_DEC, };

} // DateTime

/**
 * @brief   Where a user app was launched from.
 * @note    Exiting an app returns to its launch point, so "back" out of an app
 *          behaves like "back" out of the settings menu instead of always
 *          dropping to the watch face.
 */
enum class AppLaunchOrigin : uint8_t {
    HOME = 0,       ///< Unknown / not launched from a menu - fall back to the watch face
    TOP_MENU,       ///< Utility apps, launched from the top menu
    TRACKING_MENU,  ///< Activity apps, launched from the tracking menu
};

namespace Menu
{

namespace Top
{
/**
 * @brief   Top menu items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_POWEROFF = 0,
    ID_SETTINGS,
    ID_ALERTS,
    ID_SLEEP,
    //ID_FIND_PHONE,

    ID_COUNT,   // How many entries the menu has that are not apps

    /// "Wherever Settings is", asked for by the Home screen. Not an item id:
    /// apps sit above Settings now (Alarm first), so which item it is depends
    /// on what is installed. TopMenuView resolves it against the live order.
    ID_DEFAULT = 0xFFFF,
};


namespace Settings
{
/**
 * @brief   Settings menu items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_CLOCK = 0,
    ID_WATCH_FACE,
    ID_BACKLIGHT,
    ID_PHONE,
    ID_SOUNDS,
    ID_VIBRATION,
    ID_GNSS,
    ID_SENSORS,
    ID_DEVICE,
    ID_RESET,

    ID_COUNT,   // Number of items in the settings menu
    ID_DEFAULT = ID_CLOCK,
};


namespace Clock
{
/**
 * @brief   Clock settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    // ID_SECONDS is temporarily hidden: the watch face has no layout for
    // seconds yet, so the setting is still stored and carried to the screen
    // but is not offered here. To restore, re-add ID_SECONDS below ID_TIME_FORMAT
    // and bring its toggle item back in the view and the presenter.
    ID_TIME_FORMAT = 0,
    ID_DATE_FORMAT,

    ID_COUNT,   // Number of items in the clock settings menu
    ID_DEFAULT = ID_TIME_FORMAT,
};
} // namespace Clock

namespace Backlight
{
/**
 * @brief   Backlight settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_GESTURE = 0,
    ID_TIMEOUT,
    ID_SLEEP_MODE,

    ID_COUNT,   // Number of items in the backlight settings menu
    ID_DEFAULT = ID_GESTURE,
};

namespace Sleep
{
/**
 * @brief   Sleep mode settings items: the schedule switch, then one item per
 *          day, Monday first, in WatchSettingsLocal::Sleep::days order.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_SCHEDULE = 0,
    ID_FIRST_DAY,
    ID_LAST_DAY = ID_FIRST_DAY + 6,

    ID_COUNT,   // Number of items in the sleep mode menu
    ID_DEFAULT = ID_SCHEDULE,
};

/**
 * @brief   Which days an edited sleep window is applied to.
 * @note    The order is the order of the "apply to" list.
 */
enum ApplyTo {
    APPLY_THIS_DAY = 0,
    APPLY_WEEKDAYS,
    APPLY_WEEKEND,
    APPLY_ALL_DAYS,

    APPLY_COUNT,
};
} // namespace Sleep
} // namespace Backlight

namespace Phone
{
/**
 * @brief   Phone settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_UNPAIR = 0,
    ID_PAIR,

    ID_COUNT,   // Number of items in the phone settings menu
    ID_DEFAULT = ID_UNPAIR,
};
} // namespace Phone

namespace Sounds
{
/**
 * @brief   Sounds settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_BUTTONS = 0,
    ID_MSG_AND_CALLS,
    ID_ALERTS,

    ID_COUNT,   // Number of items in the sounds settings menu
    ID_DEFAULT = ID_BUTTONS,
};
} // namespace Sounds

namespace Vibration
{
/**
 * @brief   Vibration settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_BUTTONS = 0,
    ID_MSG_AND_CALLS,
    ID_ALERTS,

    ID_COUNT,   // Number of items in the vibration settings menu
    ID_DEFAULT = ID_BUTTONS,
};
} // namespace Vibration

namespace GNSS
{
/**
 * @brief   GNSS settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_GPS_ASSIST = 0,

    ID_COUNT,   // Number of items in the GNSS settings menu
    ID_DEFAULT = ID_GPS_ASSIST,
};
} // namespace GNSS

namespace Device
{
/**
 * @brief   Device settings items.
 * @note    The order of items is important, it is used in the menu logic.
 */
enum Id {
    ID_DEVICE_ID = 0,
    ID_SERIAL_NUM,
    ID_SOFTWARE_VER,
    ID_HARDWARE_VER,
    ID_BLUETOOTH,
    ID_FCC_ID,
    ID_CE,

    ID_COUNT,   // Number of items in the device settings menu
    ID_DEFAULT = ID_DEVICE_ID,
};
} // namespace Device

} // namespace Settings

} // namespace Top

} // namespace Menu

} // namespace Config


namespace Utils
{
/**
 * @brief   Convert seconds to hours, minutes and seconds.
 * @param   sec: Seconds to convert.
 * @param   h: Reference to store hours.
 * @param   m: Reference to store minutes.
 * @param   s: Reference to store seconds.
 */
inline void sec2hms(uint32_t sec, uint16_t& h, uint8_t& m, uint8_t& s)
{
    h = static_cast<uint16_t>(sec / 3600);
    sec %= 3600;
    m = static_cast<uint8_t>(sec / 60);
    s = static_cast<uint8_t>(sec % 60);
}

/**
 * @brief   Convert seconds to hours.
 * @param   sec: Seconds to convert.
 * @return  Hours.
 */
inline uint16_t sec2hmsH(uint32_t sec)
{
    uint16_t h;
    uint8_t m;
    uint8_t s;
    sec2hms(sec, h, m, s);
    return h;
}

/**
 * @brief   Convert seconds to minutes.
 * @param   sec: Seconds to convert.
 * @return  Minutes.
 */
inline uint8_t sec2hmsM(uint32_t sec)
{
    uint16_t h;
    uint8_t m;
    uint8_t s;
    sec2hms(sec, h, m, s);
    return m;
}

/**
 * @brief   Convert seconds to seconds.
 * @param   sec: Seconds to convert.
 * @return  Seconds.
 */
inline uint8_t sec2hmsS(uint32_t sec)
{
    uint16_t h;
    uint8_t m;
    uint8_t s;
    sec2hms(sec, h, m, s);
    return s;
}

} // namespace Utils

} // namespace Gui

#endif /* __GUI_CONFIG_HPP */
