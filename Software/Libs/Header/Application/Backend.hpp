/**
 ******************************************************************************
 * @file    Backend.hpp
 * @brief   Backend. Main watch logic.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef APPLICATION_BACKEND_HPP
#define APPLICATION_BACKEND_HPP

#include <atomic>

#include "GuiBackendInterface.hpp"
#include "OS.hpp"

#include "main.h"
#include "Power.hpp"
#include "IButtons.hpp"

#include "IBle.hpp"
#include "BLE/Services/DeviceInformationService.hpp"
#include "BLE/Accessory/AccessoryCentral.hpp"
#include "BLE/Services/CurrentTimeService.hpp"
#include "BLE/Services/BatteryService.hpp"
#include "BLE/Services/FileTransferService.hpp"
#include "BLE/Services/NotificationService.hpp"
#include "BLE/Services/MediaService.hpp"
#include "BLE/Services/CustomCommandService.hpp"
#include "BLE/Services/NordicUartService.hpp"

#include "SettingsAppSorting.hpp"

#include "IGps.hpp"

#include "FileSystemGuard.hpp"

#include "Components/ModuleInfo/ModuleInfoStorage.hpp"

#include "AppTypes.hpp"
#include "AppSurface.hpp"
#include "App/Manager.hpp"

#include "DailyHealth/Service.hpp"

#include "Instances/InstanceMagCalRunner.hpp"
#include "UsbDevice.hpp"

#include "RtcAltitudeStorage/RtcAltitudeStorage.hpp"
#include "ErrorMonitor.hpp"
#include "Compass/Compass.hpp"

#include "Loggers/BatteryLogger.hpp"
#include "Loggers/AccelLogger.hpp"
#include "Loggers/AccelBleLogger.hpp"

#include "SDK/Interfaces/ISensorDataListener.hpp"
#include "SensorLayer/Core/SensorConnection.hpp"

#include <memory>

/**
 * @brief Backend. Main watch logic.
 */

struct Backend : public IGuiBackend,
                 public AppSurface::Callback,
                 public Power::StatusCallback,
                 public Interface::IButtons::Callback,
                 public Interface::IBle::StatusCallback,
                 public Ble::CurrentTimeService::Callback,
                 public Ble::BatteryService::Callback,
                 public Ble::FileTransferService::Callback,
                 public Ble::NotificationService::Callback,
                 public Ble::MediaService::Callback,
                 public Ble::CustomCommandService::Callback,
                 public Interface::IGps::Callback,
                 public App::LifeCycleCallback,
                 public DailyHealth::Service::Callback,
                 public UsbDevice::Callback,
                 public ErrorMonitor::Callback,
                 public Ble::AccessoryCentral::ScanCallback,
                 public Ble::AccessoryCentral::StatusCallback,
                 public Instance::MagCalRunner::Owner,
                 public Compass::Listener,
                 public SDK::Interface::ISensorDataListener
{
public:
    static Backend& GetInstance()
    {
        static Backend sInstance;
        return sInstance;
    }

    void run();

private:
    Backend();

    Ble::DeviceInformationService *pBleDeviceInformationService = nullptr;
    Ble::CurrentTimeService *pBleCurrentTimeService = nullptr;
    Ble::BatteryService *pBleBatteryService = nullptr;
    Ble::FileTransferService *pBleFileTransferService = nullptr;
    Ble::NotificationService *pBleNotificationService = nullptr;
    Ble::MediaService *pBleMediaService = nullptr;
    Ble::CustomCommandService *pBleCustomCommandService = nullptr;
    Ble::NordicUartService *pBleNordicUartService = nullptr;

    // Core (MCU) module identification. Loaded from ModuleInfoStorage and kept
    // alive for the BLE DIS, which stores a pointer into it rather than a copy.
    Component::ModuleInfoStorage::Info mCoreModuleInfo {};

    FileSystemGuard mBleFileSystem;
    OS::Timer<Backend> mFindPhoneTimer;

    /// The two directions between this thread and the GUI's.
    ///
    /// Pushed with no wait; a full queue drops the event, deliberately, since
    /// producers include the sensor sweep and the timer task. A caller may
    /// report a drop but must not retry, block, or carry out the work itself.
    OS::CircularBuffer<G2BEvent::Data, 20> mGuiRxQueue;
    OS::CircularBuffer<B2GEvent::Data, 20> mGuiTxQueue;

    OS::Timer<Backend> mMinuteTimer;

    OS::Timer<Backend> mBleMonitorTimer;
    static constexpr uint32_t skBleMonitorPeriodMs = 5000;

    // L2 held this long opens the media-control screen.
    OS::Timer<Backend> mMusicHoldTimer;
    static constexpr uint32_t skMusicHoldMs = 800;

    bool mEpoSyncState = false;

    bool mBlePinRequested = false;

    OS::Thread<Backend, 40*1024> mThread;

    Component::RtcAltitudeStorage mAltitudeStorage;

    // Held by pointer rather than value so the ~5-6 KB worth of thread stack,
    // queue and stream buffers only get allocated when battery logging is
    // actually enabled in settings. nullptr otherwise.
    std::unique_ptr<Component::BatteryLogger> mBatteryLogger;
    WatchSettingsAppSorting mSettings;
    Component::AccelLogger mAccelLogger;
    Component::AccelBleLogger mAccelBleLogger;

    /// Lift-to-wake: the IMU's wrist-wakeup gesture, connected while the
    /// Gesture setting is on and sleep mode is off. The gesture runs in the
    /// IMU, so holding it costs next to nothing.
    Sensor::Connection mWristWake;

    /// How long the backlight stays on after a button, a tilt, USB or boot.
    uint32_t backlightTimeoutMs();

    /// Let the sleep schedule act at the current local time. Saves and tells
    /// the GUI if it changed anything. Does nothing until the clock is set.
    void applySleepSchedule();

    /// Connect lift-to-wake when the Gesture setting is on and sleep mode is
    /// off, disconnect it otherwise.
    void updateLiftToWake();

    // ISensorDataListener - the wrist-wakeup gesture
    void onSdlNewData(uint16_t                 handle,
                      const SDK::Sensor::Data* base,
                      uint16_t                 count,
                      uint16_t                 stride) override;


    void thread();

    void minuteTimCallback(void *arg);
    void bleMonitorTimCallback(void *arg);
    void findPhoneTimCallback(void *arg);
    void musicHoldTimCallback(void *arg);

    bool isBleFirmwareFile(std::string_view path);
    bool isBleAppFile(std::string_view path);
    bool isBleSettingsFile(std::string_view path);
    bool isBleEpoFile(std::string_view path);
    void sendEpoTimeToGui(Interface::IGps::EpoPeriod period);

    // Load the core (MCU) module identification record into mCoreModuleInfo.
    void loadCoreModuleInfo();

    // GUI interface
    virtual bool receiveGuiEvent(B2GEvent::Data &data) override;

    // AppSurface::Callback
    virtual void onFrame() override;

    /**
     * @brief   Hand an event to the thread on the other side.
     * @note    Never waits: a queue with no room drops the event.
     * @retval  'true' if the event was queued.
     */
    virtual bool sendEventToBackend(const G2BEvent::Data &data) override;

    /**
     * @brief   Hand an event to the GUI. Same policy as above.
     */
    bool sendEventToGui(const B2GEvent::Data &data);

    bool receiveBackendEvent(G2BEvent::Data &data);

    // GUI event handlers
    void evtHandler(const G2BEvent::Data &data);
    void guiHandler(const G2BEvent::Shutdown &evt);

    void guiHandler(const G2BEvent::BacklightCtrl &evt);
    void guiHandler(const G2BEvent::BuzzerCtrl &evt);
    void guiHandler(const G2BEvent::VibroCtrl &evt);

    void guiHandler(const G2BEvent::BleStartPairing &evt);
    void guiHandler(const G2BEvent::BleAbortPairing &evt);
    void guiHandler(const G2BEvent::BleUnpair &evt);
    void guiHandler(const G2BEvent::BleStartFCC &evt);
    void guiHandler(const G2BEvent::BleStopFCC &evt);

    void guiHandler(const G2BEvent::PhoneNotificationPositiveAction &evt);
    void guiHandler(const G2BEvent::PhoneNotificationNegativeAction &evt);
    void guiHandler(const G2BEvent::MediaRemoteCommand &evt);

    void guiHandler(const G2BEvent::FindPhone &evt);
    void guiHandler(const G2BEvent::RequestEpo &evt);

    bool clearAppData(const char* path);

    void guiHandler(const G2BEvent::ResetSettings &evt);
    void guiHandler(const G2BEvent::Settings &evt);
    void guiHandler(const G2BEvent::SettingsLocal &evt);
    void guiHandler(const G2BEvent::SleepModeSet &evt);
    void guiHandler(const G2BEvent::SleepScheduleCheck &evt);

    void guiHandler(const G2BEvent::UserAppListReq &evt);
    void guiHandler(const G2BEvent::UserAppIdsReq &evt);
    void guiHandler(const G2BEvent::UserAppIconWindow &evt);
    void guiHandler(const G2BEvent::UserAppStart &evt);
    void guiHandler(const G2BEvent::UserAppGlancesStart &evt);
    void guiHandler(const G2BEvent::UserAppGlancesStop &evt);
    void guiHandler(const G2BEvent::UserAppGlancesListReq &evt);
    void guiHandler(const G2BEvent::UserAppPause &evt);
    void guiHandler(const G2BEvent::UserAppResume &evt);
    void guiHandler(const G2BEvent::UserAppStop &evt);
    void guiHandler(const G2BEvent::WatchFacePreview &evt);
    void guiHandler(const G2BEvent::WatchFacePreviewEnd &evt);

    /**
     * @brief   Unload every face except the ones given.
     * @note    Run before loading anything new, so the number of faces in
     *          memory never exceeds the size of the window being asked for.
     * @param   keepA, keepB, keepC: app IDs to leave alone; 0 matches nothing.
     */
    void unloadWatchFacesExcept(uint64_t keepA, uint64_t keepB, uint64_t keepC);

    /**
     * @brief   Unload the watch face to make room for @p app.
     * @details Stopping the face is what selecting another face does. The home
     *          screen loads it again when it comes back, or falls back to the
     *          built-in face if it no longer fits.
     * @param   app: The app that needs the room. A face is never unloaded for
     *          another face.
     * @retval  'true' if a face was unloaded, so a retry is worth making.
     */
    bool unloadWatchFaceForRoom(App::Application &app);

    /**
     * @brief   Make room for a launch from the menu that did not fit.
     * @details The face is not on screen while the menu is, so unloading it
     *          changes nothing the user can see.
     * @param   app: The app whose launch failed for want of memory. Waited on
     *          until its failed attempt has unloaded, so it can be launched
     *          again.
     * @retval  'true' if a face was unloaded and the launch is worth retrying.
     */
    bool makeRoomForLaunch(App::Application &app);

    /**
     * @brief   Load a face by app ID without putting it on screen.
     * @param   appId: the face to have ready; 0 does nothing.
     */
    void preloadWatchFace(uint64_t appId);

    /**
     * @brief   Have the selected face ready, and say which one it will be.
     * @note    Getting a face ready and putting it on screen are separate
     *          acts: this runs while some other screen is still up, so that
     *          the home screen finds a loaded, named face instead of waiting
     *          on the file system with the built-in face showing.
     * @param   appId: the selected face; 0 for the built-in one.
     */
    void prepareWatchFace(uint64_t appId);

    /**
     * @brief   The Clockface app the local settings select.
     * @retval  Pointer to the application, nullptr when the built-in face is
     *          selected or the selected one is not installed.
     */
    App::Application* watchFace();

    /**
     * @brief   Make the frame surface match who should be drawing on it.
     *
     *          One rule: the watch face draws while the home screen is up, and
     *          stands down otherwise. Everything that can change either side
     *          calls this, and it works out what to do from the state rather
     *          than from what the caller was doing.
     *
     *          It speaks for the selected face and no other. A face that has
     *          just stopped being the selection is not stood down here, so a
     *          caller that changes the selection has to account for the face it
     *          changed away from -- which is why the picker stops its own. That
     *          is the one thing a caller still has to know.
     *
     *          Only ever run on the Backend thread, which is what lets the
     *          state it reads and the pid it remembers go unguarded.
     */
    void applyWatchFacePolicy();

    /// Whether the home screen is the one up, as the GUI last reported it.
    bool mHomeShown = false;

    /**
     * @brief   Tell the GUI which app the watch wears as its face.
     * @note    Which app it is, not whether it happens to be drawing this
     *          second: a face keeps the name while it stands down between
     *          visits to the home screen, and across a teardown that stops
     *          every app while another screen is up. Taking the name back for
     *          either would cost a frame of the built-in face on the way back
     *          in. Nought is what says there is no face app, and the built-in
     *          one is what the home screen then shows.
     * @note    Sent only on a change: it is state, and the GUI keeps it for
     *          as long as it holds.
     * @param   guiPid: the face's GUI process, 0 when there is none.
     */
    void announceWatchFace(uint32_t guiPid);

    /// Last watch face named to the GUI, so repeats are not sent.
    uint32_t mAnnouncedFacePid = 0;

    /// An AppsIdle is in the queue and not yet handled. Set by whichever app
    /// thread posts it, cleared by the Backend thread as it starts the sweep.
    std::atomic<bool> mAppsIdlePending {false};

    /// Mass storage opened and not yet closed. Set and cleared on the Backend
    /// thread only, by the MsdOpen and MsdClose handlers.
    bool mMsdOpen = false;

    void guiHandler(const G2BEvent::MsdOpen &evt);
    void guiHandler(const G2BEvent::MsdClose &evt);
    void guiHandler(const G2BEvent::ScreenFrameRate &evt);
    void guiHandler(const G2BEvent::HomeScreenShown &evt);
    // Internal: carries a watch face stopping from the thread it was running
    // on (see WatchFaceStopped) onto the Backend thread.
    void guiHandler(const G2BEvent::WatchFaceStopped &evt);
    // Internal: frees the instances of apps that have gone idle (see AppsIdle).
    void guiHandler(const G2BEvent::AppsIdle &evt);
    // Internal: makes room for a GUI a service asked for (see AppGuiDidNotFit).
    void guiHandler(const G2BEvent::AppGuiDidNotFit &evt);
    void guiHandler(const G2BEvent::PlayError &evt);
    void guiHandler(const G2BEvent::AcknowledgeError &evt);

    void guiHandler(const G2BEvent::AccessorySetMode &evt);
    // Internal: runs the accessory-status forward on the Backend thread (posted
    // by onAccessoryStatus from the BLE thread) so App::Manager is touched only
    // from the app-owning thread.
    void guiHandler(const G2BEvent::AccessoryStatusForward &evt);
    void guiHandler(const G2BEvent::AccessoryScanStart &evt);
    void guiHandler(const G2BEvent::AccessoryScanStop &evt);
    void guiHandler(const G2BEvent::AccessoryForget &evt);
    void guiHandler(const G2BEvent::AccessoryRemember &evt);

    // Ble::AccessoryCentral::ScanCallback - forwards discovery results to GUI.
    void onScanResult(const Ble::AccessoryCentral::ScanResult &result) override;

    // Ble::AccessoryCentral::StatusCallback - maps the coarse link status onto
    // the SDK::Accessory::State contract and pushes it to the GUI.
    void onAccessoryStatus(Ble::AccessoryCentral::LinkStatus status,
            const char *name) override;

    // Push the accessory feature flag + remembered list to the GUI.
    void sendAccessoryConfig();

    // Compass (Settings > Sensors > Compass).
    void guiHandler(const G2BEvent::MagFeedStart &evt);
    void guiHandler(const G2BEvent::MagFeedStop &evt);
    void guiHandler(const G2BEvent::MagCalStart &evt);
    void guiHandler(const G2BEvent::MagCalCancel &evt);

    // Compass::Listener - a bearing for the compass screen, while it has the
    // feed on. Arrives on the sensor layer's thread.
    void onCompassHeading(const Compass::Heading &heading) override;

    // Instance::MagCalRunner::Owner. Both arrive on the run's thread, and are
    // forwarded and a good fit stored from there.
    void onMagCalProgress(const Instance::MagCalRunner::Progress &progress) override;
    void onMagCalDone(const Instance::MagCalRunner::Result &result) override;

    /// Running only while the compass screen is up. A calibration run asks
    /// for its own faster field rate itself.
    Compass mCompass { *this };

    void sortGlances(std::vector<App::GlanceData>& list) const;
    void sortAppsByType(App::Type type, std::vector<App::Item>& apps);

    /**
     * @brief The icons the screen now up asked for, and nothing else.
     *
     * Holds exactly the last window the GUI stated: what it already had is
     * reused, what it did not is read from the file system, and what has left
     * the window is let go of. The GUI is handed the same shared sets, so
     * dropping one here cannot pull pixels out from under a widget that is
     * still drawing them.
     *
     * Bounded by UserAppIconWindow::kMax, so this is a fixed cost -- ~4.5 KB an
     * entry -- however many apps are installed.
     */
    App::IconRef mIconWindow[App::kIconWindowMax];
    uint8_t      mIconWindowCount = 0;

     // Power
     virtual void onCharging(bool state) override;
     virtual void onChargingInhibited(bool state) override;
     virtual void onUsbDetected(bool state) override;
     virtual void onBatteryLevelChanged(float level) override;
     virtual void onBatteryLow() override;

     // Buttons
     virtual void onPress(Interface::IButtons::Id id) override;
     virtual void onRelease(Interface::IButtons::Id id) override;
     virtual void onCode(Interface::IButtons::Code code) override;

     // BLE
     virtual void onPinRequest(uint32_t pin) override;
     virtual void onPair(bool state) override;
     virtual void onEncrypt(bool state) override;
     virtual void onConnect(const Ble::Address &addr) override;
     virtual void onDisconnect(uint8_t reason) override;
     // BLE CurrentTimeService
     virtual void onWriteTime(std::tm localTime) override;
     virtual void onReadTime(std::tm &localTime) override;
     virtual void onWriteTimeZoneDST(int16_t tz, uint16_t dstOffset) override;
     virtual void onReadTimeZoneDST(int16_t &tz, uint16_t &dstOffset) override;
     // BLE BatteryService
     virtual void onReadBatteryLevel(float &level) override;
     // BLE File transfer
     virtual void onFileWrite(const char *path, uint8_t pct) override;
     virtual void onFileRead(const char *path, uint8_t pct) override;
     // BLE Mobile app notifications
     virtual void onNotificationAdd(std::shared_ptr<PhoneNotification> &notification, bool isNew) override;
     virtual void onNotificationRemove(uint32_t uid) override;
     virtual void onNotificationRemoveAll() override;
     // BLE Media (Now Playing)
     virtual void onNowPlaying(const std::shared_ptr<NowPlaying> &nowPlaying) override;
     // BLE CustomCommandService
     virtual void onRebootReq() override;
     virtual bool onFwUpdateReq(const char *path) override;
     virtual void onFindPhoneConfirmation() override;
     virtual void onReadDailyHealth(uint16_t year, uint8_t month, uint8_t day,
             uint32_t &steps, uint32_t &floors, uint32_t &actMin, uint32_t &RHR, uint32_t &AHR) override;
     virtual void onReadEpoPeriod(uint32_t &sgps, uint32_t &egps,
             uint32_t &sglo, uint32_t &eglo, uint32_t &sgal, uint32_t &egal,
             uint32_t &sbei, uint32_t &ebei, uint32_t &sqzs, uint32_t &eqzs) override;
     virtual void onWriteReferenceLocation(float lat, float lon, float alt) override;
     virtual void onReadDailyHealthHrMatrix(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                     uint8_t (&out)[60]) override;
     virtual void onEventNotificationsChanged(bool enabled) override;

     // GPS
     virtual void onGpsFix(bool acquired) override;
     virtual void onGpsTime(std::time_t utc) override;
     virtual void onGpsLocation(Interface::IGps::LocationInfo location) override;

     // App LifeCycleCallback
     virtual void onGuiStart(App::Application *app) override;
     virtual void onGuiStop(App::Application *app) override;
     virtual void onIdle(App::Application *app) override;
     virtual void onGuiResume(App::Application *app) override;
     virtual void onGuiSuspend(App::Application *app) override;
     virtual void onGuiResumeRequest(App::Application *app) override;
     virtual void onGuiStartFailed(App::Application *app) override;
     virtual void onCapabilities(App::Application *app, const App::Capabilities &caps) override;
     virtual void onNewActivity(App::Application *app) override;
     virtual void onWidgetStart(App::Application *app) override;
     virtual void onWidgetUpdate(App::Application *app, const App::WidgetContent &content) override;
     virtual void onWidgetStop(App::Application *app) override;

     // Daily Health
     virtual void onDailyValuesChange(DailyHealth::DailyRecord record) override;

     // USB
     virtual void onHostConfigured() override;
     virtual void onDisconnect() override;

     // Error
     virtual void onSystemErrorDisplay(SystemError code) override;
     virtual void onSystemErrorCondition(SystemError code) override;
};

#endif // APPLICATION_BACKEND_HPP
