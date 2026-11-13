/**
 ******************************************************************************
 * @file    GuiBackendInterface.hpp
 * @brief   Link interface between GUI and Backend.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef SHARED_GUI_BACKEND_INTERFACE_HPP
#define SHARED_GUI_BACKEND_INTERFACE_HPP

#include <variant>
#include <vector>

#if !defined(SIMULATOR)

#include "PhoneNotification.hpp"
#include "NowPlaying.hpp"
#include "NowPlayingTracker.hpp"
#include "DeviceInfo.hpp"
#include "WatchSettings.hpp"
#include "WatchSettingsLocal.hpp"
#include "AppTypes.hpp"
#include "../App/IconPool.hpp"
#include "BleChannelsFCC.hpp"
#include "IBuzzer.hpp"
#include "IVibro.hpp"
#include "SystemError.hpp"
#include "AppLaunchFailure.hpp"
#include "HomeWidget.hpp"
#include "AppSurfaceInfo.hpp"
#else // defined(SIMULATOR)
#include "../../../../Libs/Header/Components/Shared/PhoneNotification.hpp"
#include "../../../../Libs/Header/Components/Shared/NowPlaying.hpp"
#include "../../../../Libs/Header/Components/Shared/NowPlayingTracker.hpp"
#include "../../../../Libs/Header/Components/Shared/DeviceInfo.hpp"
#include "../../../../Libs/Header/Components/Shared/WatchSettings.hpp"
#include "../../../../Libs/Header/Components/Shared/WatchSettingsLocal.hpp"
#include "../../../../Libs/Header/Components/Shared/AppTypes.hpp"
#include "../../../../Libs/Header/Components/App/IconPool.hpp"
#include "../../../../Libs/Header/Components/Shared/BleChannelsFCC.hpp"
#include "../../../../Libs/Header/Components/Shared/SystemError.hpp"
#include "../../../../Libs/Header/Components/Shared/AppLaunchFailure.hpp"
#include "../../../../Libs/Header/Components/Shared/HomeWidget.hpp"
#include "../../../../Libs/Header/Components/Shared/AppSurfaceInfo.hpp"
#include "../../../../Libs/Header/Interfaces/Components/IBuzzer.hpp"
#include "../../../../Libs/Header/Interfaces/Components/IVibro.hpp"
#endif // defined(SIMULATOR)


/**
 * @brief   Data types to send GUI --> Backend
 */
namespace G2BEvent
{

struct Shutdown {};

struct BacklightCtrl {
    bool state;
    uint32_t timeOn;
};

struct BuzzerCtrl {
    Interface::IBuzzer::Note notes[Interface::IBuzzer::skMaxNotes];
    uint8_t num;
};

struct VibroCtrl {
    Interface::IVibro::Note notes[Interface::IVibro::skMaxNotes];
    uint8_t num;
};

struct BleStartPairing {};
struct BleAbortPairing {};
struct BleUnpair {};
struct BleStartFCC {
    BleChannelsFCC::Info channel;
};
struct BleStopFCC {};

struct PhoneNotificationPositiveAction {
    std::shared_ptr<PhoneNotification> notification;
};

struct PhoneNotificationNegativeAction {
    std::shared_ptr<PhoneNotification> notification;
};

// Send a remote command to the phone media player (Ble::AMS::RemoteCommandID).
struct MediaRemoteCommand {
    uint8_t command;
};

struct FindPhone {
    bool enable;
};

struct RequestEpo {};

struct ResetSettings {};
struct Settings {
    WatchSettings settings;
};
struct SettingsLocal {
    WatchSettingsLocal settings;
};

/// Switch sleep mode by hand (the top menu). The backend owns the sleep state:
/// it applies the change and answers with SettingsLocal.
struct SleepModeSet {
    bool on;
};

/// Not from the GUI: the backend's minute timer posts it so the sleep schedule
/// is looked at on the backend thread, which owns the settings.
struct SleepScheduleCheck {};

// Request to get the list of apps
struct UserAppListReq {
    App::Type type;
};

/// Asks for ids alone, for a screen that shows the apps some other way than by
/// their icon and name.
struct UserAppIdsReq {
    App::Type type;
};

/**
 * @brief The apps whose icons the screen now up is showing.
 *
 * An app list carries no icons, because a menu draws only a few of however
 * many apps are installed. The menu names those few here and is sent their
 * icons back; it states the whole set on every step, so the two sides cannot
 * drift apart, and anything not named is free to be dropped.
 *
 * Ask for a little more than is on screen where the set is a window that
 * moves: an icon has to be in hand by the time it is drawn, and the reply
 * cannot arrive in the same frame as the request.
 */
struct UserAppIconWindow {
    static constexpr uint8_t kMax = App::kIconWindowMax;

    App::Type type;
    uint8_t   count;            ///< Entries in use, 0 to kMax
    uint64_t  appIds[kMax];
};
// Request to start an app
struct UserAppStart {
    uint32_t pid;

    /// Identifies THIS attempt, echoed back in UserAppStartFailed.
    ///
    /// The pid alone cannot do it: a launch the GUI has already given up on
    /// can still be answered later, and if the user has retried the same app
    /// in the meantime that answer is indistinguishable from the retry's own.
    /// Acting on it would clear the retry and show the old failure.
    uint32_t attempt;
};

// Request to start an all glance-apps
struct UserAppGlancesStart {};
// Request to stop an all glance-apps
struct UserAppGlancesStop {};
// Request to get the list of glances data
struct UserAppGlancesListReq {};

// Request to resume an app
struct UserAppResume {
    uint32_t pid;
};
// Request to pause an app
struct UserAppPause {
    uint32_t pid;
};
// Force stop an app
struct UserAppStop {
    uint32_t pid;
};

// Show one face and keep its neighbours ready, while the user picks a face by
// looking at it. The GUI owns the ordering -- including the built-in face,
// which is not an app and is asked for as id 0 -- and states the whole window
// on every step, so the two sides cannot drift apart. Any face outside the
// window is unloaded; the window is emptied before it is refilled, so at most
// three are in memory at once.
struct WatchFacePreview {
    uint64_t showAppId;   ///< face to put on screen, 0 for the built-in one
    uint64_t prevAppId;   ///< neighbour to keep loaded, 0 for none
    uint64_t nextAppId;   ///< neighbour to keep loaded, 0 for none
};

// Leave the picker: unload every face except the one that is now selected.
struct WatchFacePreviewEnd {
    uint64_t keepAppId;   ///< the selected face, 0 for the built-in one
};

struct MsdOpen {
    bool enable;
};
struct MsdClose {};

/**
 * @brief How often the screen now up needs a frame for its own sake.
 * @note  Zero says it draws only when something tells it to, so between those
 *        times it needs no frame at all. Whatever an app composites onto it is
 *        not the screen's to speak for, and asks for its own frames.
 * @note  A rate rather than a flag because that is what the sender knows. The
 *        pacing has one rate above zero to offer, so every non-zero value comes
 *        out the same today.
 */
struct ScreenFrameRate {
    uint32_t rate;
};

/**
 * @brief Whether the home screen is the one up.
 * @note  What decides whether a watch face app draws: a face is the home
 *        screen's face, and holds the frame surface for exactly as long as
 *        that screen is showing.
 */
struct HomeScreenShown {
    bool shown;
};

struct PlayError { bool enable; };
struct AcknowledgeError {};   // user dismissed the current error screen

// External accessories (Settings -> Sensors)
struct AccessorySetMode { uint8_t mode; };  // WatchSettingsAccessories::Mode
struct AccessoryScanStart {};
struct AccessoryScanStop {};

// Compass (Settings > Sensors > Compass)
//
// The feed runs for as long as the screen is up, so that a bearing is ready
// the moment one is asked for; a calibration is a separate run the user starts.
struct MagFeedStart {};   // screen opened: start delivering MagAzimuth
struct MagFeedStop {};    // screen closed: stop it, and end a run this screen started
// Start a calibration of at most 'seconds'. Answered by MagCalDone: when the
// run ends, or at once when it is refused.
struct MagCalStart { uint32_t seconds; };
// End this screen's run early. Answered by MagCalDone: from the run, or at once
// when this screen has no run to end.
struct MagCalCancel {};
struct AccessoryForget { uint8_t index; };
struct AccessoryRemember {
    char     name[24];
    uint8_t  addr[6];
    uint8_t  addrType;
    uint32_t kind;
};
// Internal (Backend-posted, NOT sent by the GUI): marshals an accessory status
// from the BLE event/timer thread onto the Backend thread, so forwarding it to
// the owner app touches the App::Manager app list only from the app-owning
// thread (avoids racing app load/stop).
struct AccessoryStatusForward {
    uint8_t state;     // SDK::Accessory::State
    char    name[24];
};

// Internal (Backend-posted, NOT sent by the GUI): a watch face has stopped.
// Reported from the thread the app was running on, so it is carried to the
// Backend thread rather than acted on where it is noticed.
struct WatchFaceStopped {
    uint32_t guiPid;
};

// Internal (Backend-posted, NOT sent by the GUI): at least one app has nothing
// loaded and nothing pending, and its instance can be freed. Carries no PID:
// one is posted for any number of apps, so a dozen glances exiting together
// take one slot in the queue rather than a dozen.
struct AppsIdle {
};

// Internal (Backend-posted, NOT sent by the GUI): a running service asked for
// its GUI and it did not fit. Reported from the app's own thread, so the room
// is made on the Backend thread.
struct AppGuiDidNotFit {
    uint32_t srvPid;
};

// Define the data types that can be sent from GUI to Backend
using Data = std::variant<
        Shutdown,

        BacklightCtrl,
        BuzzerCtrl,
        VibroCtrl,

        BleStartPairing,
        BleAbortPairing,
        BleUnpair,
        BleStartFCC,
        BleStopFCC,

        PhoneNotificationPositiveAction,
        PhoneNotificationNegativeAction,

        MediaRemoteCommand,

        FindPhone,
        RequestEpo,

        ResetSettings,
        Settings,
        SettingsLocal,
        SleepModeSet,
        SleepScheduleCheck,

        UserAppListReq,
        UserAppIdsReq,
        UserAppIconWindow,
        UserAppStart,
        UserAppGlancesStart,
        UserAppGlancesStop,
        UserAppGlancesListReq,
        UserAppResume,
        UserAppPause,
        UserAppStop,

        WatchFacePreview,
        WatchFacePreviewEnd,

        MsdOpen,
        MsdClose,
        ScreenFrameRate,
        HomeScreenShown,
        PlayError,
        AcknowledgeError,

        AccessorySetMode,
        AccessoryScanStart,
        AccessoryScanStop,
        AccessoryForget,
        AccessoryRemember,
        AccessoryStatusForward,

        MagFeedStart,
        MagFeedStop,
        MagCalStart,
        MagCalCancel,

        WatchFaceStopped,
        AppsIdle,
        AppGuiDidNotFit
        >;
};

/**
 * @brief   Data types to send Backend --> GUI
 */
namespace B2GEvent
{
struct Booted {};

/**
 * @brief   Announce where a running app's frame can be read from.
 * @note    Sent once, ahead of Booted.
 */
struct AppSurfaceConfig {
    AppSurfaceInfo surface;
};

/**
 * @brief   Which app is drawing the watch face.
 * @note    The GUI matches this against the surface owner to tell the face's
 *          frames from whatever was on the surface before. Zero when no face
 *          is selected or the selected one could not be loaded, which leaves
 *          the built-in face on screen.
 */
struct WatchFaceRunning {
    uint32_t guiPid;
};

struct DevInfo {
    DeviceInfo info;
};

struct BleBond {
    uint8_t count;
};
struct BleConnect {
    bool status;
};
struct BlePinCode {
    uint32_t code;
};
struct BlePair {
    bool status;
};
// A bonded device reconnected: the link was encrypted without a pin code
struct BleReconnect {
};

struct BatteryLevel {
    float level;
};
struct BatteryCharge {
    bool status;
};
// Charging refused by a protection rule (over-temperature). Unlike
// BatteryCharge, which is also false on a full battery, this means the watch
// will not charge while the cable stays connected.
struct ChargingBlocked {
    bool status;
};
struct LowBatteryShutdown {
};
struct UsbConnect {
    bool status;
};
struct UsbPortOpen {
    bool status;
};

struct MsdOpeningRequest {
};
// Mass storage is being opened (true) or has been closed (false). While open,
// the host owns the volume and the GUI must not let anything reach it.
struct MsdState {
    bool open;
};

struct Time {
    struct std::tm time;
};

struct FirmwareUpdate {
    bool status;
    uint8_t pct;
};

struct PhoneNotificationAdd {
    std::shared_ptr<PhoneNotification> notification;
    bool isNew;
};
struct PhoneNotificationRemove {
    uint32_t uid;
    bool all;
};

// Current Now Playing media state (cleared snapshot on disconnect).
struct MediaNowPlaying {
    std::shared_ptr<NowPlaying> nowPlaying;
};

// Request to open the media-control screen (e.g. a long L2 button press).
struct MediaControlRequest {};

struct FindPhoneConfirmation {};

struct Settings {
    WatchSettings settings;
};
struct SettingsLocal {
    WatchSettingsLocal settings;
};

// Request to get the list of apps
struct UserAppList {
    App::Type type;
    std::shared_ptr<std::vector<App::Item>> list;
};

struct UserAppIds {
    App::Type type;
    std::shared_ptr<std::vector<uint64_t>> ids;
};

/**
 * @brief The icons asked for by the last UserAppIconWindow.
 *
 * One entry per app that was asked for and found, in the order asked. A handle
 * is a counted reference to the pool, so a menu that keeps one keeps the set
 * alive -- which is what lets the loader drop it from its window without
 * pulling the pixels out from under a widget still drawing them. Carried by
 * value in a fixed array: the window is bounded, so this needs no allocation
 * and no shared_ptr across the task boundary.
 */
struct UserAppIcons {
    static constexpr uint8_t kMax = App::kIconWindowMax;

    App::Type type;
    uint8_t   count;              ///< Entries in use, 0 to kMax
    App::IconRef icons[kMax];
};
// Request to start an app
// Confirms a start. Carries the attempt so a start the BACKEND raised - an
// app asking for its own GUI, say - cannot consume the origin recorded for a
// launch the user is still waiting on. Those arrive with attempt 0.
struct UserAppStartReq {
    uint32_t pid;
    uint32_t attempt;
};
// Notify GUI that the app has stopped
struct UserAppStopped {
    uint32_t pid;
};
// Notify GUI about the capabilities of the app
struct UserAppCapabilities {
    uint32_t pid;
    App::Capabilities caps;
};

// Notify GUI about the glances data of the apps
struct UserAppGlanceList {
    std::shared_ptr<std::vector<App::GlanceData>> list;
};


struct GpsFix {
    bool status;
};

struct GpsEpoTime {
    uint32_t hoursLeft;
};

struct GpsEpoSync {
    bool state;
};


struct DailyHealth {
    uint32_t steps;
    uint32_t floors;
    uint32_t activityMinutes;
    uint32_t restingHeartRate;
    uint32_t averageHeartRate;
};

struct HeartRate {
    float heartRate;
};

struct Error {
    SystemError error;
};

/**
 * @brief A launch the user asked for did not happen.
 *
 *        Deliberately NOT routed through Error/SystemError: those are
 *        persistent hardware conditions, they reach the phone as a protocol
 *        value, and the GUI ranks them above low battery and pairing. A
 *        failed launch is transient, local and low severity.
 */
struct UserAppStartFailed {
    uint32_t           pid;

    /// The attempt this answers, copied from the UserAppStart that asked.
    /// Zero when the backend raised the failure on its own initiative, which
    /// belongs to no attempt the user made.
    uint32_t           attempt;

    App::LaunchFailure reason;
};

// External accessories (Settings -> Sensors)
struct AccessoryScanResult {
    char     name[24];
    uint8_t  addr[6];
    uint8_t  addrType;
    int8_t   rssi;
    uint32_t kind;
};
struct AccessoryStatus {
    uint32_t kind;
    uint8_t  state;   // SDK::Accessory::State
    char     name[24];
};
// Pushed on boot and after any change, so the settings screen can show the
// feature-enable flag and the remembered-accessory list.
struct AccessoryConfig {
    static constexpr uint8_t kMaxNames = 8;
    uint8_t mode;   // WatchSettingsAccessories::Mode (0=Off,1=Ext&Wrist,2=ExtOnly)
    uint8_t count;
    char    names[kMaxNames][24];
};

// Compass bearing of 12 o'clock, while the compass screen has the feed on,
// levelled with gravity so that it holds however the watch is tilted. 'valid'
// is false whenever there is no bearing: no magnetic field stream, an
// uncalibrated watch, too little horizontal field, 12 o'clock pointing near
// straight up or down, or no gravity to level with - none yet, too old, or the
// wrist moving for too long. While the wrist is moving none is sent, so the
// last one stands.
// 'faceTiltDeg' is how far the face is turned from facing up, 0..180, for the
// screen to decide whether a bearing can be read off it; it means something
// only when 'valid'.
struct MagAzimuth {
    float degrees;
    bool  valid;
    float faceTiltDeg;
};

// How far along a calibration run is, 0..100, as the run itself decides it.
struct MagCalProgress {
    uint8_t percent;
};

// The answer to a MagCalStart or MagCalCancel: the run ended, or there was no
// run to start or end. A cancel that crosses the run's own end is answered as
// if there were no run, and the run's own answer follows.
struct MagCalDone {
    bool completed;   // the run reached its end; false: ended early, refused, or no run
    bool stored;      // the fit was good enough to keep, and was kept
};

// Delivers the current home-screen widget state to the GUI (see HomeWidgetData).
struct HomeWidget {
    HomeWidgetData data;
};

// Define the data types that can be sent from Backend to GUI
using Data = std::variant<
        Booted,
        AppSurfaceConfig,
        WatchFaceRunning,
        DevInfo,

        BleBond,
        BleConnect,
        BlePinCode,
        BlePair,
        BleReconnect,

        BatteryLevel,
        BatteryCharge,
        ChargingBlocked,
        LowBatteryShutdown,
        UsbConnect,
        UsbPortOpen,
        MsdOpeningRequest,
        MsdState,

        Time,

        FirmwareUpdate,

        PhoneNotificationAdd,
        PhoneNotificationRemove,

        MediaNowPlaying,
        MediaControlRequest,

        FindPhoneConfirmation,

        Settings,
        SettingsLocal,

        UserAppList,
        UserAppIds,
        UserAppIcons,
        UserAppStartReq,
        UserAppStopped,
        UserAppCapabilities,
        UserAppGlanceList,

        GpsFix,
        GpsEpoTime,
        GpsEpoSync,
        DailyHealth,
        HeartRate,
        AccessoryScanResult,
        AccessoryStatus,
        AccessoryConfig,

        MagAzimuth,
        MagCalProgress,
        MagCalDone,

        HomeWidget,

        Error,
        UserAppStartFailed
        >;
};

/**
 * @class IGuiBackend
 * @brief Interface for GUI backend communication.
 *
 * This interface defines methods for receiving GUI events from the backend
 * and sending events to the backend.
 */
class IGuiBackend {
public:

    /**
     * @brief Receive GUI event from the backend.
     * @param data The event data to be received.
     * @return true if the event was successfully received, false otherwise.
     */
    virtual bool receiveGuiEvent(B2GEvent::Data& data) = 0;

    /**
     * @brief Send event to the backend.
     * @param data The event data to be sent.
     * @return true if the event was successfully sent, false otherwise.
     */
    virtual bool sendEventToBackend(const G2BEvent::Data& data) = 0;

    /**
     * @brief Default destructor.
     */
    virtual ~IGuiBackend() = default;

};

/**
 * @brief Holder class for unlink Backend implementation and GUI.
 */
class GuiBackendHolder {

public:

    /**
     * @brief   Set real interface instance.
     * @param   ifc: IGuiBackend instance.
     */
    static void Set(IGuiBackend &ifc)
    {
        Real() = &ifc;
    }

    /**
     * @brief   Get GuiBackendInterface instance.
     * @note    The interface must be set first with 'SetInterface' method.
     * @retval  IGuiBackend instance.
     */
    static IGuiBackend& Get()
    {
        // Always return a valid interface
        if (Real() == nullptr) {
            return Dummy();
        } else {
            return *Real();
        }
    }

private:

    class DummyIfc : public IGuiBackend
    {
    public:
        virtual bool receiveGuiEvent(B2GEvent::Data &data) override {return false;}
        virtual bool sendEventToBackend(const G2BEvent::Data &data) override {return true;}
        virtual ~DummyIfc() = default;
    };

    // Holder functions for static field to avoid CPP file
    static IGuiBackend& Dummy() {
        static DummyIfc dummy;
        return dummy;
    }

    static IGuiBackend*& Real() {
        static IGuiBackend *spReal;
        return spReal;
    }

};

#endif // SHARED_GUI_BACKEND_INTERFACE_HPP
