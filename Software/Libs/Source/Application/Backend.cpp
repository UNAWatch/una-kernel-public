/**
 ******************************************************************************
 * @file    Backend.cpp
 * @brief   Backend. Main watch logic.
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "Backend.hpp"

#define LOG_MODULE_PRX      "Backend"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include <algorithm>

#include "System.hpp"
#include "Rtc.hpp"
#include "Power.hpp"
#include "ComponentFactory.hpp"
#include "AppSurface.hpp"
#include "Buzzer.hpp"
#include "Vibro.hpp"
#include "Backlight.hpp"
#include "HWButtons.hpp"
#include "SettingsManager.hpp"
#include "SettingsLocalManager.hpp"
#include "SettingsDebugManager.hpp"
#include "SettingsAccessories.hpp"
#include "SDK/Messages/AccessoryMessages.hpp"
#include "App/Manager.hpp"
#include "App/AccessoryManager.hpp"
#include "Instances/InstanceActivityService.hpp"
#include "Instances/InstanceDailyHealthService.hpp"
#include "Debug/ActivityWatcher.hpp"
#include "Time/TimePorting.hpp"

#include "Instances/InstanceI2C.hpp"
#include "Instances/InstanceSensorLayer.hpp"
#include "Instances/InstanceMagCal.hpp"
#include "Instances/InstanceMagCalRunner.hpp"
#include "SensorLayer/Core/SensorManager.hpp"

#include "App/AppSorter.hpp"
#include "App/AppSorterFreqUse.hpp"

#include "flash_diskio.h"

#include "GpsAiroha.hpp"
#include "app_touchgfx.h"
#include "Crash/CrashDump.h"

#include "BleLogger.hpp"
#include "Loggers/SystemLogger.hpp"
#include "SleepMode/SleepSchedule.hpp"
#include "SDK/SensorLayer/SensorDataBatch.hpp"
#include "SDK/SensorLayer/DataParsers/SensorDataParserWristMotion.hpp"

#include <string>
#include <cstring>

namespace {

// Push the user-configured moderate-intensity HR cutoff into the
// activity-minute tracker's HR path. The zone-1 upper bound
// (heartRateZones.thresholds[0], ~50% HRmax) is the WHO lower bound of
// moderate activity. Reads the just-saved/loaded value from the settings
// singleton, so callers only need to ensure the manager is current first.
// Called at boot and on every path that can change heartRateZones (BLE
// settings.json write, USB MSD disconnect).
void applyActivityHrThreshold()
{
    Instance::ActivityService::getInstance().getFeatureCenter()
        .setActivityHrThresholdBpm(
            Settings::Manager::GetInstance()
                .getSettings().heartRateZones.thresholds[0]);
}

} // namespace

void Backend::run()
{
    mGuiRxQueue.init("GuiRx");
    mGuiTxQueue.init("GuiTx");

    mMinuteTimer.init("MinuteTim", this, &Backend::minuteTimCallback, true, nullptr);
    mBleMonitorTimer.init("BleMonitorTim", this, &Backend::bleMonitorTimCallback, true, nullptr);
    mFindPhoneTimer.init("FindPhoneTim", this, &Backend::findPhoneTimCallback, true, nullptr);
    mMusicHoldTimer.init("MusicHoldTim", this, &Backend::musicHoldTimCallback, false, nullptr);

    WatchSettingsDebug settingsDebug = Settings::DebugManager::GetInstance().getSettings();
    if (settingsDebug.battCfg.logEnable) {
        // battCfg.period == 0 means "use the production default"; nonzero
        // overrides (handy for shorter R&D periods via the debug JSON).
        constexpr uint32_t kBatteryLogDefaultPeriodSec = 60;
        const uint32_t period = settingsDebug.battCfg.period
                              ? settingsDebug.battCfg.period
                              : kBatteryLogDefaultPeriodSec;
        mBatteryLogger = std::make_unique<Component::BatteryLogger>();
        mBatteryLogger->init(period);
    }

    mThread.init(LOG_MODULE_PRX, this, &Backend::thread, osPriorityNormal);
}

Backend::Backend()
    : mBleFileSystem(FS::FileSystem::GetInstance(), BLE_ROOT_DIR)
    , mAltitudeStorage(Driver::Rtc::GetInstance(),
                       RTC_BRK_REG_WATCH_ALTITUDE,
                       RTC_BRK_REG_WATCH_ALTITUDE_CRC)
    , mSettings(Settings::AppSorting::GetInstance().getSettings())
    , mWristWake(SDK::Sensor::Type::WRIST_MOTION, this)
{
}

void Backend::thread()
{
    LOG_INFO("Device ID %s\n", Driver::System::GetDeviceIdStr());

    HWButtons::GetInstance().attachCallback(this);
    ErrorMonitor::GetInstance().attachStatusCallback(this);
    UsbDevice::GetInstance().attachCallback(this);

    ComponentFactory &factory = ComponentFactory::GetInstance();

    minuteTimCallback(nullptr);    // Start timer to update minutes

    DeviceInfo deviceInfo {};
    deviceInfo.name = Driver::System::GetModelNameStr();
    deviceInfo.manufacturer = Driver::System::GetManufacturerNameStr();
    deviceInfo.id = Driver::System::GetDeviceIdStr();
    deviceInfo.serialNumber = Driver::System::GetMcuUidStr();
    deviceInfo.swVersion = Driver::System::GetBuildVersionStr();
    sendEventToGui(B2GEvent::DevInfo{deviceInfo});

    //Settings
    WatchSettings settings = Settings::Manager::GetInstance().getSettings();
    sendEventToGui(B2GEvent::Settings{settings});

    WatchSettingsLocal settingsLocal = Settings::LocalManager::GetInstance().getSettings();
    sendEventToGui(B2GEvent::SettingsLocal{settingsLocal});

    WatchSettingsDebug settingsDebug = Settings::DebugManager::GetInstance().getSettings();

    // External-accessory config (feature flag + remembered straps) for the
    // Settings -> Sensors screen.
    Settings::Accessories::GetInstance().load();
    sendAccessoryConfig();
    // Receive live acquisition status from the central link.
    if (Ble::AccessoryCentral *c = Ble::AccessoryCentral::instance()) {
        c->attachStatusCallback(this);
    }

    // Power
    Power::GetInstance().attachStatusCallback(this);
    sendEventToGui(B2GEvent::BatteryLevel{ Power::GetInstance().getBatteryLevel()});
    sendEventToGui(B2GEvent::BatteryCharge{ Power::GetInstance().isCharging()});
    sendEventToGui(B2GEvent::ChargingBlocked{ Power::GetInstance().isChargingInhibited()});
    sendEventToGui(B2GEvent::UsbConnect{ Power::GetInstance().isUsbDetected()});



    // BLE
    Interface::IBle &ble = factory.getBle();

    if (factory.isBlePresent()) {
        ble.attachStatusCallback(this);

        char fullBleName[32] {};
        snprintf(fullBleName, sizeof(fullBleName), "%s %s",
                Driver::System::GetModelNameStr(),
                Driver::System::GetDeviceIdStr());
        ble.setName(fullBleName);

        loadCoreModuleInfo();

        pBleDeviceInformationService =
                new (std::nothrow) Ble::DeviceInformationService(ble,
                        Driver::System::GetManufacturerNameStr(),
                        Driver::System::GetModelNameStr(),
                        Driver::System::GetMcuUidStr(),
                        Driver::System::GetBuildVersionStr(),
                        mCoreModuleInfo.revision);

        pBleCurrentTimeService = new (std::nothrow) Ble::CurrentTimeService(ble, this, true);

        pBleBatteryService = new (std::nothrow) Ble::BatteryService(ble, this);

        pBleFileTransferService = new (std::nothrow) Ble::FileTransferService(ble, this, mBleFileSystem);

        pBleCustomCommandService = new (std::nothrow) Ble::CustomCommandService(ble, this);



        if (settingsDebug.bleCfg.useNUS) {
            pBleNordicUartService = new (std::nothrow) Ble::NordicUartService(ble, &BleLogger::GetInstance());
        } else {
            pBleNotificationService = new (std::nothrow) Ble::NotificationService(ble);
            pBleNotificationService->attachCallback(this);
            pBleNotificationService->enablePhoneNotification(settings.phone.notifications);
        }

        if (pBleCustomCommandService) {
            pBleMediaService = new (std::nothrow) Ble::MediaService(ble, *pBleCustomCommandService);
            pBleMediaService->attachCallback(this);
        }

        if (settingsDebug.bleCfg.useNUS && settingsDebug.logCfg.logToBLE &&
                pBleNordicUartService)
        {
            BleLogger::GetInstance().start(*pBleNordicUartService);
        }

        if (settingsDebug.bleCfg.useNUS && settingsDebug.fusionCfg.logToBLE &&
                pBleNordicUartService)
        {
            mAccelBleLogger.init(*pBleNordicUartService);
        }


        if (!ble.enable()) {
            ble.enable(); // Try again
        }

        ble.startAdvertising();

        // Start BLE monitor timer
        mBleMonitorTimer.start(skBleMonitorPeriodMs);

        sendEventToGui(B2GEvent::BleBond{ble.getBondedCount()});
    } else {
#if 1   // For debug only, just for skip startup screens
        sendEventToGui(B2GEvent::BleBond{1});
#endif
    }


    Instance::SensorLayer &sl = Instance::SensorLayer::getInstance();
    sl.init();

    // Activity service runs the FSM + sensor policy.
    Activity::Service& acs =
            Instance::ActivityService::getInstance().getService();
    acs.init();

    // Seed the activity-minute HR cutoff from the persisted HR zones;
    // kept in sync afterwards wherever heartRateZones can change.
    applyActivityHrThreshold();

    // A window may have started or ended while the watch was off. Then
    // lift-to-wake, which needs the sensor layer up.
    applySleepSchedule();
    updateLiftToWake();

    // DailyHealth pulls cumulative counters from Activity through
    // IActivityDataSource and computes per-day deltas. Sink sensors
    // (HRMetrics, DailyStepCounter, DailyFloorCounter,
    // DailyActivityTime) are already wired as DailyHealth::Service::Callback
    // subscribers in Instance::SensorLayer::init().
    DailyHealth::Service &dhs =
        Instance::DailyHealthService::getInstance().getService();
    dhs.init();
    dhs.attachCallback(this);

    // Update GUI with actual information
    onDailyValuesChange(dhs.getCurrentRecord());

#if 0
    // Debug helper that prints the IActivityDataSource snapshot every
    // ~10 s. Useful during bring-up.
    Debug::ActivityWatcher::getInstance().init();
#endif

    // Apps can only read the workouts folder, so it has to exist before any
    // runs; creating it also shows a USB user where workout files go.
    // mkdir() makes 2:/Workouts on the way and is a no-op once both exist.
    if (!FS::FileSystem::GetInstance().mkdir(WORKOUTS_SCHEDULE_DIR_PATH)) {
        LOG_ERROR("Failed to create %s\n", WORKOUTS_SCHEDULE_DIR_PATH);
    }

#if 1
    LOG_DEBUG("Scan apps\n");
    App::Manager &appmgr = App::Manager::GetInstance();
    appmgr.attachCallback(this);
    AppSurface::GetInstance().attachCallback(this);
    if (HWButtons::GetInstance().get(Interface::IButtons::Id::SW1) == false) {
        appmgr.scan();

        // Loaded before the autorun services, so it is ready to draw the
        // moment the home screen asks for it, rather than being read off the
        // file system while the user waits. Not put on screen here: no screen
        // has said it is up yet.
        prepareWatchFace(Settings::LocalManager::GetInstance()
                .getSettings().watchfaceAppId);

        appmgr.autoRun();
    }

    Interface::IGps &gps = factory.getGps();

    Interface::IGps::EpoPeriod period = gps.getEpoFilePeriod();
    LOG_INFO("EPO period: GPS: %u-%u, GLO: %u-%u, GAL: %u-%u, BD: %u-%u, QZZS: %u-%u\n",
                period.gps.start, period.gps.end,
                period.glonass.start, period.glonass.end,
                period.galileo.start, period.galileo.end,
                period.beidou.start, period.beidou.end,
                period.qzss.start, period.qzss.end);
    gps.attachCallback(this);
    sendEpoTimeToGui(period);
#endif

    float altitude;
    if (mAltitudeStorage.get(altitude)) {
        Instance::SensorLayer::getInstance().getSensorBarometer().computeP0(altitude);
    }

    // Ahead of Booted, so the first screen that wants it already has it
    sendEventToGui(B2GEvent::AppSurfaceConfig{ AppSurface::GetInstance().getInfo() });

    // Ready to handle event from GUI
    sendEventToGui(B2GEvent::Booted{});

    factory.getBacklight().on(backlightTimeoutMs());

    ErrorMonitor::GetInstance().run();

    for (;;) {

        G2BEvent::Data data;
        if (receiveBackendEvent(data)) {
            evtHandler(data);
        }

    }
}

void Backend::minuteTimCallback(void *arg)
{
    (void)arg;

    time_t now = time(nullptr);
    std::tm localTime;
    if (localtime_r(&now, &localTime) == nullptr) {
        LOG_ERROR("failed to get localtime\n");
        return;
    }

    // Get time to the end of the minute
    uint32_t nextWaitTimeMs = 1000 * (60 - localTime.tm_sec);
    // The next check will be when the number of minutes changes.
    mMinuteTimer.start(nextWaitTimeMs);

    sendEventToGui(B2GEvent::Time {localTime});

    // Timer task: the settings belong to the backend thread.
    sendEventToBackend(G2BEvent::SleepScheduleCheck {});
}

uint32_t Backend::backlightTimeoutMs()
{
    return 1000u * Settings::LocalManager::GetInstance().getSettings().backlight.timeoutS;
}

void Backend::applySleepSchedule()
{
    // Before the first sync the RTC holds no real time, and acting on it would
    // start or end sleep at some arbitrary hour.
    if (!Driver::Rtc::GetInstance().isTimeSet()) {
        return;
    }

    // While the volume is open to a PC the settings file is the PC's. The
    // schedule acts on its answer changing, so the first minute after the
    // volume closes catches up on anything missed.
    if (mMsdOpen) {
        return;
    }

    time_t now = time(nullptr);
    std::tm localTime;
    if (localtime_r(&now, &localTime) == nullptr) {
        LOG_ERROR("failed to get localtime\n");
        return;
    }

    Settings::LocalManager &slm = Settings::LocalManager::GetInstance();
    WatchSettingsLocal settings = slm.getSettings();

    const uint16_t minute = static_cast<uint16_t>(localTime.tm_hour * 60 + localTime.tm_min);
    if (!SleepMode::evaluate(settings, SleepMode::dayFromTmWday(localTime.tm_wday), minute)) {
        return;
    }

    LOG_INFO("Sleep schedule: %s\n", settings.sleep.active ? "asleep" : "awake");
    slm.saveSettings(settings);
    sendEventToGui(B2GEvent::SettingsLocal {settings});
    updateLiftToWake();
}

void Backend::updateLiftToWake()
{
    const bool want = SleepMode::liftToWake(Settings::LocalManager::GetInstance().getSettings());
    if (want == mWristWake.isConnected()) {
        return;
    }

    LOG_INFO("Lift to wake %s\n", want ? "on" : "off");
    if (want) {
        mWristWake.connect();
    } else {
        mWristWake.disconnect();
    }
}

void Backend::onSdlNewData(uint16_t                 handle,
                           const SDK::Sensor::Data* base,
                           uint16_t                 count,
                           uint16_t                 stride)
{
    if (!mWristWake.matchesDriver(handle) || count == 0) {
        return;
    }

    SDK::Sensor::DataBatch data(base, count, stride);
    if (SDK::SensorDataParser::WristMotion(data[0]).isDataValid()) {
        ComponentFactory::GetInstance().getBacklight().on(backlightTimeoutMs());
    }
}

void Backend::bleMonitorTimCallback(void *arg)
{
    Interface::IBle &ble = ComponentFactory::GetInstance().getBle();

    if (ble.isConnected()) {
        return;
    }

    if (ble.isAdvertise()) {
        return;
    }

    if (ble.isEnabled()) {
        if (!ble.isAlive()) {
            LOG_WARNING("BLE chip not responding, restarting\n");
            ble.disable();
            // Fall through to enable() on next timer tick
            return;
        }
        if (ble.isBonded()) {
            LOG_WARNING("Restart BLE advertising\n");
            ble.startAdvertising();
        }
        return;
    }

    LOG_WARNING("Restart BLE\n");
    if (ble.enable()) {
        bool isBonded = ble.isBonded();

        // Update bond state in GUI
        sendEventToGui(B2GEvent::BleBond {ble.getBondedCount()});

        if (isBonded) {
            ble.startAdvertising();
        }
    }

}

void Backend::findPhoneTimCallback(void *arg)
{
    if (pBleCustomCommandService) {
        pBleCustomCommandService->sendEventFindPhoneAlert(true);
    }
}

bool Backend::isBleFirmwareFile(std::string_view path)
{
    // "2:/Update/*.ota"
    auto isFwFile = [](std::string_view prefix,
            std::string_view suffix, std::string_view path) {

        return path.size() >= prefix.size() + suffix.size() &&
               path.substr(0, prefix.size()) == prefix &&
               path.substr(path.size() - suffix.size()) == suffix;
    };

    return isFwFile(UPDATE_DIR_PATH, OTA_FILE_EXTENSION, path);
}

bool Backend::isBleAppFile(std::string_view path)
{
    // "2:/Apps/*/*.uapp"
    auto isFwFile = [](std::string_view prefix,
            std::string_view suffix, std::string_view path) {

        return path.size() >= prefix.size() + suffix.size() &&
               path.substr(0, prefix.size()) == prefix &&
               path.substr(path.size() - suffix.size()) == suffix;
    };

    return isFwFile(APPS_DIR_PATH, APP_FILE_EXTENSION, path);
}

bool Backend::isBleSettingsFile(std::string_view path)
{
    // "2:/settings.json"
    return path == SETTINGS_FILE_FULL_PATH;
}

bool Backend::isBleEpoFile(std::string_view path)
{
    // "2:/GPS_EPO/*.DAT"
    auto isFwFile = [](std::string_view prefix,
            std::string_view suffix, std::string_view path) {

        return path.size() >= prefix.size() + suffix.size() &&
               path.substr(0, prefix.size()) == prefix &&
               path.substr(path.size() - suffix.size()) == suffix;
    };

    return isFwFile(GPS_EPO_FILE_DIR_PATH, PS_EPO_FILE_EXTENSION, path);
}

void Backend::sendEpoTimeToGui(Interface::IGps::EpoPeriod period)
{
    // find minimum end value
    uint32_t minH = UINT32_MAX;
    minH = period.gps.end < minH ? period.gps.end : minH;
    minH = period.glonass.end < minH ? period.glonass.end : minH;
    minH = period.galileo.end < minH ? period.galileo.end : minH;
    minH = period.beidou.end < minH ? period.beidou.end : minH;
    minH = period.qzss.end < minH ? period.qzss.end : minH;

    // convert to relative time
    std::time_t t = Interface::IGps::EpoPeriod::toTimeT(minH);
    time_t now = time(nullptr);

    uint32_t hoursLeft = 0;

    if (t > now) {
        hoursLeft = (t - now) / 3600;
    }
    sendEventToGui(B2GEvent::GpsEpoTime { hoursLeft });
}

bool Backend::receiveGuiEvent(B2GEvent::Data &data)
{
    return mGuiTxQueue.pop(data, 0);
}

bool Backend::sendEventToBackend(const G2BEvent::Data &data)
{
    return mGuiRxQueue.push(data, 0);
}

void Backend::onFrame()
{
    // One frame, and nothing follows from it: the pixels are already on the
    // surface, and carrying them to the panel is the whole of the work. This
    // is why an app drawing slowly leaves the kernel with no beat to keep.
    TouchGFX_RequestFrame();
}

bool Backend::sendEventToGui(const B2GEvent::Data &data)
{
    // Queued before the frame is asked for, so the frame that answers cannot
    // arrive ahead of what it is being asked to carry.
    const bool pushed = mGuiTxQueue.push(data, 0);

    // Frames are not asked for one at a time in general: this is a no-op while
    // the kernel draws at its steady rate, and only bites on a screen where
    // nothing of the kernel's own moves and the beat has been stopped.
    TouchGFX_ForceFrame();

    return pushed;
}

bool Backend::receiveBackendEvent(G2BEvent::Data &data)
{
    return mGuiRxQueue.pop(data);
}


// GUI event handlers
void Backend::evtHandler(const G2BEvent::Data &data)
{
    std::visit([](const auto &event) {
        GetInstance().guiHandler(event);
    }, data);
}

void Backend::guiHandler(const G2BEvent::Shutdown &evt)
{
    LOG_INFO("Shutdown\n");
    App::Manager::GetInstance().stopAll();

    // Persist the in-progress day before the watch loses power.
    Instance::DailyHealthService::getInstance().getService().flush();
    Instance::DailyHealthService::getInstance().getService().deinit();

    // Flush the final battery log record and close the file cleanly.
    // No-op if logging was disabled at boot (unique_ptr is null).
    mBatteryLogger.reset();

    Power::GetInstance().powerOff();
}

void Backend::guiHandler(const G2BEvent::BacklightCtrl &evt)
{
    if (evt.state) {
        ComponentFactory::GetInstance().getBacklight().on(evt.timeOn);
    } else {
        ComponentFactory::GetInstance().getBacklight().off();
    }
}

void Backend::guiHandler(const G2BEvent::BuzzerCtrl &evt)
{
    // A stop is not an alert, so it must pass even while muted: the flag can
    // already be set by the time a stop arrives -- the notification screen
    // emits one when it closes -- and dropping it would leave the hardware
    // running. Muting does not itself emit a stop.
    if (evt.num == 0) {
        Buzzer::GetInstance().stop();
        return;
    }

    WatchSettingsLocal settings = Settings::LocalManager::GetInstance().getSettings();

    if (!settings.alertMutedFlag) { // Alerts from GUI
        Buzzer::GetInstance().play(evt.notes, evt.num);
    }
}

void Backend::guiHandler(const G2BEvent::VibroCtrl &evt)
{
    if (evt.num == 0) {
        Vibro::GetInstance().stop();
        return;
    }

    WatchSettingsLocal settings = Settings::LocalManager::GetInstance().getSettings();

    // Muting covers haptics, not just sound -- matching the buzzer above and
    // the app-side REQUEST_VIBRO_PLAY handler in Application.cpp. Without this
    // the mute flag left phone-notification and low-battery alerts vibrating.
    if (!settings.alertMutedFlag) { // Alerts from GUI
        Vibro::GetInstance().play(evt.notes, evt.num);
    }
}

void Backend::guiHandler(const G2BEvent::BleStartPairing &evt)
{
    LOG_DEBUG("BleStartPairing\n");
    if (!ComponentFactory::GetInstance().getBle().pair()) {
        sendEventToGui(B2GEvent::BlePair { false });
    }
}

void Backend::guiHandler(const G2BEvent::BleAbortPairing &evt)
{
    LOG_DEBUG("BleAbortPairing\n");
    Interface::IBle& ble = ComponentFactory::GetInstance().getBle();

    if (ble.isConnected()) {
        ble.disconnect();
    } else {
        ble.stopAdvertising();  // Stop pairing advertising
    }
    if (ble.isBonded()) {
        ble.startAdvertising(); // Start normal advertising
    }
}

void Backend::guiHandler(const G2BEvent::BleUnpair &evt)
{
    LOG_DEBUG("BleUnpair\n");
    Interface::IBle& ble = ComponentFactory::GetInstance().getBle();
    ble.unpair();
    // Update bonding status
    sendEventToGui(B2GEvent::BleBond{ ble.getBondedCount() });
}

void Backend::guiHandler(const G2BEvent::BleStartFCC &evt)
{
    LOG_DEBUG("BleStartFCC. ch %u, frq %u MHz\n", evt.channel.number, evt.channel.frequency);

    mBleMonitorTimer.stop();

    Interface::IBle& ble = ComponentFactory::GetInstance().getBle();
    ble.startTxFCC(evt.channel.number);
}

void Backend::guiHandler(const G2BEvent::BleStopFCC &evt)
{
    LOG_DEBUG("BleStopFCC\n");
    Interface::IBle& ble = ComponentFactory::GetInstance().getBle();
    uint32_t packets = 0;
    ble.stopTxFCC(packets);

    // Restart BLE
    bleMonitorTimCallback(nullptr);
    // Restart BLE monitor timer
    mBleMonitorTimer.start(skBleMonitorPeriodMs);
}

void Backend::guiHandler(const G2BEvent::PhoneNotificationPositiveAction &evt)
{
    LOG_DEBUG("PhoneNotificationPositiveAction\n");
    if (pBleNotificationService) {
        pBleNotificationService->positiveAction(evt.notification);
    }
}

void Backend::guiHandler(const G2BEvent::PhoneNotificationNegativeAction &evt)
{
    LOG_DEBUG("PhoneNotificationNegativeAction\n");
    if (pBleNotificationService) {
        pBleNotificationService->negativeAction(evt.notification);
    }
}

void Backend::guiHandler(const G2BEvent::MediaRemoteCommand &evt)
{
    LOG_DEBUG("MediaRemoteCommand %u\n", evt.command);
    if (pBleMediaService) {
        pBleMediaService->sendRemoteCommand(evt.command);
    }
}

void Backend::guiHandler(const G2BEvent::FindPhone &evt)
{
    LOG_DEBUG("FindPhone %s\n", evt.enable ? "on" : "off");
    if (evt.enable) {
        if (!mFindPhoneTimer.start(1000)) {
            sendEventToGui(B2GEvent::FindPhoneConfirmation {});
        }
    } else {
        mFindPhoneTimer.stop();
        if (pBleCustomCommandService) {
            pBleCustomCommandService->sendEventFindPhoneAlert(false);
        }
    }
}

void Backend::guiHandler(const G2BEvent::RequestEpo &evt)
{
    LOG_DEBUG("RequestEpo\n");

    Interface::IGps::EpoPeriod period = ComponentFactory::GetInstance().getGps().getEpoFilePeriod();
    sendEpoTimeToGui(period);
}

bool Backend::clearAppData(const char* path)
{
    FILINFO fno{};
    DIR     dir{};
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        return false;
    }

    auto filter = [](const TCHAR* p, const FILINFO* fno) -> bool {
        (void) fno;

        const char* ext = strrchr(p, '.');

        if (ext && strcmp(ext, ".uapp") == 0) {
            return false;
        }

        return true;
    };

    std::string folderName;
    folderName.reserve(FF_MAX_LFN);

    bool status = true;
    for (;;) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK) {
            return false;
        }

        if (!fno.fname[0]) {
            break;
        }

//        if ((strcmp(fno.fname, ".") == 0) || (strcmp(fno.fname, "..") == 0)) {
//            continue;
//        }


        if (fno.fattrib & AM_DIR) {
            folderName  = path;
            folderName += fno.fname;

            status &= FS::Directory::RemoveRecursively(folderName.c_str(), filter, false);
        } else {
            // remove all files in Apps dir
            folderName  = path;
            folderName += fno.fname;
            FS::File::Remove(folderName.c_str());
        }
    }

    return status;
}

void Backend::guiHandler(const G2BEvent::ResetSettings &evt)
{
    LOG_DEBUG("ResetSettings\n");

    //// Disable USB access to the EMMC
    UsbDevice::GetInstance().deinit();

    //// Stop all applications
    App::Manager::GetInstance().stopAll();

    //// Reset DH and block until reboot
    Instance::DailyHealthService::getInstance().getService().resetAll();
    Instance::DailyHealthService::getInstance().getService().deinit();

    //// Stop battery logger so the current file is flushed and closed cleanly
    //// before the upcoming reboot. No-op if logging was disabled at boot.
    mBatteryLogger.reset();

    //// Clear GPS reference location
    ComponentFactory::GetInstance().getGps().setReferenceLocation(0, 0, 0);

    mAltitudeStorage.reset();

#if 0   // Not necessary
    //// Unpair BLE
    ComponentFactory::GetInstance().getBle().unpair();
#endif

    //// Remove all data in the apps folders first (system log stays open so any
    //// activity here is still captured).
    clearAppData(APPS_DIR_PATH);

    //// Flush and close the system log file before wiping the disk - otherwise
    //// FatFs cannot delete Logs/System/ while syslog_*.log is still open.
    //// No-op if system logging was disabled at boot.
    if (Component::SystemLogger::isStarted()) {
        Component::SystemLogger::GetInstance().stop();
    }

    //// Wipe the disk. All files and folders must be closed by this point -
    //// FatFs refuses to delete a directory while any file under it is open.
    //// Remove all except 'Apps' or 'apps' dir
    auto filter = [](const TCHAR *path, const FILINFO *fno) -> bool {
        const char* pFolder = APPS_DIR_PATH;
        size_t len = strlen(pFolder);
        if (len && pFolder[len - 1] == '/') --len;

        for (size_t i = 0; i < len; ++i) {
            unsigned char c1 = (unsigned char)tolower((unsigned char)path[i]);
            unsigned char c2 = (unsigned char)tolower((unsigned char)pFolder[i]);

            if (c1 != c2) return true;
            if (c1 == '\0') break;
        }
        return false;
    };
    FS::Directory::RemoveRecursively(USER_ROOT_DIR, filter, false);

    //// Create default settings
    Settings::Manager::GetInstance().setDefault();
    Settings::LocalManager::GetInstance().setDefault();

    //// Reboot
    OS::Delay(500);
    Power::GetInstance().reboot();
}

void Backend::guiHandler(const G2BEvent::Settings &evt)
{
    LOG_DEBUG("Settings\n");
    Settings::Manager &sm = Settings::Manager::GetInstance();
    sm.saveSettings(evt.settings);

    if (pBleNotificationService) {
        pBleNotificationService->enablePhoneNotification(sm.getSettings().phone.notifications);
    }

    applyActivityHrThreshold();   // HR zones may have changed
}

void Backend::guiHandler(const G2BEvent::SettingsLocal &evt)
{
    LOG_DEBUG("SettingsLocal\n");

    Settings::LocalManager &slm = Settings::LocalManager::GetInstance();

    const WatchSettingsLocal current = slm.getSettings();
    const bool faceChanged = current.watchfaceAppId != evt.settings.watchfaceAppId;

    // Sleep state is the backend's; the GUI's copy of it may be a step behind.
    slm.saveSettings(SleepMode::mergeFromGui(current, evt.settings));

    // An edited or newly enabled schedule may put now inside or outside a
    // window, and that counts as the schedule changing its mind.
    if (current.sleep.schedule != evt.settings.sleep.schedule
            || !std::equal(std::begin(current.sleep.days), std::end(current.sleep.days),
                           std::begin(evt.settings.sleep.days))) {
        applySleepSchedule();
    }

    if (current.backlight.gesture != evt.settings.backlight.gesture) {
        updateLiftToWake();
    }

    // Choosing a different face changes who should be drawing, which is the
    // other half of what the rule decides. Only on a change: this handler runs
    // for every local setting there is.
    if (faceChanged) {
        applyWatchFacePolicy();
    }
}

void Backend::guiHandler(const G2BEvent::SleepModeSet &evt)
{
    LOG_INFO("Sleep mode %s by hand\n", evt.on ? "on" : "off");

    Settings::LocalManager &slm = Settings::LocalManager::GetInstance();
    WatchSettingsLocal settings = slm.getSettings();

    // Only the state: what the schedule last asked for is left alone, so the
    // change holds until the schedule next changes its mind.
    if (SleepMode::set(settings, evt.on)) {
        slm.saveSettings(settings);
    }

    // Answered even when nothing changed, so the GUI's copy ends up matching.
    sendEventToGui(B2GEvent::SettingsLocal {settings});
    updateLiftToWake();
}

void Backend::guiHandler(const G2BEvent::SleepScheduleCheck &evt)
{
    (void)evt;
    applySleepSchedule();
}

void Backend::guiHandler(const G2BEvent::UserAppIdsReq &evt)
{
    B2GEvent::UserAppIds rsp {};
    rsp.type = evt.type;
    rsp.ids = std::make_shared<std::vector<uint64_t>>(
            App::Manager::GetInstance().getAppIds(evt.type));

    sendEventToGui(rsp);
}

void Backend::guiHandler(const G2BEvent::UserAppListReq &evt)
{
    LOG_DEBUG("UserAppListReq\n");
    App::Manager& appmgr = App::Manager::GetInstance();

    B2GEvent::UserAppList rsp {};
    rsp.type = evt.type;
    rsp.list = std::make_shared<std::vector<App::Item>>(appmgr.getAppList(evt.type));

    sortAppsByType(evt.type, *rsp.list);

    sendEventToGui(rsp);
}

void Backend::guiHandler(const G2BEvent::UserAppIconWindow &evt)
{
    App::Manager& appmgr = App::Manager::GetInstance();

    const uint8_t count = (evt.count < G2BEvent::UserAppIconWindow::kMax)
                        ? evt.count
                        : G2BEvent::UserAppIconWindow::kMax;

    B2GEvent::UserAppIcons rsp {};
    rsp.type  = evt.type;
    rsp.count = 0;

    // Built alongside the reply and swapped in at the end, so an id that is
    // still wanted keeps the set already in hand and everything else is
    // released -- the window is what is asked for, never a high-water mark.
    App::IconRef kept[App::kIconWindowMax];
    uint8_t keptCount = 0;

    for (uint8_t i = 0; i < count; ++i) {
        const uint64_t appId = evt.appIds[i];
        if (appId == 0) {
            continue;
        }

        // Already asked for earlier in this same window: one set, named twice.
        App::IconRef found;
        for (uint8_t k = 0; k < keptCount; ++k) {
            if (kept[k]->appId == appId) {
                found = kept[k];
                break;
            }
        }

        if (found) {
            if (rsp.count < B2GEvent::UserAppIcons::kMax) {
                rsp.icons[rsp.count++] = found;
            }
            continue;
        }

        // Already in the window this is replacing: reuse it rather than read
        // the same icons off the file system again.
        for (uint8_t h = 0; h < mIconWindowCount; ++h) {
            if (mIconWindow[h] && mIconWindow[h]->appId == appId) {
                found = mIconWindow[h];
                break;
            }
        }

        if (!found) {
            found = appmgr.getIconSet(appId);
        }

        if (!found) {
            continue;   // no such app, or no free slot; the menu draws the rest
        }

        if (keptCount < App::kIconWindowMax) {
            kept[keptCount++] = found;
        }

        if (rsp.count < B2GEvent::UserAppIcons::kMax) {
            rsp.icons[rsp.count++] = found;
        }
    }

    // Assigned rather than swapped: what leaves the window is released here,
    // and what stays is held by both for the moment in between.
    for (uint8_t i = 0; i < App::kIconWindowMax; ++i) {
        mIconWindow[i] = (i < keptCount) ? kept[i] : App::IconRef();
    }
    mIconWindowCount = keptCount;

    sendEventToGui(rsp);
}

/// Launch budget for a menu launch, inside the GUI's own 5 s launch timer.
static constexpr uint32_t skAppStartTimeoutMs = 3000;

/// How long a failed launch is given to unwind before the retry. Its service
/// exits as soon as it sees the stop, which is tens of milliseconds in the
/// field logs; this only matters for one that is slow to.
static constexpr uint32_t skFailedLaunchUnwindMs = 1000;

void Backend::guiHandler(const G2BEvent::UserAppStart &evt)
{
    LOG_DEBUG("AppStart [pid = 0x%08X]\n", evt.pid);
    App::Manager &mgr = App::Manager::GetInstance();
    App::Application *app = mgr.findByPID(evt.pid);
    if (!app) {
        // A stale icon for a .uapp that has since been deleted. Previously
        // this fell out of the bottom and the launcher sat there looking
        // like nothing had been pressed.
        LOG_WARNING("AppStart: no app for pid 0x%08X\n", evt.pid);
        sendEventToGui(B2GEvent::UserAppStartFailed {
                evt.pid, evt.attempt, App::LaunchFailure::NOT_FOUND });
        return;
    }

    bool started = app->run(skAppStartTimeoutMs);

    // Out of memory comes back within milliseconds - the image is refused as
    // it is placed - so a second attempt still fits inside the GUI's launch
    // timer. Any other failure is not one that unloading the face can fix.
    if (!started && app->getLaunchFailure() == App::LaunchFailure::OUT_OF_MEMORY
            && makeRoomForLaunch(*app)) {
        started = app->run(skAppStartTimeoutMs);
    }

    if (started) {
        // Confirm first. The GUI bounds a launch with a timer, and run()
        // has already spent most of that budget - a settings write before
        // the confirmation is charged to the same budget and can push the
        // launch past it, so the user is told it failed while it is starting.
        sendEventToGui(B2GEvent::UserAppStartReq {evt.pid, evt.attempt});

        // Both lists are offered the hit rather than one chosen by the
        // app's type: each tracks only the names its own menu listed, so
        // the list this app is not in ignores it -- and an app carrying
        // both type bits appears in both menus and belongs in both counts.
        App::SorterFreqUse activities(mSettings.activities);
        App::SorterFreqUse utilities(mSettings.utilities);

        // Sequenced deliberately: both must be offered the hit, so neither
        // call may be short-circuited away by the other's result.
        const bool activityBumped = activities.call(app->getName());
        const bool utilityBumped  = utilities.call(app->getName());

        if (activityBumped || utilityBumped) {
            Settings::AppSorting::GetInstance().saveSettings(mSettings);
        }
    } else {
        // Tell the GUI why, so it can say so. Without this the launcher
        // does nothing at all and the only record is a log line the user
        // never sees - which is the whole of the bug from their side.
        const App::LaunchFailure reason = app->getLaunchFailure();
        LOG_WARNING("AppStart failed [pid = 0x%08X], reason %u\n",
                evt.pid, static_cast<unsigned>(reason));
        sendEventToGui(B2GEvent::UserAppStartFailed {
                evt.pid, evt.attempt, reason});
    }
}

void Backend::guiHandler(const G2BEvent::UserAppGlancesStart &evt)
{
    LOG_DEBUG("UserAppGlancesStart\n");

    App::Manager& appmgr = App::Manager::GetInstance();
    appmgr.startGlances();
}

void Backend::guiHandler(const G2BEvent::UserAppGlancesStop &evt)
{
    LOG_DEBUG("UserAppGlancesStop\n");

    App::Manager& appmgr = App::Manager::GetInstance();
    appmgr.stopGlances();
}

void Backend::guiHandler(const G2BEvent::UserAppGlancesListReq &evt)
{
    LOG_DEBUG("UserAppListGlancesReq\n");
    App::Manager& appmgr = App::Manager::GetInstance();

    B2GEvent::UserAppGlanceList rsp {};
    rsp.list = std::make_shared<std::vector<App::GlanceData>>(appmgr.getGlancesData());

    sortGlances(*rsp.list);

    sendEventToGui(rsp);
}

void Backend::guiHandler(const G2BEvent::UserAppPause &evt)
{
    LOG_DEBUG("AppPause [pid = 0x%08X]\n", evt.pid);
    App::Manager &mgr = App::Manager::GetInstance();
    App::Application *app = mgr.findByPID(evt.pid);
    if (app) {
        app->suspend();
    } else {
        // Send stop event if manager didn't find the app
        // just clear GUI list
        sendEventToGui(B2GEvent::UserAppStopped {evt.pid});
    }
}

void Backend::guiHandler(const G2BEvent::UserAppResume &evt)
{
    LOG_DEBUG("AppResume [pid = 0x%08X]\n", evt.pid);
    App::Manager &mgr = App::Manager::GetInstance();
    App::Application *app = mgr.findByPID(evt.pid);
    if (app) {
        app->resume();
        sendEventToGui(B2GEvent::UserAppCapabilities{evt.pid, app->getCapabilities()});
    } else {
        // Send stop event if manager didn't find the app
        // just clear GUI list
        sendEventToGui(B2GEvent::UserAppStopped {evt.pid});
    }
}

void Backend::guiHandler(const G2BEvent::UserAppStop &evt)
{
    App::Manager &mgr = App::Manager::GetInstance();

    if (evt.pid == 0) {
        LOG_DEBUG("AppStop. All\n");
        App::Manager::GetInstance().stopAll();
        LOG_DEBUG("Done\n");
    } else {
        LOG_DEBUG("AppStop [pid = 0x%08X]\n", evt.pid);
        App::Application *app = mgr.findByPID(evt.pid);
        if (app) {
            app->stop();
        }
    }
}

/// Load budget for the watch face, matching the one a menu launch gives an app.
static constexpr uint32_t skWatchFaceStartTimeoutMs = 3000;

/// Teardown budget for a face being dropped. The wait is what frees the
/// memory: a stop without one returns before the app has torn down, so the next
/// load would run on top of it. Only a wedged face gets anywhere near this, and
/// App::Application caps its own wait past that.
///
/// NOTE this wait is now ahead of the user rather than behind them. It used to
/// run after the incoming face was already on screen, because the face being
/// dropped was the one falling out of a three-face window and was never the one
/// showing. With one face resident the face being stopped IS the one being
/// stepped away from, so the stop has to complete before the next face can be
/// placed -- that is the whole point, since it is what lets the reload take
/// back the hole. In the healthy case it is tens of milliseconds. A wedged face
/// makes it the full budget here plus App::Application's own force-terminate
/// grace, and all of that is now time the user spends waiting for the next
/// face.
static constexpr uint32_t skWatchFaceStopTimeoutMs = 3000;

App::Application* Backend::watchFace()
{
    const uint64_t appId =
            Settings::LocalManager::GetInstance().getSettings().watchfaceAppId;

    return App::Manager::GetInstance().findWatchFace(appId);
}

void Backend::announceWatchFace(uint32_t guiPid)
{
    if (guiPid == mAnnouncedFacePid) {
        return;
    }

    // Remembered only once it has gone out. Recording it first would make a
    // send that failed indistinguishable from one that landed, and the test
    // above -- which is there to stop repeats -- would then stop the correction
    // as well: every later attempt would match and say nothing. Left unchanged,
    // the next time the rule is applied says it again.
    if (!sendEventToGui(B2GEvent::WatchFaceRunning{guiPid})) {
        LOG_ERROR("Watch face name dropped (GUI queue full)\n");
        return;
    }

    mAnnouncedFacePid = guiPid;
}

void Backend::applyWatchFacePolicy()
{
    App::Application *face = watchFace();

    if (!mHomeShown) {
        // Something else is on screen. The face is left loaded but stood down,
        // so nothing of its is on the surface and it stops being paced.
        //
        // This is also what keeps a face off a volume being handed to the
        // host: mass storage is reached from a screen that is not the home
        // screen, so the face is already down by the time the apps are
        // stopped.
        if (face != nullptr && face->isGuiRun()) {
            face->suspend();
        }

        // Left named. A face that is merely standing down is still the face,
        // and nothing is showing it meanwhile; withdrawing the name would cost
        // the home screen a frame of the built-in face on every way back in.
        // A face that is really gone says so itself, as WatchFaceStopped.
        return;
    }

    // Naming nought is a real answer, and the one every way of failing below
    // comes to: it says there is no face app, which is what leaves the home
    // screen its own clock.
    if (face == nullptr) {
        announceWatchFace(0);
        return;
    }

    // Loading is the one step worth waiting on, and the wait is why this can
    // be trusted: run() answers whether the app is really there, so a face
    // that has been deleted or will not load is found out here instead of
    // being named to a screen that would then have nothing to show. Skipped in
    // the ordinary case -- a face is loaded once and stood down between
    // visits, not started afresh on every one.
    if (!face->isGuiRun() && !face->run(skWatchFaceStartTimeoutMs)) {
        LOG_ERROR("Watch face [%s] failed to start\n", face->getName());
        announceWatchFace(0);
        return;
    }

    // Asked for, not waited on. This runs on every return to the home screen,
    // the most frequent transition there is, and a confirmed message would put
    // the Backend thread behind an app's dispatch loop on that path. It would
    // also prove little: the wait ends when the app instance has posted to the
    // app, not when the app has come forward.
    //
    // Nothing watches for the frame that follows either. An app has to draw
    // once within its first three seconds to prove it is alive, and is left
    // alone after that -- so a face that has already drawn owes the kernel
    // nothing, and what it puts on screen from here is its own business. A
    // screen decides from the surface owner and the frame counter rather than
    // from having been told.
    face->resume();

    announceWatchFace(face->getGuiPID());
}

void Backend::unloadWatchFacesExcept(uint64_t keepA, uint64_t keepB, uint64_t keepC)
{
    for (App::Application *face :
            App::Manager::GetInstance().findAllByType(App::Type::kClockface)) {
        const uint64_t id = face->getAppID();
        if (id == keepA || id == keepB || id == keepC) {
            continue;
        }
        if (!face->isGuiRun()) {
            continue;
        }

        LOG_INFO("Watch face [%s] unloaded\n", face->getName());
        face->stop(skWatchFaceStopTimeoutMs);
    }
}

namespace {

/// The face that would give way to @p app, or nullptr if there is none to
/// unload - the built-in face, or one already unloaded for an earlier app.
App::Application* WatchFaceToUnloadFor(App::Application &app)
{
    // A face cannot make room for a face: the space being asked for is the
    // face's own.
    if (app.isType(App::Type::kClockface)) {
        return nullptr;
    }

    // Allocation-free: this runs because memory has run out, and a vector of
    // candidates is a throwing allocation that would reset the watch instead.
    return App::Manager::GetInstance().findLoadedWatchFace();
}

} // namespace

bool Backend::unloadWatchFaceForRoom(App::Application &app)
{
    App::Application *face = WatchFaceToUnloadFor(app);
    if (face == nullptr) {
        return false;
    }

    LOG_INFO("[%s] did not fit; unloading watch face [%s]\n",
             app.getName(), face->getName());

    // Stopped directly rather than through unloadWatchFacesExcept(), which
    // collects every face into a vector first. Every loaded face, not just the
    // first: choosing a face in settings loads it before the old one goes, so
    // two can be resident for a while. A face that will not stop is found
    // again, and ends the loop rather than spinning on it.
    for (App::Application *last = nullptr; face != nullptr && face != last;
            face = App::Manager::GetInstance().findLoadedWatchFace()) {
        face->stop(skWatchFaceStopTimeoutMs);
        last = face;
    }
    return true;
}

bool Backend::makeRoomForLaunch(App::Application &app)
{
    // Checked before waiting, so a launch with no face to give way fails at
    // once rather than after the wait.
    if (WatchFaceToUnloadFor(app) == nullptr) {
        return false;
    }

    if (!app.waitUntilIdle(skFailedLaunchUnwindMs)) {
        LOG_WARNING("Launch of [%s] did not fit, and its failed attempt has not "
                    "unwound; not retrying\n", app.getName());
        return false;
    }

    return unloadWatchFaceForRoom(app);
}

void Backend::prepareWatchFace(uint64_t appId)
{
    preloadWatchFace(appId);

    // Named before any screen asks, so the home screen already knows whose
    // frames to expect and never has to draw the built-in face while it finds
    // out. Safe before the face is resumed: a screen decides what to draw from
    // the surface owner and the frame count, not from having been told.
    App::Application *face = App::Manager::GetInstance().findWatchFace(appId);
    announceWatchFace((face != nullptr && face->isGuiRun())
            ? face->getGuiPID() : 0);
}

void Backend::preloadWatchFace(uint64_t appId)
{
    App::Application *face = App::Manager::GetInstance().findWatchFace(appId);
    if (face == nullptr || face->isGuiRun()) {
        return;
    }

    if (!face->run(skWatchFaceStartTimeoutMs)) {
        LOG_ERROR("Watch face [%s] failed to load\n", face->getName());
    }
}

void Backend::guiHandler(const G2BEvent::WatchFacePreview &evt)
{
    // One watch face resident at a time.
    //
    // This used to keep a three-face window -- the shown face and both its
    // neighbours -- and unload what fell out of it AFTER placing the new one.
    // That order is what shreds the arena: each step loads above the faces it
    // is about to free, so every free leaves a hole under live data. Measured:
    // five load/unload events in three seconds, and a 201,628-byte
    // face loaded, unloaded and reloaded inside the same window, the reload
    // then failing with 1.3 MB free.
    //
    // The window is given up entirely. Nothing is preloaded, and the outgoing
    // faces go BEFORE the incoming one is placed, so a reload can take back the
    // hole it just left instead of being pushed above it.
    //
    // The cost is that every step through the picker pays a full stop-and-start
    // rather than a resume. Measured over 189 steps on hardware: 31 ms mean to
    // load and show, 80 ms worst case. The picker is not an everyday path, and
    // that is a price worth paying to stop the arena being cut to pieces by it.
    //
    // Passing showAppId three times keeps exactly one face. When showAppId is 0
    // -- the kernel's built-in face -- this stands every app face down, which is
    // what leaves the surface unowned so the screen draws its own.
    unloadWatchFacesExcept(evt.showAppId, evt.showAppId, evt.showAppId);

    App::Application *show = App::Manager::GetInstance().findWatchFace(evt.showAppId);
    uint32_t showPid = 0;

    if (show != nullptr) {
        if (!show->isGuiRun() && !show->run(skWatchFaceStartTimeoutMs)) {
            LOG_ERROR("Watch face [%s] failed to start\n", show->getName());
        } else {
            show->resume();
            showPid = show->getGuiPID();
        }
    }

    // Exactly one face may be on screen, and the surface takes frames from
    // whichever app last claimed it. Done after the resume above, so the new
    // face already holds the surface.
    //
    // This no longer has the face being stepped away from to deal with: the
    // unload at the top of this handler has already stopped and destroyed it,
    // and Application::suspend() returns immediately on !isCreated(). What is
    // left for this loop is the one case that unload skips -- a face that is
    // created but whose GUI is not running -- which it stands down rather than
    // leaving half up.
    for (App::Application *face :
            App::Manager::GetInstance().findAllByType(App::Type::kClockface)) {
        if (face->getAppID() != evt.showAppId) {
            face->suspend();
        }
    }

    announceWatchFace(showPid);

    // No preloading of the neighbours, and nothing left to unload: the window
    // is gone, so a step to either side pays the same stop-and-start this one
    // just did.
    //
}

void Backend::guiHandler(const G2BEvent::WatchFacePreviewEnd &evt)
{
    // First, for the same reason a step unloads before it loads. On a cancel
    // the face being kept is the one the user came in with, which a step has
    // already stood down -- so the one still resident here is the face last
    // previewed. Loading the kept face before that one goes would put it above
    // a block about to be freed, and leave the hole underneath: precisely the
    // allocate-before-free this change exists to remove, on the one path that
    // would still have had it.
    //
    // When the kept face is the one already showing -- the ordinary confirm --
    // this stands nothing down and prepareWatchFace finds it running, so the
    // common path is unaffected.
    unloadWatchFacesExcept(evt.keepAppId, 0, 0);

    // Loaded, not put on screen: this runs while the picker is on its way out,
    // and it is the home screen arriving that decides a face should draw. Doing
    // it here means the screen finds it ready instead of waiting on the file
    // system with nothing to show. On a cancel this is the face the user came
    // in with rather than the one last previewed.
    prepareWatchFace(evt.keepAppId);

    // The picker leaves a face resumed -- it was showing one a moment ago --
    // and the rule is what stands it down again. Without this a face goes on
    // drawing behind every screen the user visits next, and worse: it is
    // resumed with no screen showing it, so the next resume finds it already
    // up and quietly does nothing, while the surface is still handed over and
    // cleared underneath it. The name survives this: standing a face down does
    // not take it back.
    applyWatchFacePolicy();
}

void Backend::guiHandler(const G2BEvent::MsdOpen &evt)
{
    if (ErrorMonitor::GetInstance().isSet(SystemError::CABLE_CONNECTION)) {
        return;
    }

    LOG_INFO("MSD Opening\n");

    if (!evt.enable) {
        return;
    }

    // A host that resets the bus and configures the device again before this
    // has run reports itself a second time; reopening would drop the volume
    // and re-enumerate it once more.
    if (mMsdOpen) {
        LOG_INFO("MSD already open\n");
        return;
    }
    mMsdOpen = true;

    // Before the delay and stopAll(), so the GUI has stopped offering a way
    // off the charging screen by the time the host can reach the volume.
    // Refused if the GUI cannot be told: it would go on offering that way off
    // while the host owns the volume. Replugging tries again.
    if (!sendEventToGui(B2GEvent::MsdState{true})) {
        mMsdOpen = false;
        LOG_ERROR("MSD open state dropped (GUI queue full); not opening MSD\n");
        return;
    }

    LOG_INFO("Reinit UsbDevice\n");

    OS::Delay(500);

    DailyHealth::Service& dhm = Instance::DailyHealthService::getInstance().getService();
    dhm.notifyMsdMode(true);

    App::Manager::GetInstance().stopAll();

    UsbDevice::GetInstance().reinit(true, FLASH_DISK_USR);

    OS::Delay(100); // TOTO: Fix App manager
}

void Backend::guiHandler(const G2BEvent::MsdClose &evt)
{
    DailyHealth::Service& dhm = Instance::DailyHealthService::getInstance().getService();
    dhm.notifyMsdMode(false);

    App::Manager &mgr = App::Manager::GetInstance();

    // Same as at boot, on settings that were reloaded before this event was
    // posted: the selection may have been edited over MSD, so the face that
    // comes back is not necessarily the one that was stopped. Loaded only --
    // the screen on the way back is what puts it on the surface.
    prepareWatchFace(Settings::LocalManager::GetInstance()
            .getSettings().watchfaceAppId);

    mgr.autoRun();

    OS::Delay(100); // TOTO: Fix App manager

    // At this point, USB is disabled, so reinit is a light
    UsbDevice::GetInstance().reinit(false, FLASH_DISK_USR);

    mMsdOpen = false;
    sendEventToGui(B2GEvent::MsdState{false});

    // The settings were reloaded before this was posted, and the file may have
    // been edited over USB, sleep state included. Here rather than where they
    // were reloaded: that is the USB task, and the connection is this thread's.
    updateLiftToWake();
}

void Backend::guiHandler(const G2BEvent::ScreenFrameRate &evt)
{
    // Nothing is weighed against this any more. What an app draws announces
    // itself frame by frame, so the screen is the only standing claim there is.
    TouchGFX_EnableLowFrameRate(evt.rate == 0);
    LOG_DEBUG("Screen frame rate %u\n", static_cast<unsigned>(evt.rate));
}

void Backend::guiHandler(const G2BEvent::HomeScreenShown &evt)
{
    mHomeShown = evt.shown;
    applyWatchFacePolicy();
}

void Backend::guiHandler(const G2BEvent::WatchFaceStopped &evt)
{
    if (evt.guiPid != mAnnouncedFacePid) {
        // Not the one being drawn. Faces are loaded and unloaded around the
        // one on screen as a matter of course, and none of that is news.
        return;
    }

    // Stopped and already started again before this was read - unloaded to
    // make room, then put back when the room was not needed after all - so
    // the PID is the running face's own. Acting on it would take the face
    // off a screen that is showing it.
    App::Application *face = App::Manager::GetInstance().findByPID(evt.guiPid);
    if (face != nullptr && face->isGuiRun()) {
        return;
    }

    if (!mHomeShown) {
        // Nobody is showing it, and the name says which face the watch wears
        // rather than whether it happens to be loaded this second. Taking it
        // back here would cost a frame of the built-in face on the way back in
        // -- which is exactly what mass storage does: every app is stopped
        // while another screen is up, and the face is loaded again before the
        // home screen returns. A face that is gone for good is found out by
        // the rule the next time it is asked for, which names nothing.
        return;
    }

    // The face the GUI is drawing has stopped -- of its own accord, or because
    // it stopped answering and was terminated -- and there is no other way for
    // a screen to learn that. Said without starting it again: a face that dies
    // on its own would otherwise be restarted for as long as it kept dying.
    announceWatchFace(0);
}

void Backend::guiHandler(const G2BEvent::AppGuiDidNotFit &evt)
{
    App::Application *app = App::Manager::GetInstance().findByPID(evt.srvPid);

    // Checked again here, on the thread that sends the app its requests: the
    // service may have exited, or its GUI come up, since it was refused.
    if (app == nullptr || !app->isSrvRun() || app->isGuiRun()) {
        return;
    }

    if (!unloadWatchFaceForRoom(*app)) {
        return;
    }

    // Brought on screen exactly as the service's own request would have been.
    // Not retried again if it fails: the kernel's request reports nothing.
    if (!app->runGuiForService(skAppStartTimeoutMs)) {
        LOG_WARNING("[%s] GUI still did not fit with the watch face unloaded\n",
                    app->getName());

        // Nothing is going to take the face's place, so put it back: if the
        // home screen is up it would otherwise stay on the built-in face until
        // the next time it is left and returned to. Not while the request is
        // still in the app's queue - a start slower than the timeout - or the
        // face would take the room it is about to need; nor once the GUI is up
        // after all. The home screen puts the face back in either case.
        if (!app->hasPendingRequests() && !app->isGuiRun()) {
            applyWatchFacePolicy();
        }
    }
}

void Backend::guiHandler(const G2BEvent::AppsIdle &evt)
{
    // Cleared before the sweep, not after: an app that goes idle while this
    // runs must be able to post again, or it would wait for the next report
    // from somebody else.
    mAppsIdlePending.store(false);
    App::Manager::GetInstance().releaseIdleApps();
}

void Backend::guiHandler(const G2BEvent::PlayError &evt)
{
    if (evt.enable) {
        ComponentFactory::GetInstance().getBacklight().on(750);

        Interface::IVibro::Note vnotes[1] {};
        vnotes[0].effect = Interface::IVibro::ALERT_750MS_100;   // loop/pause: none
        Vibro::GetInstance().play(vnotes, 1);

        Interface::IBuzzer::Note bnotes[7];

        bnotes[0] = { 100, 3 };
        bnotes[1] = { 100, 1 };
        bnotes[2] = { 100, 3 };
        bnotes[3] = { 100, 1 };
        bnotes[4] = { 100, 3 };
        bnotes[5] = { 100, 1 };
        bnotes[6] = { 100, 3 };
        Buzzer::GetInstance().play(bnotes, 7);
    } else {
        Vibro::GetInstance().stop();
        Buzzer::GetInstance().stop();
    }
}

void Backend::guiHandler(const G2BEvent::AcknowledgeError &evt)
{
    // User dismissed the error screen. Acknowledge (hide) - the condition
    // stays active until its producer clears it.
    ErrorMonitor::GetInstance().acknowledge();
}

void Backend::guiHandler(const G2BEvent::AccessorySetMode &evt)
{
    WatchSettingsAccessories cfg = Settings::Accessories::GetInstance().getSettings();
    uint8_t mode = evt.mode;
    if (mode > static_cast<uint8_t>(WatchSettingsAccessories::Mode::ExternalOnly)) {
        mode = 0;
    }
    cfg.mode = static_cast<WatchSettingsAccessories::Mode>(mode);
    Settings::Accessories::GetInstance().saveSettings(cfg);
    LOG_INFO("accessory mode -> %u\n", static_cast<unsigned>(mode));
    sendAccessoryConfig();
    // Apply to any in-progress acquisition: Off stops it now; a mid-session
    // External&Wrist<->ExternalOnly switch re-applies the fallback policy.
    App::AccessoryManager::getInstance().applyModeChange(mode);
}

void Backend::guiHandler(const G2BEvent::AccessoryScanStart &evt)
{
    if (Ble::AccessoryCentral *c = Ble::AccessoryCentral::instance()) {
        c->startDiscoveryScan(this);
    }
}

void Backend::guiHandler(const G2BEvent::MagFeedStart &evt)
{
    (void) evt;

    if (!mCompass.start()) {
        // Answered once with an invalid bearing, so the screen is not left
        // waiting.
        LOG_WARNING("No compass for the compass screen\n");
        sendEventToGui(B2GEvent::MagAzimuth{0.0f, false, 0.0f});
    }
}

void Backend::guiHandler(const G2BEvent::MagFeedStop &evt)
{
    (void) evt;

    // The screen is gone, so a run it started is asked to end; a run started
    // elsewhere is not.
    (void) Instance::MagCalRunner::cancel(*this);

    mCompass.stop();
}

void Backend::guiHandler(const G2BEvent::MagCalStart &evt)
{
    if (!Instance::MagCalRunner::start(*this, Instance::MagCalRunner::Mode::FIT,
                                       evt.seconds)) {
        // Refused: answered with the same event that ends a real run.
        sendEventToGui(B2GEvent::MagCalDone{false, false});
    }
}

void Backend::guiHandler(const G2BEvent::MagCalCancel &evt)
{
    (void) evt;

    if (!Instance::MagCalRunner::cancel(*this)) {
        // No run of this screen's to cancel: none, or one already ending on
        // its own, whose result follows this answer. Answered anyway, so a
        // screen whose answer was lost still gets one.
        sendEventToGui(B2GEvent::MagCalDone{false, false});
    }
}

void Backend::onCompassHeading(const Compass::Heading &heading)
{
    sendEventToGui(B2GEvent::MagAzimuth{heading.degrees, heading.valid,
                                        heading.faceTiltDeg});
}

void Backend::onMagCalProgress(const Instance::MagCalRunner::Progress &progress)
{
    sendEventToGui(B2GEvent::MagCalProgress{progress.percent});
}

void Backend::onMagCalDone(const Instance::MagCalRunner::Result &result)
{
    const bool stored =
            result.fitAccepted() && Instance::MagCal::store(result.record);

    sendEventToGui(B2GEvent::MagCalDone{result.completed, stored});
}

void Backend::guiHandler(const G2BEvent::AccessoryScanStop &evt)
{
    if (Ble::AccessoryCentral *c = Ble::AccessoryCentral::instance()) {
        c->stopDiscoveryScan();
    }
}

void Backend::guiHandler(const G2BEvent::AccessoryForget &evt)
{
    WatchSettingsAccessories cfg = Settings::Accessories::GetInstance().getSettings();
    if (evt.index < cfg.accessories.size()) {
        cfg.accessories.erase(cfg.accessories.begin() + evt.index);
        Settings::Accessories::GetInstance().saveSettings(cfg);
        LOG_INFO("forgot accessory %u\n", evt.index);
    }
    sendAccessoryConfig();
}

void Backend::guiHandler(const G2BEvent::AccessoryRemember &evt)
{
    WatchSettingsAccessories cfg = Settings::Accessories::GetInstance().getSettings();
    if (cfg.accessories.size() >= WatchSettingsAccessories::kMaxAccessories) {
        LOG_WARNING("remembered-accessory list full\n");
        return;
    }

    char addrStr[18];
    snprintf(addrStr, sizeof(addrStr), "%02X:%02X:%02X:%02X:%02X:%02X",
            evt.addr[5], evt.addr[4], evt.addr[3], evt.addr[2], evt.addr[1],
            evt.addr[0]);

    for (const RememberedAccessory &x : cfg.accessories) {
        if (x.addr == addrStr) {
            return;  // already remembered
        }
    }

    RememberedAccessory a {};
    a.kind = "hrm";  // v1 ships HRM only
    a.addrType = evt.addrType;
    a.addr = addrStr;
    a.name = evt.name;
    a.encrypted = false;
    // Log before the move: reading a.name afterwards yields a moved-from
    // string, which prints empty and makes a correctly-stored name look lost.
    LOG_INFO("remembered accessory %s (%s)\n", a.name.c_str(), addrStr);
    cfg.accessories.push_back(std::move(a));
    Settings::Accessories::GetInstance().saveSettings(cfg);
    sendAccessoryConfig();
}

void Backend::onScanResult(const Ble::AccessoryCentral::ScanResult &result)
{
    B2GEvent::AccessoryScanResult out {};
    // Copy one byte less than the buffer so the zero-initialised final byte
    // always remains as the NUL terminator, even if an advertised name fills
    // the source buffer without one (guards downstream string ops).
    memcpy(out.name, result.name, sizeof(out.name) - 1);
    memcpy(out.addr, result.addr, sizeof(out.addr));
    out.addrType = result.addrType;
    out.rssi = result.rssi;
    out.kind = SDK::Accessory::Kind::HRM;
    sendEventToGui(out);
}

void Backend::onAccessoryStatus(Ble::AccessoryCentral::LinkStatus status,
        const char *name)
{
    using LinkStatus = Ble::AccessoryCentral::LinkStatus;

    SDK::Accessory::State state;
    switch (status) {
        case LinkStatus::Searching:  state = SDK::Accessory::State::SEARCHING;   break;
        case LinkStatus::Connecting: state = SDK::Accessory::State::CONNECTING;  break;
        case LinkStatus::Connected:  state = SDK::Accessory::State::CONNECTED;   break;
        case LinkStatus::Lost:       state = SDK::Accessory::State::LOST;        break;
        case LinkStatus::Failed:     state = SDK::Accessory::State::UNAVAILABLE; break;
        case LinkStatus::Idle:
        default:                     state = SDK::Accessory::State::IDLE;        break;
    }

    B2GEvent::AccessoryStatus out {};
    out.kind = SDK::Accessory::Kind::HRM;
    out.state = static_cast<uint8_t>(state);
    if (name != nullptr) {
        strncpy(out.name, name, sizeof(out.name) - 1);
    }
    sendEventToGui(out);

    // Also forward to the owner app's service so an activity app can
    // show a live status indicator (pre-activity / in-workout). This runs on the
    // BLE event/timer thread, so defer the forward onto the Backend thread (via
    // the event queue): forwardStatusToOwner walks App::Manager's app list, which
    // is only safe to touch from the app-owning thread.
    G2BEvent::AccessoryStatusForward fwd {};
    fwd.state = out.state;
    strncpy(fwd.name, out.name, sizeof(fwd.name) - 1);
    if (!sendEventToBackend(fwd)) {
        // Queue full - the app misses this status edge; the FSM re-emits on the
        // next transition and the indicator self-corrects, but note the drop.
        LOG_WARNING("accessory status forward dropped (backend queue full)\n");
    }
}

void Backend::guiHandler(const G2BEvent::AccessoryStatusForward &evt)
{
    // Backend-thread context (see onAccessoryStatus): safe to walk the app list.
    App::AccessoryManager::getInstance().forwardStatusToOwner(evt.state, evt.name);
}

void Backend::sendAccessoryConfig()
{
    WatchSettingsAccessories cfg = Settings::Accessories::GetInstance().getSettings();

    B2GEvent::AccessoryConfig out {};
    out.mode = static_cast<uint8_t>(cfg.mode);
    out.count = 0;
    for (const RememberedAccessory &a : cfg.accessories) {
        if (out.count >= B2GEvent::AccessoryConfig::kMaxNames) {
            break;
        }
        strncpy(out.names[out.count], a.name.c_str(),
                sizeof(out.names[out.count]) - 1);
        ++out.count;
    }
    sendEventToGui(out);
}

void Backend::sortGlances(std::vector<App::GlanceData>& list) const
{
#if 0
    LOG_INFO("before\n");
    for (const auto& g : list) {
        LOG_INFO("glance name : %s\n", g.altname);
    }
#endif

    App::Sorter sorter(mSettings.glances);
    sorter.sort(list, [](const App::GlanceData& g) -> std::string_view {
        return std::string_view{g.altname};
    });

#if 0
    LOG_INFO("after\n");
    for (const auto& g : list) {
        LOG_INFO("glance name : %s\n", g.altname);
    }
#endif
}

void Backend::sortAppsByType(App::Type type, std::vector<App::Item>& apps)
{
#if 0
    LOG_INFO("before\n");
    for (const auto& app : apps) {
        LOG_INFO("app name : %s\n", app.name);
    }
#endif

    if (type == App::Type::kActivity) {
        #if 0
            LOG_INFO("ORDER\n");
            for (const auto& order : mSettings.activities) {
                LOG_INFO("%10s : %u\n", order.name.c_str(), order.count);
            }
        #endif
        App::SorterFreqUse sorter(mSettings.activities);
        sorter.sort(apps);
    } else if (type == App::Type::kUtility) {
        // Most-used first, as the activity list does. The top menu's three
        // fixed entries are not apps and are not in this list, so they keep
        // their places whatever this does to the apps that follow them.
        App::SorterFreqUse sorter(mSettings.utilities);
        sorter.sort(apps);
    }

#if 0
    LOG_INFO("after\n");
    for (const auto& app : apps) {
        LOG_INFO("app name : %s\n", app.name);
    }
#endif
}

// Power
void Backend::onCharging(bool state)
{
    sendEventToGui(B2GEvent::BatteryCharge{state});
}

void Backend::onChargingInhibited(bool state)
{
    sendEventToGui(B2GEvent::ChargingBlocked{state});
}

void Backend::onUsbDetected(bool state)
{
    if (state && ErrorMonitor::GetInstance().isSet(SystemError::CABLE_CONNECTION)) {
        return;
    }

    if (!ErrorMonitor::GetInstance().isSet(SystemError::CABLE_CONNECTION)) {
        ComponentFactory::GetInstance().getBacklight().on(backlightTimeoutMs());
    }

    sendEventToGui(B2GEvent::UsbConnect{state});
}

// UsbPortMonitor
void Backend::onHostConfigured()
{
    // Not on onConnectedPC(): that is the BCD result alone, and BCD reads some
    // wall chargers as a PC. Opening MSD stops every app, so it must wait for
    // a host that has actually enumerated the device.
    LOG_INFO("USB host configured the device\n");
    sendEventToBackend(G2BEvent::MsdOpen{true});
}

void Backend::onDisconnect()
{
    // Update settings
    Settings::Manager &sm = Settings::Manager::GetInstance();
    Settings::LocalManager &slm = Settings::LocalManager::GetInstance();
    Settings::DebugManager &sdm = Settings::DebugManager::GetInstance();
    Settings::AppSorting &sam = Settings::AppSorting::GetInstance();

    sm.load();
    slm.load();
    sdm.load();
    sam.load();

    // USB MSD writes bypass the managers' atomic write(), so refresh each
    // ".bak" from the just-reloaded primary to keep a crash-safe fallback.
    sm.refreshBackup();
    slm.refreshBackup();
    sdm.refreshBackup();
    sam.refreshBackup();

    applyActivityHrThreshold();   // HR zones may have changed via MSD

    WatchSettings settings = sm.getSettings();
    sendEventToGui(B2GEvent::Settings{settings});

    WatchSettingsLocal settingsLocal = slm.getSettings();
    sendEventToGui(B2GEvent::SettingsLocal{settingsLocal});

    // Last, because the handler brings the apps back and runs on another
    // thread: posting it before the reload above would race it, and the face
    // would be picked by whatever the settings held before the volume was
    // edited.
    sendEventToBackend(G2BEvent::MsdClose{});
}

void Backend::onSystemErrorDisplay(SystemError code)
{
    // code == UNKNOWN tells the GUI to leave the error screen.
    sendEventToGui(B2GEvent::Error{code});
}

void Backend::onSystemErrorCondition(SystemError code)
{
    // The phone is told about the condition, not about the screen: dismissing
    // a message on the watch hides it locally but fixes nothing, so it must
    // not read as a recovery. NONE here means everything really recovered.
    static_assert(static_cast<uint32_t>(SystemError::COUNT) <= 0xFFu,
            "SystemError no longer fits the single-byte BLE payload");

    if (pBleCustomCommandService) {
        pBleCustomCommandService->sendEventSystemError(static_cast<uint8_t>(code));
    }
}

void Backend::onBatteryLevelChanged(float level)
{
    sendEventToGui(B2GEvent::BatteryLevel{level});
    if (pBleBatteryService) {
        pBleBatteryService->onBatteryLevelChanged(level);
    }
}

void Backend::onBatteryLow()
{
    // Power decides the shutdown moment. The GUI only reacts to this event -
    // it must not derive shutdown from the displayed integer SOC, which rounds
    // any value below 1% to 0.
    sendEventToGui(B2GEvent::LowBatteryShutdown{});
}

// Buttons
void Backend::onPress(Interface::IButtons::Id id)
{
    ComponentFactory::GetInstance().getBacklight().on(backlightTimeoutMs());

    // Arm the media-control hold only on a clean L2 press (no other button already
    // held); any other button press cancels it, so it fires solely for L2 alone.
    if (id == Interface::IButtons::Id::SW3) {
        Interface::IButtons &buttons = HWButtons::GetInstance();
        bool otherHeld = buttons.get(Interface::IButtons::Id::SW1) ||
                buttons.get(Interface::IButtons::Id::SW2) ||
                buttons.get(Interface::IButtons::Id::SW4);
        if (!otherHeld) {
            mMusicHoldTimer.start(skMusicHoldMs);
        }
    } else {
        mMusicHoldTimer.stop();
    }

    WatchSettingsLocal settings = Settings::LocalManager::GetInstance().getSettings();

    if (!settings.alertMutedFlag) {
        if (settings.sound.buttons) {
            Buzzer::GetInstance().play();
        }
    }

    if (settings.vibro.buttons) {
        Vibro::GetInstance().play(Interface::IVibro::SHARP_CLICK_100);
    }
}

void Backend::onRelease(Interface::IButtons::Id id)
{
    // Cancel the media-control hold if L2 is released before it elapses.
    if (id == Interface::IButtons::Id::SW3) {
        mMusicHoldTimer.stop();
    }
}

void Backend::onCode(Interface::IButtons::Code code)
{
    // What a key sets off is not knowable from here -- it may change the
    // screen, which is carried out a tick later, or start something that
    // moves. A run of frames covers that, and covers the code reaching the
    // queue the GUI samples a moment after this: the two are told separately
    // and in no fixed order.
    TouchGFX_ForceFrame();
}

void Backend::musicHoldTimCallback(void *arg)
{
    (void) arg;

    // The hold is armed only for a clean L2-alone press and cancelled by any other
    // button, so reaching here already means L2 was held alone; just confirm L2 is
    // still down to cover a release-versus-expiry race. The GUI honors the active
    // app's enMusicControl capability and pauses the app while the screen is shown.
    if (HWButtons::GetInstance().get(Interface::IButtons::Id::SW3)) {
        sendEventToGui(B2GEvent::MediaControlRequest{});
    }
}


// BLE
void Backend::onPinRequest(uint32_t pin)
{
    mBlePinRequested = true;
    sendEventToGui(B2GEvent::BlePinCode{pin});
}

void Backend::onPair(bool state)
{
    sendEventToGui(B2GEvent::BlePair{state});
    sendEventToGui(B2GEvent::BleBond{ComponentFactory::GetInstance().getBle().getBondedCount()});
}

void Backend::onEncrypt(bool state)
{
    if (state && !mBlePinRequested) {
        sendEventToGui(B2GEvent::BleReconnect{});
    }
}

void Backend::onConnect(const Ble::Address &addr)
{
#if LOG_MODULE_LEVEL == LOG_LEVEL_DEBUG
    char tbuff[24];
    LOG_DEBUG("onConnect: Address %s\n", addr.toString(tbuff));
#endif

    mBlePinRequested = false;
    sendEventToGui(B2GEvent::BleConnect{true});
}

void Backend::onDisconnect(uint8_t reason)
{
    // Clear EPO synchronization state if it was active
    if (mEpoSyncState) {
        mEpoSyncState = false;
        sendEventToGui(B2GEvent::GpsEpoSync{mEpoSyncState});
    }

    LOG_DEBUG("onDisconnect: Reason 0x%0X\n", reason);
    sendEventToGui(B2GEvent::BleConnect{false});
}

// BLE CurrentTimeService
void Backend::onWriteTime(std::tm localTime)
{
    // 1) Let libc compute DST for this date/time
    localTime.tm_isdst = -1;

    // 2) localTime -> epoch according to the current timezone
    time_t epoch = mktime(&localTime);
    if (epoch == (time_t)-1) {
        LOG_ERROR("failed to convert [localTime -> epoch]\n");
        return;
    }

    // 3) epoch -> UTC without taking into account the current timezone
    std::tm utctime;
    if (gmtime_r(&epoch, &utctime) == nullptr) {
        LOG_ERROR("failed to convert [epoch -> UTC]\n");
        return;
    }

    // 4) Write UTC time to the RTC
    TimePorting_setTime(utctime);

    // 5) Speed up the time update on the screen, without
    // waiting for the end of the current minute
    sendEventToGui(B2GEvent::Time {localTime});
}

void Backend::onReadTime(std::tm &localTime)
{
    time_t now = time(nullptr);
    if (localtime_r(&now, &localTime) == nullptr) {
        LOG_ERROR("failed to get localtime\n");
        return;
    }
}

void Backend::onWriteTimeZoneDST(int16_t tz, uint16_t dstOffset)
{
    int16_t tzCurrent         = Driver::Rtc::GetInstance().getTimeZone();
    uint16_t dstOffsetCurrent = Driver::Rtc::GetInstance().getDstOffset();

    // BLE TimeZone already includes DST,
    // however in the system we use them separately
    tz -= dstOffset;

    // An unset zone reads as 0, which can equal the new one; it still
    // has to be written so it reads as set.
    if (Driver::Rtc::GetInstance().isTimeZoneSet() &&
        tzCurrent == tz && dstOffsetCurrent == dstOffset) {
        return;
    }

    // With the clock unset there is no local time worth keeping: only
    // apply the zone. Re-writing the time here would mark the RTC as set
    // at its 2000-01-01 power-on value until the phone's time write.
    if (!Driver::Rtc::GetInstance().isTimeSet()) {
        Driver::Rtc::GetInstance().setTimeZone(tz);
        Driver::Rtc::GetInstance().setDstOffset(dstOffset);
        TimePorting_updateTimeZone();
        return;
    }

    // 1) Remember current localtime
    time_t now = time(nullptr);
    std::tm localTime{};
    if (localtime_r(&now, &localTime) == nullptr) {
        LOG_ERROR("failed to get localtime\n");
        return;
    }

    // 2) Apply new timezone and DST to the RTC
    Driver::Rtc::GetInstance().setTimeZone(tz);
    Driver::Rtc::GetInstance().setDstOffset(dstOffset);

    // 3) Apply new timezone and DST to the libc (rely on the RTC values)
    TimePorting_updateTimeZone();

    // 4) Set localtime again
    onWriteTime(localTime);
}

void Backend::onReadTimeZoneDST(int16_t &tz, uint16_t &dstOffset)
{
    dstOffset = Driver::Rtc::GetInstance().getDstOffset();

    // BLE TimeZone already includes DST,
    // however in the system we use them separately
    tz = Driver::Rtc::GetInstance().getTimeZone() + dstOffset;
}

// BLE BatteryService
void Backend::onReadBatteryLevel(float &level)
{
    level = Power::GetInstance().getBatteryLevel();
}

// BLE File transfer
void Backend::onFileWrite(const char *path, uint8_t pct)
{
    // path - relative to BLE_ROOT_DIR
    //LOG_DEBUG("onFileWrite %s %d%%\n", path, pct);

    char realPath[Interface::IFileSystem::skMaxPathLen] {};
    if (!mBleFileSystem.getFullPath(realPath, sizeof(realPath), path)) {
        return;
    }

    if (pct == 100 && isBleSettingsFile(realPath)) {
        Settings::Manager &sm = Settings::Manager::GetInstance();
        sm.load();
        // The phone's FTS write bypasses the manager's atomic write(), so
        // refresh the ".bak" from the just-written primary - otherwise a
        // phone-only-written watch has no crash-safe fallback.
        sm.refreshBackup();

        if (pBleNotificationService) {
            pBleNotificationService->enablePhoneNotification(sm.getSettings().phone.notifications);
        }
        applyActivityHrThreshold();   // HR zones may have changed
        sendEventToGui(B2GEvent::Settings{sm.getSettings()});
        LOG_INFO("Settings updated\n");
    }

    if (isBleFirmwareFile(realPath) || isBleAppFile(realPath)) {
        sendEventToGui(B2GEvent::FirmwareUpdate{pct <= 100, pct});
        if (pct == 100) {
            sendEventToGui(B2GEvent::FirmwareUpdate{false, 100});
        }
    }

    if (isBleEpoFile(realPath)) {
        bool syncState = true;

        if (pct == 100) {
            LOG_INFO("EPO file updated [%s]\n", realPath);
            // We cannot read the updated information
            // directly here because it would lock the
            // file for writing from BLE.
            syncState = false;
        }

        if (mEpoSyncState != syncState) {
            mEpoSyncState = syncState;
            sendEventToGui(B2GEvent::GpsEpoSync{mEpoSyncState});
        }
    }

}

void Backend::onFileRead(const char *path, uint8_t pct)
{

}

// BLE Mobile app notifications
void Backend::onNotificationAdd(std::shared_ptr<PhoneNotification> &notification, bool isNew)
{
    LOG_DEBUG("Notification #%u (%s)\n", notification->getUid(), isNew ? "new" : "modified");
#if 1
    char buf[64] {};
    std::tm time = notification->getTimestamp();
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &time);
    LOG_DEBUG(" Date:     [%s]\n", buf);

    LOG_DEBUG(" App ID:   [%.*s]\n", notification->getAppId().size(), notification->getAppId().data());
    //LOG_DEBUG_DUMP(notification->getAppId().data(), notification->getAppId().size());
    LOG_DEBUG(" Title:    [%.*s]\n", notification->getTitle().size(), notification->getTitle().data());
    //LOG_DEBUG_DUMP(notification->getTitle().data(), notification->getTitle().size());
    LOG_DEBUG(" SubTitle: [%.*s]\n", notification->getSubTitle().size(), notification->getSubTitle().data());
    //LOG_DEBUG_DUMP(notification->getSubTitle().data(), notification->getSubTitle().size());
    LOG_DEBUG(" Message:  [%.*s]\n", notification->getMsg().size(), notification->getMsg().data());
    //LOG_DEBUG_DUMP(notification->getMsg().data(), notification->getMsg().size());
    LOG_DEBUG(" P. label: [%.*s]\n", notification->getPositiveAction().size(), notification->getPositiveAction().data());
    //LOG_DEBUG_DUMP(notification->getPositiveAction().data(), notification->getPositiveAction().size());
    LOG_DEBUG(" N. label: [%.*s]\n", notification->getNegativeAction().size(), notification->getNegativeAction().data());
    //LOG_DEBUG_DUMP(notification->getNegativeAction().data(), notification->getNegativeAction().size());
#endif

    WatchSettings settings = Settings::Manager::GetInstance().getSettings();

    if (settings.phone.notifications) {
        sendEventToGui(B2GEvent::PhoneNotificationAdd {notification, isNew});
    }
}

void Backend::onNotificationRemove(uint32_t uid)
{
    LOG_DEBUG("Notification #%u - Removed\n", uid);
    sendEventToGui(B2GEvent::PhoneNotificationRemove {uid, false});
}

void Backend::onNotificationRemoveAll()
{
    sendEventToGui(B2GEvent::PhoneNotificationRemove {0, true});
}

void Backend::onNowPlaying(const std::shared_ptr<NowPlaying> &nowPlaying)
{
    sendEventToGui(B2GEvent::MediaNowPlaying {nowPlaying});
}

void Backend::onRebootReq()
{
    Power::GetInstance().reboot();
}

bool Backend::onFwUpdateReq(const char *path)
{
    // path - relative to BLE_ROOT_DIR

    char realPath[Interface::IFileSystem::skMaxPathLen] {};
    if (!mBleFileSystem.getFullPath(realPath, sizeof(realPath), path)) {
        return false;
    }

    if (!isBleFirmwareFile(realPath)) {
        LOG_ERROR("Wrong file path [%s]\n", realPath);
        return false;
    }


    FS::FileSystem &fs = FS::FileSystem::GetInstance();

    if (!fs.exist(realPath)) {
        LOG_ERROR("No file [%s]\n", realPath);
        return false;
    }

    // Documented contract: the phone app requires battery > 10% to start a
    // firmware update, otherwise the command returns an error. This is the
    // user-visible SOC (getBatteryLevel reports raw SOC now), and stays
    // above the bootloader's own cutoff so the app fails first.
    if (Power::GetInstance().getBatteryLevel() < 10) {
        LOG_ERROR("Low battery\n");
        return false;
    }

    if (!fs.copy(realPath, BOOTLOADER_OTA_PATH)) {
        LOG_ERROR("Failed to copy file in the bootloader directory\n");
        return false;
    }

    fs.remove(realPath);

    Power::GetInstance().reboot();

    return true;
}

void Backend::onFindPhoneConfirmation()
{
    if (pBleCustomCommandService) {
        pBleCustomCommandService->sendEventFindPhoneAlert(false);
    }
    sendEventToGui(B2GEvent::FindPhoneConfirmation {});
    mFindPhoneTimer.stop();
    LOG_DEBUG("Phone found\n");
}

void Backend::onEventNotificationsChanged(bool enabled)
{
    if (!enabled) {
        return;
    }

    // The error event is only pushed when the condition changes, so a phone
    // that subscribes while a condition already stands would never hear about
    // it. Ask for the current state to be re-announced, including "no error",
    // so the phone has a defined starting point after every reconnection.
    //
    // Only a request is made here. This runs inside the BLE stack's own event
    // dispatch, under its core lock, so writing a characteristic from here
    // would re-enter the stack and stall the very thread that has to drain the
    // transmit pool. The answer comes back on the monitor's thread instead.
    ErrorMonitor::GetInstance().requestConditionResend();
}

void Backend::loadCoreModuleInfo()
{
    Instance::I2C& i2c = Instance::I2C::getInstance();
    Component::ModuleInfoStorage storage(i2c.getMainBus(),
                                         i2c.getSensor1Bus(),
                                         i2c.getSensor2Bus(),
                                         i2c.getBodyBus(),
                                         MODULE_INFO_CORE_FILE_FULL_PATH);

    // mCoreModuleInfo is zero-initialized, so all fields are "" if the record
    // has never been programmed; the BLE DIS then advertises empty strings.
    if (!storage.read(Component::ModuleInfoStorage::Module::CORE, mCoreModuleInfo)) {
        LOG_WARNING("Failed to read core module info\n");
        return;
    }

    LOG_INFO("Core module: name=%s revision=%s serial=%s\n",
            mCoreModuleInfo.name, mCoreModuleInfo.revision, mCoreModuleInfo.serial);
}

void Backend::onReadDailyHealth(uint16_t  year,
                                uint8_t   month,
                                uint8_t   day,
                                uint32_t& steps,
                                uint32_t& floors,
                                uint32_t& actMin,
                                uint32_t& RHR,
                                uint32_t& AHR)
{

    DailyHealth::Service &dhManager = Instance::DailyHealthService::getInstance().getService();
    if (dhManager.isInitialized()) {
        std::tm date{};
        date.tm_year = static_cast<int>(year) - 1900;
        date.tm_mon  = static_cast<int>(month) - 1;
        date.tm_mday = static_cast<int>(day);

        DailyHealth::DailyRecord r = dhManager.getStoredRecord(date);

        steps  = r.steps;
        floors = r.floorsUp + r.floorsDown;
        actMin = r.activityMinutes;
        RHR    = r.restingHeartRate;
        AHR    = r.averageHeartRate;
        LOG_DEBUG("Request data for %04u-%02u-%02u [steps %u, floors %u, act %u, RHR %u, AHR %u]\n",
                year, month, day,
                steps, floors, actMin, RHR, AHR);
    }
}

void Backend::onReadEpoPeriod(uint32_t &sgps, uint32_t &egps,
        uint32_t &sglo, uint32_t &eglo, uint32_t &sgal, uint32_t &egal,
        uint32_t &sbei, uint32_t &ebei, uint32_t &sqzs, uint32_t &eqzs)
{
    Interface::IGps &gps = ComponentFactory::GetInstance().getGps();

    Interface::IGps::EpoPeriod period = gps.getEpoFilePeriod();
    sgps = period.gps.start;
    egps = period.gps.end;
    sglo = period.glonass.start;
    eglo = period.glonass.end;
    sgal = period.galileo.start;
    egal = period.galileo.end;
    sbei = period.beidou.start;
    ebei = period.beidou.end;
    sqzs = period.qzss.start;
    eqzs = period.qzss.end;

    LOG_DEBUG("GPS: %u-%u, GLO: %u-%u, GAL: %u-%u, BD: %u-%u, QZZS: %u-%u\n",
            sgps, egps, sglo, eglo, sgal, egal, sbei, ebei, sqzs, eqzs);
}

void Backend::onWriteReferenceLocation(float lat, float lon, float alt)
{
    Interface::IGps &gps = ComponentFactory::GetInstance().getGps();

    // The position itself stays out of the log, which is written to storage
    LOG_INFO("Reference location received\n");

    gps.setReferenceLocation(lat, lon, alt);

    if (alt >= 0.000001f) {
        mAltitudeStorage.set(alt);
        Instance::SensorLayer::getInstance().getSensorBarometer().computeP0(alt);
    } else {
        LOG_INFO("Invalid altitude\n");
    }
}

void Backend::onReadDailyHealthHrMatrix(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                     uint8_t (&out)[60])
{
    DailyHealth::Service &dhManager = Instance::DailyHealthService::getInstance().getService();
    if (dhManager.isInitialized() && (hour < 24)) {
        std::tm date{};
        date.tm_year = static_cast<int>(year) - 1900;
        date.tm_mon  = static_cast<int>(month) - 1;
        date.tm_mday = static_cast<int>(day);
        date.tm_hour = static_cast<int>(hour);

        dhManager.getStoredHrSeries(date, out);

        LOG_DEBUG("Request data for %04u-%02u-%02u, hour %u\n",
                year, month, day, hour);
    }
}

// GPS
void Backend::onGpsFix(bool acquired)
{
    LOG_INFO("Gps Fix: %u\n", acquired);
}

void Backend::onGpsTime(std::time_t utc)
{
    std::time_t rtc = time(nullptr);
    uint32_t diff = std::abs(utc - rtc);

    //LOG_DEBUG("onGpsTime utc = %u, rtc = %u, diff = %d seconds\n", (uint32_t)utc, (uint32_t)rtc, diff);

    if (diff > 5) {
        std::tm gpstm {};
        gmtime_r(&utc, &gpstm);
        Driver::Rtc::GetInstance().set(gpstm);
    }
}

void Backend::onGpsLocation(Interface::IGps::LocationInfo location)
{
    //LOG_DEBUG("onGpsLocation lat = %f,  lon = %f\n", location.lat, location.lon);
}


// APP LifeCycleCallback
void Backend::onGuiStart(App::Application *app)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());

    // For debug only
    WatchSettingsDebug settingsDebug = Settings::DebugManager::GetInstance().getSettings();
    // Matches the launcher name from the app's manifest (APP_USER_NAME).
    if (strcmp("Run", app->getName()) == 0) {
        if (settingsDebug.fusionCfg.logToCSV) {
            mAccelLogger.start();
        }

        if (settingsDebug.fusionCfg.logToBLE) {
            mAccelBleLogger.start();
        }
    }

}

void Backend::onGuiStop(App::Application *app)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());

    // A user app stopping does not decide anything here: what a watch face
    // does depends on which screen is up, and the home screen says so itself
    // when it comes back. A face stopping is the one thing no screen can find
    // out on its own, so it is carried to the Backend thread and answered
    // there -- this runs on the thread of the app that just stopped.
    if (app->isType(App::Type::kClockface)
            && !sendEventToBackend(G2BEvent::WatchFaceStopped{app->getGuiPID()})) {
        // The only way a screen learns that the face it is drawing has gone.
        // Dropped, it leaves the home screen naming an app that will never draw
        // again, which no later event corrects.
        LOG_ERROR("Watch face stop dropped (backend queue full)\n");
    }

    sendEventToGui(B2GEvent::UserAppStopped{app->getGuiPID()});

    // For debug only
    WatchSettingsDebug settingsDebug = Settings::DebugManager::GetInstance().getSettings();
    // Matches the launcher name from the app's manifest (APP_USER_NAME).
    if (strcmp("Run", app->getName()) == 0) {
        if (settingsDebug.fusionCfg.logToCSV) {
            mAccelLogger.stop();
        }

        if (settingsDebug.fusionCfg.logToBLE) {
            mAccelBleLogger.stop();
        }
    }
}

void Backend::onIdle(App::Application *app)
{
    (void)app;

    // Runs on the idle app's own thread, which cannot free its own instance
    // (that joins it), so the work is carried to the Backend thread. One
    // event at a time: the queue is shared with the GUI and a dozen glances
    // exit together, so a report per app could crowd out the user's input.
    if (mAppsIdlePending.exchange(true)) {
        return;
    }

    if (!sendEventToBackend(G2BEvent::AppsIdle{})) {
        // Not lost for good: the next app to go idle posts it again, and an
        // instance that stays only costs memory until then.
        mAppsIdlePending.store(false);
        LOG_WARNING("Idle app release dropped (backend queue full)\n");
    }
}

void Backend::onGuiResume(App::Application *app)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());
}

void Backend::onGuiSuspend(App::Application *app)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());
    //sendEventToGui(B2GEvent::UserAppStopped{app->getGuiPID()});
}

void Backend::onGuiStartFailed(App::Application *app)
{
    // On the app's own thread, which cannot stop the face: that is done on the
    // Backend thread, where faces are loaded and unloaded.
    if (!sendEventToBackend(G2BEvent::AppGuiDidNotFit{app->getSrvPID()})) {
        LOG_ERROR("[%s] GUI did not fit, and the request for room was dropped "
                  "(backend queue full)\n", app->getName());
    }
}

void Backend::onGuiResumeRequest(App::Application *app)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());
    // Attempt 0: the app asked for this, not the user, so it must not claim
    // an origin recorded for a launch still in flight for the same pid.
    sendEventToGui(B2GEvent::UserAppStartReq{app->getGuiPID(), 0});
}

void Backend::onCapabilities(App::Application *app, const App::Capabilities &caps)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());
    sendEventToGui(B2GEvent::UserAppCapabilities{app->getGuiPID(), caps});
}

void Backend::onNewActivity(App::Application *app)
{
    LOG_DEBUG("App: %s, PID 0x%08X\n", app->getName(), app->getGuiPID());
    if (pBleCustomCommandService) {
        pBleCustomCommandService->sendEventActivityEnded(app->getAppID());
    }
}

// The widget label is sized by the SDK on the kernel side; HomeWidgetData
// hardcodes its own size to keep SDK headers out of the GUI-facing type. Assert
// they match so copying one into the other is a same-size move, never a
// byte-level truncation that could split a UTF-8 sequence.
static_assert(SDK::Message::WIDGET_TEXT_BYTES == HomeWidgetData::kTextBytes,
              "widget label buffer size drifted; the copy would split UTF-8");

void Backend::onWidgetStart(App::Application *app)
{
    LOG_DEBUG("Widget start from '%s'\n", app->getName());
    (void)app;   // otherwise unused when LOG_DEBUG compiles out
    // Transport only: forward the claim as-is. The GUI keeps the bottom bar on
    // daily health until an update carries content (see WatchFace1::setHomeWidget),
    // so a START on its own shows nothing.
    B2GEvent::HomeWidget w {};
    w.data.active = true;
    sendEventToGui(w);
}

void Backend::onWidgetUpdate(App::Application *app, const App::WidgetContent &content)
{
    LOG_DEBUG("Widget update from '%s'\n", app->getName());
    (void)app;   // otherwise unused when LOG_DEBUG compiles out
    // Decode the SDK bitmask here (the GUI stays SDK-free).
    B2GEvent::HomeWidget w {};
    w.data.active      = true;
    w.data.showText    = (content.shown & SDK::Message::WIDGET_SHOW_TEXT) != 0;
    w.data.showPercent = (content.shown & SDK::Message::WIDGET_SHOW_PERCENT) != 0;
    w.data.fromEnd     = (content.shown & SDK::Message::WIDGET_PROGRESS_FROM_END) != 0;
    w.data.percent     = content.percent;
    snprintf(w.data.text, sizeof(w.data.text), "%s", content.text);
    sendEventToGui(w);
}

void Backend::onWidgetStop(App::Application *app)
{
    LOG_DEBUG("Widget stop from '%s'\n", app->getName());
    (void)app;   // otherwise unused when LOG_DEBUG compiles out
    // Best-effort: sendEventToGui drops on a full queue, so a lost STOP would
    // leave a stale widget on the home screen (very unlikely; no periodic
    // reconcile in v1).
    B2GEvent::HomeWidget w {};
    w.data.active = false;
    sendEventToGui(w);
}

// Daily Health
void Backend::onDailyValuesChange(DailyHealth::DailyRecord record)
{
    B2GEvent::DailyHealth dh {};
    dh.steps            = record.steps;
    dh.floors           = record.floorsUp + record.floorsDown;
    dh.activityMinutes  = record.activityMinutes;
    dh.restingHeartRate = record.restingHeartRate;
    dh.averageHeartRate = record.averageHeartRate;

    sendEventToGui(dh);
}

