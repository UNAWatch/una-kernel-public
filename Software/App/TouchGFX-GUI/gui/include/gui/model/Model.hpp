#ifndef MODEL_HPP
#define MODEL_HPP

#include <vector>
#include <algorithm>

#include "touchgfx/UIEventListener.hpp"

#if !defined(SIMULATOR)
#include "GuiBackendInterface.hpp"
#include "HeapTelemetry.hpp"
#else // defined(SIMULATOR)
#include "../../../../Libs/Header/Components/Shared/GuiBackendInterface.hpp"
#include "../../../../Libs/Header/Components/Shared/HeapTelemetry.hpp"
#endif // defined(SIMULATOR)

#include "gui/common/GuiConfig.hpp"

class FrontendApplication;
class ModelListener;

class Model : public touchgfx::UIEventListener {
public:
    Model();

    void bind(ModelListener *listener);

    void tick();
    virtual void handleKeyEvent(uint8_t c);

    // Controls
    FrontendApplication& application();
    void switchToNextPriorityScreen();
    void resetIdleTimer();
    uint32_t getTimeMs();   // TODO: remove it

    // Menu position
    void setPosMenuTop(uint16_t p);
    uint16_t getPosMenuTop();
    void setPosMenuTopSettings(uint16_t p);
    uint16_t getPosMenuTopSettings();
    void setPosMenuTracking(uint16_t p);
    uint16_t getPosMenuTracking();
    void setPosNotificationListUid(uint32_t p);
    uint32_t getPosNotificationListUid();
    void setPosMenuGlances(uint16_t p);
    uint16_t getPosMenuGlances();
    void setPosMenuBleFCC(uint16_t p);
    uint16_t getPosMenuBleFCC();
    void setPosMenuGNSS(uint16_t p);
    uint16_t getPosMenuGNSS();
    void setPosMenuClock(uint16_t p);
    uint16_t getPosMenuClock();
    void setPosMenuSensors(uint16_t p);
    uint16_t getPosMenuSensors();
    void setPosMenuBacklight(uint16_t p) { mMenuPosition.backlight = p; }
    uint16_t getPosMenuBacklight() const { return mMenuPosition.backlight; }
    void setPosMenuSleep(uint16_t p) { mMenuPosition.sleep = p; }
    uint16_t getPosMenuSleep() const { return mMenuPosition.sleep; }

    // Power
    void shutdown();

    // Device information
    const DeviceInfo& getDeviceInfo();

    /**
     * @brief Where a running app's frame can be read from.
     * @note  pixels is null until the backend has announced it, which it
     *        does before Booted. Null in the simulator, where no app runs.
     */
    const AppSurfaceInfo& getAppSurface() const { return mAppSurface; }

    // Date/Time
    void getDate(uint8_t &m, uint8_t &d, uint8_t &wd);
    void getTime(uint8_t &h, uint8_t &m, uint8_t &s);
    std::tm getDateTime();

    // Battery
    uint8_t batteryGetLevel();
    bool batteryIsLow();
    bool batteryIsShutdown();
    void batteryPlayLow();
    void batteryMuteLow();
    bool batteryIsMuted();

    // USB
    bool usbIsConnected();
    bool usbIsPortOpened();
    bool usbIsCharging();
    bool chargingIsBlocked();
    bool chargingDismiss();
    bool chargingIsDismissed();

    // BLE
    bool bleIsBonded();
    uint8_t bleGetBondedCount();
    bool bleIsConnected();
    uint32_t bleGetPinCode();
    void bleStartPairing();
    void bleAbortPairing();
    void bleUnpair();
    bool bleIsPairingActive();
    bool bleIsPairingManual();
    bool bleIsReconnected();
    void bleStartFCC(const BleChannelsFCC::Info& ch);
    void bleStopFCC();

    // BLE Phone notification
    void phoneNotificationPlay();
    void phoneNotificationStop();
    bool phoneNotificationIsActive();
    void phoneNotificationClearAll();
    std::shared_ptr<PhoneNotification> phoneNotificationGetByUID(uint32_t uid);
    std::shared_ptr<PhoneNotification> phoneNotificationGetNextActive();
    const std::vector<std::shared_ptr<PhoneNotification>>& phoneNotificationGetAll() const;
    void phoneNotificationPositiveAction(const std::shared_ptr<PhoneNotification> &notification);
    void phoneNotificationNegativeAction(const std::shared_ptr<PhoneNotification> &notification);
    void phoneNotificationSetUidForDetails(uint32_t uid);
    uint32_t phoneNotificationGetUidForDetails();

    // Media (Now Playing)
    std::shared_ptr<NowPlaying> mediaGetNowPlaying() const;
    void mediaRemoteCommand(MediaCommand cmd);

    // Latest home-screen widget pushed by a running app, cached so the home face
    // can pull the current state when it is (re)entered. Held as the Shared
    // HomeWidgetData (not the B2GEvent envelope); percent stays float, rounded to
    // the bar's uint8 at render.
    const HomeWidgetData& homeWidget() const { return mHomeWidget; }

    // Settings
    const WatchSettings& getCommonSettings();
    const WatchSettingsLocal& getLocalSettings();
    void saveLocalSettings(const WatchSettingsLocal &sett);
    void resetSettings();

    // Selects which value the shared clock "FORMAT" picker edits: the time
    // format (false) or the date order (true). Set before opening the picker.
    void setClockPickerDate(bool date) { mClockPickerDate = date; }
    bool isClockPickerDate() const { return mClockPickerDate; }

    // Switch sleep mode by hand. The backend owns the state and answers with
    // the local settings, which reach listeners through onLocalSettings().
    void setSleepMode(bool on);

    // Which day the sleep window editor edits, 0 for Monday. Set before opening it.
    void setSleepEditDay(uint8_t day) { mSleepEditDay = day; }
    uint8_t getSleepEditDay() const { return mSleepEditDay; }

    // Apps
    void appRequestList(App::Type type);

    /// An app list and its icons as last fetched -- see appCache().
    struct AppCache {
        std::shared_ptr<std::vector<App::Item>> list;

        /// Counted references into the icon pool, so holding them here keeps
        /// the sets alive whatever the loader does with its own window.
        App::IconRef icons[App::kIconWindowMax];
        uint8_t      iconCount = 0;

        /// The window last asked for, so the same one is not asked for twice.
        /// Opening a menu otherwise asks two or three times over -- once from
        /// the cache it is served, once from the list that follows -- and every
        /// reply in flight pins the icons it carries.
        uint64_t asked[G2BEvent::UserAppIconWindow::kMax] {};
        uint8_t  askedCount = 0;
    };

    /**
     * @brief   What is already known about one type's apps, or nullptr for a
     *          type nothing holds.
     *
     * Kept here rather than in the screen because screens are built and
     * destroyed as they are shown: a menu asking for its list on the way in
     * cannot have it before its first frame, which is what made these menus
     * assemble themselves in stages on screen. Serving this first makes an
     * opening whole from the first frame, and the request that follows keeps it
     * honest.
     *
     * Cheap to hold now that a list carries no pixels -- a hundred apps is a
     * few kilobytes -- and the icons are shared with whoever else holds them,
     * so this pins only what nothing else already has. Safe to keep for the
     * session: the app registry is built by the boot scan and does not change
     * under us.
     */
    const AppCache* appCache(App::Type type) const;

    /**
     * @brief   Say which apps' icons the screen is showing, and get them back.
     * @param   type: the list these apps came from.
     * @param   appIds: the apps wanted, in the order the reply should carry.
     * @param   count: how many, capped at G2BEvent::UserAppIconWindow::kMax.
     * @note    An app list carries no icons. State the whole set every time it
     *          changes -- anything left out is dropped -- and include a little
     *          more than is on screen, because the reply cannot arrive in the
     *          frame that asks for it. Answered by onAppIcons().
     */
    void appRequestIconWindow(App::Type type, const uint64_t *appIds, uint8_t count);

    /**
     * @brief   Fetch the icons the menu for @p type will want when it opens.
     * @note    A hint, not the authority: a menu asks for its exact window as
     *          it opens. This only has to get the pixels here first.
     *          The top menu's arc is a window over a list that can be long, so
     *          for utilities this follows the saved position; the tracking
     *          wheel holds no more rows than one window carries, so for
     *          activities it is simply the head of the list.
     */
    void prefetchIcons(App::Type type);

    /**
     * @brief   Ask for the ids of the installed apps of one type.
     * @note    For a screen that shows the apps some other way than by their
     *          icon and name; the answer costs eight bytes each rather than
     *          four and a half kilobytes.
     */
    void appRequestIds(App::Type type);
    /**
     * @brief   Start (or bring to the front) a user app.
     * @param   pid     GUI process ID of the app.
     * @param   origin  Screen the launch came from; exiting the app returns
     *                  there. Defaults to the watch face for launches that do
     *                  not come from a menu.
     */
    bool appStart(uint32_t pid,
                  Gui::Config::AppLaunchOrigin origin = Gui::Config::AppLaunchOrigin::HOME);
    bool appGlancesStart();
    bool appGlancesStop();
    bool appGlancesUpdate();
    bool appResume();
    void appPause();
    void appStopAll();
    uint32_t appGetActivePID();

    // WatchFace APP
    /**
     * @brief   Say how often this screen needs a frame for its own sake.
     * @note    Only about what the screen itself draws. An app drawing onto it
     *          has its own need, which the backend adds.
     * @param   rate: frames per second, zero when it draws only on an event.
     */
    void setScreenFrameRate(uint32_t rate);

    /**
     * @brief   Say whether the home screen is the one up.
     * @note    A statement of fact, not a request: what a watch face does
     *          about it is the backend's to decide.
     * @param   shown: true on the way in, false on the way out.
     */
    void setHomeScreenShown(bool shown);

    /**
     * @brief   Say that the screen now up uses a held L2 for itself.
     * @param   claimed: true on the way in, false on the way out.
     * @note    A held L2 otherwise opens the media controls, decided by a timer
     *          in the backend and arriving here as a request. A screen that
     *          scrolls for as long as the button is down has to be able to
     *          refuse it, or letting go would drop the user into the music
     *          screen. Answering here rather than not arming the timer keeps
     *          the decision with the side that knows what is on screen.
     */
    void claimL2Hold(bool claimed);

    /**
     * @brief   Put a face on screen while the user picks one, keeping its
     *          neighbours ready so stepping to them is immediate.
     * @param   show, prev, next: app IDs; 0 means the built-in face / none.
     */
    void watchFacePreview(uint64_t show, uint64_t prev, uint64_t next);

    /**
     * @brief   Leave the picker, keeping only the selected face loaded.
     * @param   keep: app ID of the selected face, 0 for the built-in one.
     */
    void watchFacePreviewEnd(uint64_t keep);

    /**
     * @brief   Store the selected watch face.
     * @param   appId: app ID of the face, 0 for the built-in one.
     */
    void saveWatchFace(uint64_t appId);

    /// App ID of the stored watch face, 0 when the built-in one is selected.
    uint64_t getWatchFace() const { return mSettings.local.watchfaceAppId; }

    /**
     * @brief   GUI process drawing the watch face, 0 when the built-in one is.
     * @note    Held here because a screen is built anew every time it is
     *          entered, while this outlives them: a face-hosting screen can
     *          say whose frames to expect before its first draw instead of
     *          waiting for the backend to answer, which is a frame of the
     *          built-in face nobody asked for.
     */
    uint32_t getWatchFacePid() const { return mWatchFacePid; }

    // Firmware update
    bool firmwareUpdateActive();
    uint8_t firmwareUpdateGetPct();

    // Find phone
    void ringing(bool state);









    // GPS
    bool getGpsSignal() { return mGpsSignalAcquired; }
    void requestEpoUpdate();
    uint32_t getEpoTimeH() { return mGpsEpoTimeLeft; }
    bool getGpsEpoSyncState() { return mGpsEpoSyncState; }

    // HR
    uint32_t getHR();
    uint32_t getAvgHR();
    uint32_t getRHR();
    uint32_t getDailyActivityTime();
    uint32_t getDailySteps();
    uint32_t getDailyFloors();

    SystemError getErrorToShow()     { return mErrorToShow;      }

    /**
     * @brief   Why the last app launch failed.
     * @return  The reason, for the screen that reports it to the user.
     */
    App::LaunchFailure getAppLaunchFailure() const { return mAppLaunchFailure; }

    /**
     * @brief Record the reason and show the message screen.
     *
     *        One entry point for both the backend telling us a launch failed
     *        and the pending-launch timer giving up on one, so the two cannot
     *        drift apart.
     */
    void showAppLaunchFailure(App::LaunchFailure reason,
                              Gui::Config::AppLaunchOrigin origin);

    /**
     * @brief Leave the launch-failure screen for wherever the launch started.
     *
     *        Mirrors the app-exit path in switchToNextPriorityScreen: a user
     *        who pressed an activity in the tracking menu should be back in
     *        the tracking menu, not deposited in the top menu.
     */
    void returnFromAppLaunchFailure();
    void systemErrorPlay(bool on);
    void acknowledgeError();

protected:
    ModelListener *modelListener;

    IGuiBackend &mBackend;

    uint32_t mIdleTimer = 0;

    bool mIsBooted = false;

    // Where a running app's frame can be read from. Arrives once, before
    // Booted, and stays valid for the lifetime of the system.
    AppSurfaceInfo mAppSurface {};

    /// GUI process of the running watch face, 0 when there is none.
    uint32_t mWatchFacePid = 0;

    DeviceInfo mDeviceInfo {};

    std::tm mDateTime {};

    bool mGpsSignalAcquired = false;
    uint32_t mGpsEpoTimeLeft = 0;
    bool mGpsEpoSyncState = false;

    struct {
        uint8_t level = 0;
        bool low = false;
        bool muted = false;
        bool shutdown = false;  // set by the LowBatteryShutdown event from Power
    } mBattery {};

    struct {
        bool bonded = false;
        uint8_t bondedCount = 0;
        bool connected = false;
        bool pairingActive = false;
        bool manualStart = false;
        bool reconnected = false;
        uint32_t pinCode = 0;
    } mBle {};

    struct {
        bool active = false;
        uint8_t pct = 0;
    } mFwUpdate {};

    struct {
        bool connected = false;
        bool portOpened = false;
        bool charging = false;
        bool blocked = false;   ///< charging refused (over-temperature)
        bool dismissed = false; ///< charging screen left with Back; until the cable moves
        bool msdOpen = false;   ///< the host owns the volume: the charging screen can't be left
    } mUsb {};

    struct {
        WatchSettings common {};
        WatchSettingsLocal local {};
    } mSettings {};

    struct {
        uint16_t top = 0;
        uint16_t settings = 0;
        uint16_t tracking = 0;
        uint32_t notificationListUid = 0;  // Position determinates by UID
        uint16_t glances = 0;
        uint16_t bleFCC = 0;
        uint16_t gnss = 0;
        uint16_t clock = 0;
        uint16_t sensors = 0;
        uint16_t backlight = 0;
        uint16_t sleep = 0;
    } mMenuPosition {};

    /// Which value the shared clock "FORMAT" picker currently edits (see setClockPickerDate).
    bool mClockPickerDate = false;

    /// The day the sleep window editor edits (see setSleepEditDay).
    uint8_t mSleepEditDay = 0;

    /// Hard bound on the phone notification list. Each entry can hold ~2 KB of
    /// message text, so the list must stay capped regardless of how chatty the
    /// phone is; the oldest non-call entry is evicted when the cap is reached.
    static constexpr size_t skMaxPhoneNotifications = 20;

    /// Vector storing pointers to a phone notification with reserved capacity.
    std::vector<std::shared_ptr<PhoneNotification>> mPhoneNotifications;
    uint32_t mPhoneNotificationUidForDetails = 0;

    /// The screen up wants a held L2 for itself -- see claimL2Hold().
    bool mL2HoldClaimed = false;

    /// Held across screen changes -- see appCache(). Only the two types with a
    /// menu of their own are kept; nothing else asks twice.
    AppCache mUtilityCache {};
    AppCache mActivityCache {};

    AppCache* cacheFor(App::Type type);

    /// Per channel: whether this screen started a notification alert that has
    /// not been stopped yet. Deliberately "started", NOT "still playing" -- the
    /// patterns are one-shot (~1s) while the flag lives until the screen closes
    /// (up to 5s), so a matched stop can still land after the sound or buzz
    /// already ended.
    ///
    /// The narrow job is to suppress an *unmatched* stop, i.e. one for an alert
    /// that muting suppressed. The backend forwards stops unconditionally, so
    /// such a stop would cut whatever else holds the hardware -- typically an
    /// app's interval cue, which is exempt from muting by design. It does not
    /// close the wider race where a late matched stop cuts an app cue; that
    /// predates this and needs the backend to know which side last played.
    bool mNotificationVibroStarted = false;
    bool mNotificationBuzzerStarted = false;

    void evictOldestPhoneNotification();

    /// Latest Now Playing snapshot (null until the first media update / cleared
    /// on disconnect).
    std::shared_ptr<NowPlaying> mNowPlaying;

    /// Latest home-screen widget state (inactive until an app pushes one).
    HomeWidgetData mHomeWidget {};

    struct AppContext {
        uint32_t pid = 0;           ///< Process ID of the GUI
        App::Capabilities caps {};   ///< Capabilities of the application
        /// Screen this app was launched from; where exiting it returns to.
        Gui::Config::AppLaunchOrigin origin = Gui::Config::AppLaunchOrigin::HOME;
    };
    std::vector<AppContext> mActiveApps {};

    AppContext* getActiveApp();
    AppContext* findApp(uint32_t pid);
    AppContext& moveAppToFront(uint32_t pid);
    void removeApp(uint32_t pid);

    /// Launch origin recorded by appStart(), claimed by the matching start
    /// confirmation from the backend. Apps the backend starts on its own have
    /// no pending entry and so keep the default origin. The entry expires after
    /// kAppLaunchOriginTimeout so a launch that is never confirmed cannot leave
    /// an origin behind for an unrelated start to inherit.
    struct {
        uint32_t pid = 0;
        uint32_t attempt = 0;
        Gui::Config::AppLaunchOrigin origin = Gui::Config::AppLaunchOrigin::HOME;
        uint32_t ticksLeft = 0;
    } mPendingLaunch;

    /// Attempt counter, incremented for every launch the user asks for.
    /// Never zero for a real attempt, so zero means "not one of ours" in an
    /// event the backend raised by itself.
    uint32_t mLaunchAttempt = 0;

    /// The launch the timer gave up on, kept so a failure event still queued
    /// behind other events can be recognised as belonging to it.
    ///
    /// After boot Model::tick() pumps ONE backend event per tick, so a
    /// UserAppStartFailed sitting behind another event is not seen until the
    /// tick after the timer expires. Without this the late event matches no
    /// pending entry, falls back to HOME, and sends the user to the watch
    /// face instead of the menu they launched from.
    ///
    /// Bounded by the next launch rather than by a timer: it only has to
    /// survive from the expiry tick to the one that drains the event.
    struct {
        uint32_t pid = 0;
        uint32_t attempt = 0;
        Gui::Config::AppLaunchOrigin origin = Gui::Config::AppLaunchOrigin::HOME;
        bool valid = false;
    } mExpiredLaunch;

    /// Why the last launch failed, read by AppStartFailedPresenter when that
    /// screen activates. Not cleared on success: the screen is only reached
    /// by way of a failure, so a stale value can never be shown.
    App::LaunchFailure mAppLaunchFailure {App::LaunchFailure::NONE};

    /// Where the failed launch was started from, so the message screen hands
    /// the user back to the menu they were using.
    Gui::Config::AppLaunchOrigin mAppLaunchFailureOrigin
            {Gui::Config::AppLaunchOrigin::HOME};

    /// Origin of the app that just stopped. Only live for the duration of the
    /// onAppStop() callback, so a stale value can never redirect an unrelated
    /// screen switch later on.
    Gui::Config::AppLaunchOrigin mAppExitOrigin = Gui::Config::AppLaunchOrigin::HOME;

    struct {
        uint32_t steps;
        uint32_t floors;
        uint32_t activityMinutes;
        uint32_t liveHeartRate;
        uint32_t restingHeartRate;
        uint32_t averageHeartRate;
    } mDailyHealth {};

    // The arbitrated error to show, NONE when there is none. Single source
    // for both the Error priority slot and what the screen renders.
    SystemError mErrorToShow { SystemError::NONE };

    void decIdleTimer();
    void decPendingLaunchTimer();

    void onDataEvent(const B2GEvent::Booted &evt);
    void onDataEvent(const B2GEvent::AppSurfaceConfig &evt);
    void onDataEvent(const B2GEvent::WatchFaceRunning &evt);
    void onDataEvent(const B2GEvent::DevInfo &evt);
    void onDataEvent(const B2GEvent::BleBond &evt);
    void onDataEvent(const B2GEvent::BleConnect &evt);
    void onDataEvent(const B2GEvent::BlePinCode &evt);
    void onDataEvent(const B2GEvent::BlePair &evt);
    void onDataEvent(const B2GEvent::BleReconnect &evt);
    void onDataEvent(const B2GEvent::BatteryLevel &evt);
    void onDataEvent(const B2GEvent::BatteryCharge &evt);
    void onDataEvent(const B2GEvent::ChargingBlocked &evt);
    void onDataEvent(const B2GEvent::LowBatteryShutdown &evt);
    void onDataEvent(const B2GEvent::UsbConnect &evt);
    void onDataEvent(const B2GEvent::UsbPortOpen &evt);
    void onDataEvent(const B2GEvent::MsdOpeningRequest &evt);
    void onDataEvent(const B2GEvent::MsdState &evt);
    void onDataEvent(const B2GEvent::Time &evt);
    void onDataEvent(const B2GEvent::FirmwareUpdate &evt);
    void onDataEvent(const B2GEvent::PhoneNotificationAdd &evt);
    void onDataEvent(const B2GEvent::PhoneNotificationRemove &evt);
    void onDataEvent(const B2GEvent::MediaNowPlaying &evt);
    void onDataEvent(const B2GEvent::MediaControlRequest &evt);
    void onDataEvent(const B2GEvent::FindPhoneConfirmation &evt);
    void onDataEvent(const B2GEvent::Settings &evt);
    void onDataEvent(const B2GEvent::SettingsLocal &evt);

    void onDataEvent(const B2GEvent::UserAppList &evt);
    void onDataEvent(const B2GEvent::UserAppIds &evt);
    void onDataEvent(const B2GEvent::UserAppIcons &evt);
    void onDataEvent(const B2GEvent::UserAppStartReq &evt);
    void onDataEvent(const B2GEvent::UserAppStartFailed &evt);
    void onDataEvent(const B2GEvent::UserAppStopped &evt);
    void onDataEvent(const B2GEvent::UserAppCapabilities &evt);
    void onDataEvent(const B2GEvent::UserAppGlanceList &evt);

    void onDataEvent(const B2GEvent::GpsFix &evt);
    void onDataEvent(const B2GEvent::GpsEpoTime& evt);
    void onDataEvent(const B2GEvent::GpsEpoSync& evt);
    void onDataEvent(const B2GEvent::DailyHealth &evt);
    void onDataEvent(const B2GEvent::HeartRate &evt);

    void onDataEvent(const B2GEvent::Error &evt);

    void onDataEvent(const B2GEvent::AccessoryScanResult &evt);
    void onDataEvent(const B2GEvent::AccessoryStatus &evt);
    void onDataEvent(const B2GEvent::AccessoryConfig &evt);

    void onDataEvent(const B2GEvent::MagAzimuth &evt);
    void onDataEvent(const B2GEvent::MagCalProgress &evt);
    void onDataEvent(const B2GEvent::MagCalDone &evt);

    void onDataEvent(const B2GEvent::HomeWidget &evt);

public:
    // --- External accessories (Settings ▸ Sensors) -----------------------
    // State pushed from the backend.
    const B2GEvent::AccessoryConfig& accessoryConfig() const
    {
        return mAccessoryConfig;
    }
    const std::vector<B2GEvent::AccessoryScanResult>& accessoryScanResults() const
    {
        return mAccessoryScanResults;
    }
    void clearAccessoryScanResults()
    {
        mAccessoryScanResults.clear();
        HeapTelemetry::AccessoryScanResultCount().store(0, std::memory_order_relaxed);
    }
    const B2GEvent::AccessoryStatus& accessoryStatus() const
    {
        return mAccessoryStatus;
    }

    // Commands to the backend.
    void accessorySetMode(uint8_t mode);   // WatchSettingsAccessories::Mode
    void accessoryScanStart();
    void accessoryScanStop();
    void accessoryForget(uint8_t index);
    void accessoryRemember(const B2GEvent::AccessoryScanResult &dev);

    // --- Compass (Settings > Sensors > Compass) --------------------------
    void magFeedStart();
    void magFeedStop();
    void magCalStart(uint32_t seconds);
    void magCalCancel();

private:
    B2GEvent::AccessoryConfig                  mAccessoryConfig {};
    std::vector<B2GEvent::AccessoryScanResult> mAccessoryScanResults;
    B2GEvent::AccessoryStatus                  mAccessoryStatus {};

};

#endif // MODEL_HPP
