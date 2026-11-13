#ifndef MODELLISTENER_HPP
#define MODELLISTENER_HPP

#include <gui/model/Model.hpp>
#include <gui/common/FrontendApplication.hpp>

class ModelListener
{
public:
    ModelListener() : model(0) {}
    
    virtual ~ModelListener() {}

    void bind(Model* m)
    {
        model = m;
    }

    /// True only for the boot splash, which leaves the bootloader's splash
    /// image on the panel. Model::bind() lets frames reach the panel as soon as
    /// any other screen comes up.
    virtual bool isBootSplash() const
    {
        return false;
    }

    // Default implementation for high-priority events.
    // In most cases, this is an unconditional transition to some screen.
    // Default: any screen jumps to the error screen. The error screen itself
    // (ErrorPresenter) owns leaving/updating; terminal power-off screens
    // override this to ignore errors.
    virtual void onError()
    {
        // A clear arrives as NONE and means "there is nothing to show"; only
        // the error screen acts on it, by leaving. Every other screen must
        // stay where it is rather than navigate to an empty error screen.
        if (model->getErrorToShow() == SystemError::NONE) {
            return;
        }
        model->application().gotoErrorScreenNoTransition();
    }

    virtual void onLowBattery()
    { 
        model->application().gotoLowBatteryScreenNoTransition();
    }

    virtual void onLowBatteryShutdown()
    {
        model->application().gotoLowBattShutDownScreenNoTransition();
    }

    virtual void onBlePinCode() 
    { 
        model->application().gotoPairCodeScreenNoTransition();
    }

    virtual void onUsbConnect()
    {
        // Charging refused (over-temperature): the charging screen would imply
        // a healthy charge. Stay put - the plug re-raises the error, which
        // takes the screen anyway.
        if (model->chargingIsBlocked()) {
            return;
        }
        model->application().gotoChargingScreenNoTransition();
    }

    // A file transfer no longer switches screens; the watch stays usable
    // while it downloads in the background.
    virtual void onFirmwareUpdStart() { }

    virtual void onPhoneNotificationNew(std::shared_ptr<PhoneNotification> notification)
    { 
        model->application().gotoPhoneNotificationScreenNoTransition();
    }

    virtual void onAppStartReq(uint32_t pid)
    {
        model->appResume();
        model->application().gotoUserAppScreenNoTransition();
    }

    /**
     * @brief   A launch the user asked for will not start.
     *
     *          Routed through the presenter rather than switched from the
     *          model, because the answer arrives asynchronously and may land
     *          on a screen that must not be replaced. Those screens override
     *          this to nothing, exactly as they do for onAppStartReq: the
     *          launch has failed either way, and what they are showing
     *          (shutting down, a system error, pairing) outranks saying so.
     */
    virtual void onAppLaunchFailed()
    {
        model->application().gotoAppStartFailedScreenNoTransition();
    }



    virtual void onIdleTimeout() { }

    virtual void onBooted() { }

    /**
     * @brief   Which app is drawing the watch face, 0 for the built-in one.
     * @note    Matched against the frame surface owner to tell the face's
     *          frames from whatever was on the surface before it.
     */
    virtual void onWatchFaceRunning(uint32_t guiPid) { }

    virtual void onGpsSignal(bool acquired) { }
    virtual void onGpsEpoTime(uint32_t h) {}
    virtual void onGpsEpoSyncState(bool state) {}

    virtual void onBatteryLevel(uint8_t lvl) { }

    
    virtual void onTime(uint8_t h, uint8_t m, uint8_t s) { }
    virtual void onDate(uint8_t m, uint8_t d, uint8_t wd) { }

    virtual void onBleBond(bool paired) { }
    virtual void onBleConnect(bool connected) { }
    virtual void onBlePair(bool paired) { }
    virtual void onBleReconnect() { }

    virtual void onCommonSettings(const WatchSettings &sett) { }
    virtual void onLocalSettings(const WatchSettingsLocal &sett) { }

    // External accessories (Settings ▸ Sensors).
    virtual void onAccessoryConfig() { }
    virtual void onAccessoryScanResult() { }
    virtual void onAccessoryStatus() { }

    // Compass (Settings > Sensors > Compass).
    virtual void onMagAzimuth(float degrees, bool valid, float faceTiltDeg)
    {
        (void) degrees;
        (void) valid;
        (void) faceTiltDeg;
    }
    virtual void onMagCalProgress(uint8_t percent) { (void) percent; }
    virtual void onMagCalDone(bool completed, bool stored) { (void) completed; (void) stored; }

    virtual void onDailyHealth(uint32_t steps, uint32_t floors, uint32_t activityMinutes) { }
    virtual void onHeartRate(uint32_t live, uint32_t avg, uint32_t rhr) { }

    virtual void onUsbState(bool portOpen, bool charging) { }
    virtual void onUsbDisconnect() { }

    
    virtual void onFirmwareUpdPct(uint8_t pct) { }
    virtual void onFirmwareUpdEnd() { }

    virtual void onAppList(App::Type type, const std::shared_ptr<std::vector<App::Item>> &list) { }
    virtual void onAppIds(App::Type type, const std::shared_ptr<std::vector<uint64_t>> &ids) { }
    /// The icons asked for by the last appRequestIconWindow(), in that order.
    virtual void onAppIcons(App::Type type, const App::IconRef *icons, uint8_t count) { }
    virtual void onAppStop() { }
    virtual void onAppGlanceList(const std::shared_ptr<std::vector<App::GlanceData>> &list) { }
   

    virtual void onPhoneNotificationListChanged(const std::vector<std::shared_ptr<PhoneNotification>> &list) { }
    virtual void onPhoneNotificationRemoved(uint32_t uid, bool all) { }

    virtual void onMediaNowPlaying(std::shared_ptr<NowPlaying> nowPlaying) { }

    // Home-screen widget pushed by a running app (active=false clears it).
    virtual void onHomeWidget(const HomeWidgetData &widget) { }

    virtual void onFindPhoneConfirmed() { }

protected:
    Model* model;

    
};

#endif // MODELLISTENER_HPP
