#include <gui/model/Model.hpp>
#include <gui/model/ModelListener.hpp>
#include <gui/common/FrontendApplication.hpp>
#include <gui/topmenu_screen/TopMenuOrder.hpp>
#include <algorithm>

#if !defined(SIMULATOR)
#include <TouchGFXHAL.hpp>
#endif


#define LOG_MODULE_PRX      "GUI.Model"
#if defined(SIMULATOR)
#define LOG_MODULE_LEVEL    LOG_LEVEL_DEBUG
#else
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#endif
#include "gui/common/GuiLogger.hpp"

namespace {
/**
 * @brief Descriptor slots for dynamic bitmaps, shared by the whole GUI.
 *
 * Mostly app icons, and only for what is on screen: the top menu's strip needs
 * one per icon on the arc -- one more than there are resting positions, since
 * one is entering as another leaves -- plus the enlarged centre icon, and
 * the tracking menu's wheel two per row it keeps loaded. One more is reserved
 * for the running app's own frame. Exhausting the
 * table is silent -- dynamicBitmapCreateExternal just returns BITMAP_INVALID
 * and the icon does not draw -- so leave headroom above the worst case.
 */
constexpr uint32_t kDynamicBitmaps = 17;
} // namespace

#if defined(SIMULATOR)

#include "touchgfx/canvas_widget_renderer/CanvasWidgetRenderer.hpp"
#include "Windows.h"
#include <chrono>
#include <ctime>


#include "../../simulator/Backend.hpp"

Model::Model() : modelListener(0), mBackend(Backend::GetInstance())
{
    
    //touchgfx::CanvasWidgetRenderer::setWriteMemoryUsageReport(true);

    const uint32_t cacheSize = 1024 * 4;
    static uint16_t cach[cacheSize];
    Bitmap::setCache(cach, cacheSize, kDynamicBitmaps);

    mPhoneNotifications.reserve(20);
    mActiveApps.reserve(20);

    LOG_INFO("Application is running through simulator! \n");

    LOG_DEBUG_WP("\n"
        "       Keys:                       \n"
        "       ----------------------------\n"
        "       1   L1,         !   Long L1 \n"
        "       2   L2,         @   Long L2 \n"
        "       3   R1,         #   Long R1 \n"
        "       4   R2,         $   Long R2 \n"
        "       5-8 press,      %^&* release\n"
        "           (5/% = L1, 6/^ = L2, 7/& = R1, 8/* = R2;\n"
        "            hold = press, wait, release)\n"
        "       q   L1+L2                   \n"
        "       e   R1+R2                   \n"
        "       a   L1+R1                   \n"
        "       s   L2+R2                   \n"
        "       z   L1+R2                   \n"
        "       w   R1+L2                   \n"
    );



}

#else
Model::Model() :
        modelListener(0), mBackend(GuiBackendHolder::Get())
{
    // Set cache for display dynamic app icons
    const uint32_t cacheSize = 1024 * 4;
    static uint16_t cach[cacheSize];
    Bitmap::setCache(cach, cacheSize, kDynamicBitmaps);

    mPhoneNotifications.reserve(20);
}

#endif

void Model::bind(ModelListener *listener)
{
    modelListener = listener;

#if !defined(SIMULATOR)
    // The panel has shown the bootloader's splash image since start-up. Every
    // screen change comes through here, after the old screen is torn down and
    // before the new one draws, so the first screen other than the boot splash
    // is the first thing to reach the panel, whichever way it was reached.
    if (!listener->isBootSplash()) {
        TouchGFXHAL::releaseBootHold();
    }
#endif
}

void Model::tick()
{
#if defined(SIMULATOR)
    Backend::GetInstance().handleTickEvent();
#endif

    decIdleTimer();

    bool received = false;
    B2GEvent::Data data {};
    do {
        received = mBackend.receiveGuiEvent(data);
        if (received) {
            std::visit([this](const auto &event) {
                this->onDataEvent(event);
            }, data);
        }
    } while (received && !mIsBooted); // Receive all events until fully booted

    // After the pump, so a confirmation arriving on the expiry tick still counts
    decPendingLaunchTimer();
}

void Model::handleKeyEvent(uint8_t key)
{
#if defined(SIMULATOR)
    Backend::GetInstance().handleKeyEvent(key);
#endif

    // Hardwaare buttons
    if (Gui::Config::Button::L1 == key) {
        resetIdleTimer();
    }

    if (Gui::Config::Button::L2 == key) {
        resetIdleTimer();
    }

    if (Gui::Config::Button::R1 == key) {
        resetIdleTimer();
    }

    if (Gui::Config::Button::R2 == key) {
        resetIdleTimer();
    }

    // A button going down is the user doing something, and it is the only sign
    // of it there is while one is held: a press long enough to run a list on
    // never becomes a click, so without this a screen could time out from under
    // someone in the middle of scrolling it.
    if (Gui::Config::Button::L1_PRESS == key || Gui::Config::Button::L2_PRESS == key ||
        Gui::Config::Button::R1_PRESS == key || Gui::Config::Button::R2_PRESS == key) {
        resetIdleTimer();
    }
}


FrontendApplication& Model::application()
{
    return *static_cast<FrontendApplication*>(touchgfx::Application::getInstance());
}

void Model::switchToNextPriorityScreen()
{
    // Priority:
    // LowBatteryShutdown
    // Error (arbitrated over-temp / USB / ... warning)
    // Alarm
    // Phone notification
    // Pairing
    // LowBattery
    // Charging
    // UserAPP
    // Home screen

    if (mBattery.shutdown) {
        application().gotoLowBattShutDownScreenNoTransition();
        return;
    }

    if (mErrorToShow != SystemError::NONE) {
        application().gotoErrorScreenNoTransition();
        return;
    }

    if (phoneNotificationIsActive()) {
        auto app = getActiveApp();
        if (!app || app->caps.enPhoneNotification) {
            application().gotoPhoneNotificationScreenNoTransition();
        }
        return;
    }

    if (bleIsPairingActive()) {
        if (bleGetPinCode() != 0) {
            application().gotoPairCodeScreenNoTransition();
        } else {
            application().gotoPairingScreenNoTransition();
        }
        return;
    }

    if (batteryIsLow() && !batteryIsMuted() && !usbIsCharging()) {
        application().gotoLowBatteryScreenNoTransition();
        return;
    }

    // A firmware/app file transfer no longer takes over the UI - the watch
    // stays usable while it downloads in the background (see onDataEvent).

    if (usbIsConnected()) {
        auto app = getActiveApp();
        if (app && !app->caps.enUsbChargingScreen) {
            return;
        }

        if (usbIsPortOpened()) {
            application().gotoUSBScreenNoTransition();
            return;
        }

        // The charging screen reports a battery level and nothing else, so
        // while charging is refused it would read as a healthy charge. Skip it
        // and fall through to the normal screen - the reason was already shown
        // on the error screen, and re-plugging brings it back. Skip it too
        // once the user has backed out of it: otherwise it would come straight
        // back every time something else (a notification, say) gives way.
        if (!chargingIsBlocked() && !chargingIsDismissed()) {
            application().gotoChargingScreenNoTransition();
            return;
        }
    }

    auto pApp = getActiveApp();
    if (pApp) {
        appResume();
        application().gotoUserAppScreenNoTransition();
        return;
    }

    // An app that has just exited hands back the menu it was launched from, so
    // backing out of an app mirrors backing out of the settings menu. Every
    // other caller leaves the origin at HOME. Deliberately give way to anything
    // above: if a notification (say) takes the screen on the way out, the origin
    // is spent and dismissing that lands on the watch face, rather than have a
    // remembered menu resurface long after the app is gone.
    switch (mAppExitOrigin) {
        case Gui::Config::AppLaunchOrigin::TOP_MENU:
            application().gotoTopMenuScreenNoTransition();
            return;
        case Gui::Config::AppLaunchOrigin::TRACKING_MENU:
            application().gotoTrackingMenuScreenNoTransition();
            return;
        case Gui::Config::AppLaunchOrigin::HOME:
            break;
    }

    // Default
    application().gotoHomeScreenNoTransition();
}

void Model::resetIdleTimer()
{
    mIdleTimer = Gui::Config::kScreenTimeoutSteps;
}

uint32_t Model::getTimeMs()
{
#if defined(SIMULATOR)
    Backend::GetInstance().getTimeMs();
#endif
    return 0;
}

void Model::claimL2Hold(bool claimed)
{
    mL2HoldClaimed = claimed;
}

// Menu position
void Model::setPosMenuTop(uint16_t p)
{
    mMenuPosition.top = p;

    // This is the position the top menu will open at, and the earliest moment
    // it is known -- so the icons it will want are fetched now rather than
    // after its first frame. Without this the cache holds wherever the menu was
    // last looking: walk to the end of the list, leave, and the icons in hand
    // are the ones at the end while the menu reopens at the top.
    prefetchIcons(App::Type::kUtility);
}

Model::AppCache* Model::cacheFor(App::Type type)
{
    switch (type) {
        case App::Type::kUtility:  return &mUtilityCache;
        case App::Type::kActivity: return &mActivityCache;
        default:                   return nullptr;
    }
}

const Model::AppCache* Model::appCache(App::Type type) const
{
    return const_cast<Model*>(this)->cacheFor(type);
}

void Model::prefetchIcons(App::Type type)
{
    const AppCache* cache = appCache(type);
    if (cache == nullptr || cache->list == nullptr || cache->list->empty()) {
        return;   // nothing to name yet; the list's arrival asks again
    }

    const int32_t size = static_cast<int32_t>(cache->list->size());

    // Both menus show a window over a list that can outgrow it, so both follow
    // the position they will open at and ask for that neighbourhood -- never
    // for the whole list. Each menu asks for its exact window as it opens, so
    // this only has to be close enough to have the pixels in hand by then.
    if (type == App::Type::kUtility) {
        // Fixed entries and apps interleave, so a menu item id is not an app
        // index any more and the window has to be walked in menu order with
        // the fixed entries skipped. Build the order the screen will build --
        // the same function, so the two cannot disagree -- and ask for the
        // apps among the nine items the arc will be showing: seven resting
        // positions plus the one entering and the one leaving.
        Gui::Config::Menu::Top::ItemRef items[Gui::Config::Menu::Top::kMaxOrderItems];
        const int16_t itemCount = Gui::Config::Menu::Top::buildOrder(
                cache->list.get(), Gui::Config::Menu::Top::kMaxOrderApps, items,
                Gui::Config::Menu::Top::kMaxOrderItems);

        // Top::ID_DEFAULT is "wherever Settings is" and not an item id -- the
        // Home screen leaves it here on its way past. Resolve it against the
        // order just built: read raw it is 0xFFFF, which as an item id is -1,
        // and the window then sits off the front of the list so the second
        // half of the arc opens without its icons.
        int16_t centreId = static_cast<int16_t>(mMenuPosition.top);
        if (mMenuPosition.top == Gui::Config::Menu::Top::ID_DEFAULT) {
            centreId = Gui::Config::Menu::Top::itemForFixed(
                    items, itemCount, Gui::Config::Menu::Top::ID_SETTINGS);
        }
        if (centreId < 0) {
            centreId = 0;
        }

        uint64_t topIds[G2BEvent::UserAppIconWindow::kMax];
        uint8_t topCount = 0;

        for (int16_t off = -4; (off <= 4) && (topCount < G2BEvent::UserAppIconWindow::kMax); ++off) {
            const int16_t id = static_cast<int16_t>(centreId + off);
            if (id < 0 || id >= itemCount) {
                continue;   // past an end; the arc leaves those positions empty
            }

            const int16_t appIdx = items[id].appIdx;
            if (appIdx < 0 || appIdx >= static_cast<int16_t>(size)) {
                continue;   // a fixed entry draws from the bitmap database
            }

            topIds[topCount++] = (*cache->list)[appIdx].appId;
        }

        if (topCount > 0) {
            appRequestIconWindow(type, topIds, topCount);
        }
        return;
    }

    // The tracking wheel is still a plain list of apps, so its window is a
    // straight slice of it.
    int32_t centre = 0;
    int32_t behind = 0;
    uint8_t span   = 0;
    bool    wraps  = false;

    {
        // The wheel renders five rows centred on the selection and one more at
        // each end while it turns -- and it comes round on itself, so the rows
        // above the first item are the last ones in the list. Clamping here
        // instead of wrapping is what left those rows without icons until
        // something else made the wheel redraw.
        centre = static_cast<int32_t>(mMenuPosition.tracking);
        behind = 3;
        span   = 7;
        wraps  = (size >= 3);   // matches OrbitMenu::setCircularMinItems(3)
    }

    uint64_t appIds[G2BEvent::UserAppIconWindow::kMax];
    uint8_t count = 0;

    for (int32_t offset = -behind;
         (count < span) && (offset < behind + static_cast<int32_t>(span));
         ++offset) {
        int32_t idx = centre + offset;

        if (wraps) {
            idx %= size;
            if (idx < 0) {
                idx += size;
            }
        } else if (idx < 0 || idx >= size) {
            continue;   // past an end of a list that does not wrap
        }

        const uint64_t appId = (*cache->list)[idx].appId;

        // Wrapping a list shorter than the span brings the same item round
        // more than once.
        bool already = false;
        for (uint8_t i = 0; i < count; ++i) {
            if (appIds[i] == appId) {
                already = true;
                break;
            }
        }

        if (!already) {
            appIds[count++] = appId;
        }
    }

    if (count > 0) {
        appRequestIconWindow(type, appIds, count);
    }
}

uint16_t Model::getPosMenuTop()
{
    return mMenuPosition.top;
}

void Model::setPosMenuTopSettings(uint16_t p)
{
    mMenuPosition.settings = p;
}

uint16_t Model::getPosMenuTopSettings()
{
    return mMenuPosition.settings;
}

void Model::setPosMenuTracking(uint16_t p)
{
    mMenuPosition.tracking = p;

    // The position the wheel will open at, known at the earliest moment -- so
    // the icons it will want are fetched now rather than after its first frame.
    // Same reasoning as setPosMenuTop().
    prefetchIcons(App::Type::kActivity);
}

uint16_t Model::getPosMenuTracking()
{
    return mMenuPosition.tracking;
}

void Model::setPosNotificationListUid(uint32_t p)
{
    mMenuPosition.notificationListUid = p;
}

uint32_t Model::getPosNotificationListUid()
{
    return mMenuPosition.notificationListUid;
}

void Model::setPosMenuGlances(uint16_t p)
{
    mMenuPosition.glances = p;
}

uint16_t Model::getPosMenuGlances()
{
    return mMenuPosition.glances;
}

void Model::setPosMenuBleFCC(uint16_t p)
{
    mMenuPosition.bleFCC = p;
}

uint16_t Model::getPosMenuBleFCC()
{
    return mMenuPosition.bleFCC;
}

void Model::setPosMenuGNSS(uint16_t p)
{
    mMenuPosition.gnss = p;
}

uint16_t Model::getPosMenuGNSS()
{
    return mMenuPosition.gnss;
}

void Model::setPosMenuSensors(uint16_t p)
{
    mMenuPosition.sensors = p;
}

uint16_t Model::getPosMenuSensors()
{
    return mMenuPosition.sensors;
}

void Model::setPosMenuClock(uint16_t p)
{
    mMenuPosition.clock = p;
}

uint16_t Model::getPosMenuClock()
{
    return mMenuPosition.clock;
}

// Power
void Model::shutdown()
{
    mBackend.sendEventToBackend(G2BEvent::Shutdown {});
}

// Device information
const DeviceInfo& Model::getDeviceInfo()
{
    return mDeviceInfo;
}

// Date/Time
void Model::getDate(uint8_t &m, uint8_t &d, uint8_t &wd)
{
    m = mDateTime.tm_mon;
    d = mDateTime.tm_mday;
    wd = mDateTime.tm_wday;
}

void Model::getTime(uint8_t &h, uint8_t &m, uint8_t &s)
{
    h = mDateTime.tm_hour;
    m = mDateTime.tm_min;
    s = mDateTime.tm_sec;
}

std::tm Model::getDateTime()
{
    return mDateTime;
}

// Battery
uint8_t Model::batteryGetLevel()
{
    return mBattery.level;
}

bool Model::batteryIsLow()
{
    return mBattery.low;
}

bool Model::batteryIsShutdown()
{
    return mBattery.shutdown;
}

void Model::batteryPlayLow()
{
    G2BEvent::VibroCtrl data {};
    data.notes[0].effect = Interface::IVibro::ALERT_750MS_100;
    data.num = 1;
    mBackend.sendEventToBackend(data);
}

void Model::batteryMuteLow()
{
    mBattery.muted = true;
}

bool Model::batteryIsMuted()
{
    return mBattery.muted;
}

// USB
bool Model::usbIsConnected()
{
    return mUsb.connected;
}

bool Model::usbIsPortOpened()
{
    return mUsb.portOpened;
}

bool Model::usbIsCharging()
{
    return mUsb.charging;
}

bool Model::chargingIsBlocked()
{
    return mUsb.blocked;
}

bool Model::chargingDismiss()
{
    // Refused while the host owns the volume: the charging screen is all that
    // keeps the user out of the menu, and an app started from there would
    // write to a volume the host has mounted.
    if (mUsb.msdOpen) {
        return false;
    }
    mUsb.dismissed = true;
    return true;
}

bool Model::chargingIsDismissed()
{
    return mUsb.dismissed;
}

// BLE
bool Model::bleIsBonded()
{
    return mBle.bonded;
}

uint8_t Model::bleGetBondedCount()
{
    return mBle.bondedCount;
}

bool Model::bleIsConnected()
{
    return mBle.connected;
}

uint32_t Model::bleGetPinCode()
{
    return mBle.pinCode;
}

void Model::bleStartPairing()
{
    mBle.pairingActive = true;
    mBle.manualStart = true; // Pairing initiated by user
    mBle.reconnected = false;
    mBle.pinCode = 0;
    mBackend.sendEventToBackend(G2BEvent::BleStartPairing {});
}

void Model::bleAbortPairing()
{
    mBle.pairingActive = false;
    mBle.manualStart = false;
    mBle.pinCode = 0;
    mBackend.sendEventToBackend(G2BEvent::BleAbortPairing {});
}

void Model::bleUnpair()
{
    mBackend.sendEventToBackend(G2BEvent::BleUnpair {});
    // Clean BLE state
    mBle.bonded = false;
    mBle.bondedCount = 0;
    mBle.connected = false;
    mBle.pairingActive = false;
    mBle.pinCode = 0;
    mBle.manualStart = false;
}

bool Model::bleIsPairingActive()
{
    return mBle.pairingActive;
}

bool Model::bleIsPairingManual()
{
    return mBle.manualStart;
}

bool Model::bleIsReconnected()
{
    return mBle.reconnected;
}

void Model::bleStartFCC(const BleChannelsFCC::Info& ch)
{
    mBackend.sendEventToBackend(G2BEvent::BleStartFCC{ ch });
}

void Model::bleStopFCC()
{
    mBackend.sendEventToBackend(G2BEvent::BleStopFCC{});
}


// BLE Phone notification
void Model::phoneNotificationPlay()
{
    // Muted means no alerting of any kind, lighting the screen included: a
    // notification must not glow in a cinema or a dark bedroom. Only the
    // alerting is suppressed -- the notification is still stored and reachable
    // from the notification list. The backlight is gated here rather than in
    // the backend's BacklightCtrl handler because that handler also serves the
    // BLE pairing PIN, which the user has to be able to read while muted.
    if (mSettings.local.alertMutedFlag) {
        return;
    }

    mBackend.sendEventToBackend(G2BEvent::BacklightCtrl {true, 1000});

    if (mSettings.local.vibro.msgCalls) {
        G2BEvent::VibroCtrl vdata {};
        vdata.notes[0].effect = Interface::IVibro::ALERT_1000MS_100;
        vdata.num = 1;
        mBackend.sendEventToBackend(vdata);
        mNotificationVibroStarted = true;
    }

    if (mSettings.local.sound.msgCalls) {
        G2BEvent::BuzzerCtrl bdata{};
        bdata.notes[0] = { 100, 3 };
        bdata.notes[1] = { 100, 1 };
        bdata.notes[2] = { 100, 3 };
        bdata.notes[3] = { 100, 1 };
        bdata.notes[4] = { 100, 3 };
        bdata.notes[5] = { 100, 1 };
        bdata.notes[6] = { 100, 3 };
        bdata.num = 7;

        mBackend.sendEventToBackend(bdata);
        mNotificationBuzzerStarted = true;
    }
}

void Model::phoneNotificationStop()
{
    // Keyed on what this screen actually started, not on the msgCalls settings:
    // it deactivates on every close, including one whose alert muting
    // suppressed, and the backend forwards a stop unconditionally. An unmatched
    // stop would therefore silence an app's alert instead -- interval cues are
    // exempt from muting and can legitimately be playing here. It also stays
    // correct if msgCalls changes between the play and the stop.
    //
    // This does not make the hand-off airtight: the flag outlives the ~1s
    // pattern, so a matched stop at the screen's 5s close can still cut an app
    // cue that started in between. Closing that needs the backend to track
    // which side played last; it is a pre-existing race, not one added here.
    if (mNotificationVibroStarted) {
        mBackend.sendEventToBackend(G2BEvent::VibroCtrl {});
        mNotificationVibroStarted = false;
    }

    if (mNotificationBuzzerStarted) {
        mBackend.sendEventToBackend(G2BEvent::BuzzerCtrl {});
        mNotificationBuzzerStarted = false;
    }
}

bool Model::phoneNotificationIsActive()
{
    return std::any_of(mPhoneNotifications.begin(), mPhoneNotifications.end(),
        [](const std::shared_ptr<PhoneNotification>& notification) {
            return (!notification->isShown() && !notification->isRead()) ||
                notification->getCategory() == PhoneNotification::Category::ACTIVE_CALL ||
                notification->getCategory() == PhoneNotification::Category::INCOMING_CALL;
        });
}

void Model::phoneNotificationClearAll()
{
    for (const auto &notification : mPhoneNotifications) {
        if (notification) {
            mBackend.sendEventToBackend(G2BEvent::PhoneNotificationNegativeAction {notification});
        }
    }

    mPhoneNotifications.clear();
    HeapTelemetry::PhoneNotificationCount().store(0, std::memory_order_relaxed);

    modelListener->onPhoneNotificationRemoved(0, true);
    modelListener->onPhoneNotificationListChanged(mPhoneNotifications);
}

std::shared_ptr<PhoneNotification> Model::phoneNotificationGetByUID(uint32_t uid)
{
    auto it = std::find_if(mPhoneNotifications.begin(), mPhoneNotifications.end(),
        [&](const std::shared_ptr<PhoneNotification>& notification) {
            return notification->getUid() == uid;
        });

    if (it != mPhoneNotifications.end()) {
        return *it;
    }
    else {
        return nullptr;
    }
}

std::shared_ptr<PhoneNotification> Model::phoneNotificationGetNextActive()
{
    // Looking for active call
    auto it = std::find_if(mPhoneNotifications.rbegin(), mPhoneNotifications.rend(),
        [](const std::shared_ptr<PhoneNotification>& notification) {
            return notification->getCategory() == PhoneNotification::Category::ACTIVE_CALL;
        });
    if (it != mPhoneNotifications.rend()) {
        return *it;
    }

    // Incoming Call
    it = std::find_if(mPhoneNotifications.rbegin(), mPhoneNotifications.rend(),
        [](const std::shared_ptr<PhoneNotification>& notification) {
            return notification->getCategory() == PhoneNotification::Category::INCOMING_CALL;
        });
    if (it != mPhoneNotifications.rend()) {
        return *it;
    }

    // Other messages
    it = std::find_if(mPhoneNotifications.rbegin(), mPhoneNotifications.rend(),
        [](const std::shared_ptr<PhoneNotification>& notification) {
            return !notification->isShown() && !notification->isRead();
        });

    if (it != mPhoneNotifications.rend()) {
        return *it;
    }
    else {
        return nullptr;
    }
}

const std::vector<std::shared_ptr<PhoneNotification>>& Model::phoneNotificationGetAll() const
{
    return mPhoneNotifications;
}

void Model::phoneNotificationPositiveAction(const std::shared_ptr<PhoneNotification>& notification)
{
    mBackend.sendEventToBackend(G2BEvent::PhoneNotificationPositiveAction{ notification });
}

void Model::phoneNotificationNegativeAction(const std::shared_ptr<PhoneNotification>& notification)
{
    mBackend.sendEventToBackend(G2BEvent::PhoneNotificationNegativeAction{ notification });
}

std::shared_ptr<NowPlaying> Model::mediaGetNowPlaying() const
{
    return mNowPlaying;
}

void Model::mediaRemoteCommand(MediaCommand cmd)
{
    mBackend.sendEventToBackend(G2BEvent::MediaRemoteCommand{ static_cast<uint8_t>(cmd) });
}

void Model::phoneNotificationSetUidForDetails(uint32_t uid)
{
    mPhoneNotificationUidForDetails = uid;
}

uint32_t Model::phoneNotificationGetUidForDetails()
{
    return mPhoneNotificationUidForDetails;
}




// Settings
const WatchSettings& Model::getCommonSettings()
{
    return mSettings.common;
}

const WatchSettingsLocal& Model::getLocalSettings()
{
    return mSettings.local;
}

void Model::saveLocalSettings(const WatchSettingsLocal &sett)
{
    mSettings.local = sett;
    mBackend.sendEventToBackend(G2BEvent::SettingsLocal {mSettings.local});
}

void Model::setSleepMode(bool on)
{
    mBackend.sendEventToBackend(G2BEvent::SleepModeSet {on});
}

void Model::resetSettings()
{
    mBackend.sendEventToBackend(G2BEvent::ResetSettings{});
}



// APPs
void Model::appRequestList(App::Type type)
{
    mBackend.sendEventToBackend(G2BEvent::UserAppListReq{type});
}

void Model::appRequestIds(App::Type type)
{
    mBackend.sendEventToBackend(G2BEvent::UserAppIdsReq{type});
}

void Model::appRequestIconWindow(App::Type type, const uint64_t *appIds, uint8_t count)
{
    G2BEvent::UserAppIconWindow req {};
    req.type = type;
    req.count = (count < G2BEvent::UserAppIconWindow::kMax)
              ? count
              : G2BEvent::UserAppIconWindow::kMax;

    for (uint8_t i = 0; i < req.count; ++i) {
        req.appIds[i] = appIds[i];
    }

    // Asking again for the window already in hand costs a reply, and a reply in
    // flight pins every icon it carries -- which is what runs the icon pool dry
    // when a menu opens and asks two or three times over for the same set.
    // Anything that actually moves the window differs here and goes through.
    if (AppCache* cache = cacheFor(type)) {
        if (cache->askedCount == req.count) {
            bool same = true;
            for (uint8_t i = 0; i < req.count; ++i) {
                if (cache->asked[i] != req.appIds[i]) {
                    same = false;
                    break;
                }
            }

            if (same) {
                return;
            }
        }

        cache->askedCount = req.count;
        for (uint8_t i = 0; i < req.count; ++i) {
            cache->asked[i] = req.appIds[i];
        }
    }

    mBackend.sendEventToBackend(req);
}

bool Model::appStart(uint32_t pid, Gui::Config::AppLaunchOrigin origin)
{
    LOG_DEBUG("appStart [pid = 0x%08X, origin = %u]\n", pid, static_cast<unsigned>(origin));

    // One launch at a time. The backend event loop is serialised and blocks
    // for the whole of a launch, so a second request queued behind the first
    // is not answered until both have run - long enough for the pending timer
    // to fire and tell the user the second one failed while it was still
    // starting. Refusing here keeps the pending state single-valued, and a
    // press during a launch is one the user cannot have meant to overtake it.
    if (mPendingLaunch.ticksLeft > 0) {
        LOG_DEBUG("appStart ignored, launch for pid = 0x%08X still pending\n",
                mPendingLaunch.pid);
        return false;
    }

    // A new launch retires the expired one. Keeping it would let the previous
    // attempt's answer, still in flight, reach the screen ahead of this one:
    // it no longer matches the pending entry, but it would still match the
    // expired entry and report a failure the user has moved past over a launch
    // that may be about to succeed. Retiring it sends that answer down the
    // stale path instead, which is where an answer nobody is waiting for
    // belongs. The attempt identifier is what makes that safe - the pid cannot
    // tell the two apart, because retrying the same app reuses it.
    mExpiredLaunch = {};
    mLaunchAttempt++;

    // Remember where the launch came from until the backend confirms the start
    // (UserAppStartReq), which is where the app's context is created.
    mPendingLaunch.pid = pid;
    mPendingLaunch.attempt = mLaunchAttempt;
    mPendingLaunch.origin = origin;
    mPendingLaunch.ticksLeft = Gui::Config::kAppLaunchOriginTimeout;

    if (!mBackend.sendEventToBackend(G2BEvent::UserAppStart{ pid, mLaunchAttempt })) {
        // The request never left, so no timer will rescue it and no backend
        // answer is coming. Without this the press does nothing at all and
        // looks exactly like the silent failure this path exists to remove.
        mPendingLaunch = {};
        showAppLaunchFailure(App::LaunchFailure::OTHER, origin);
        return false;
    }

    return true;
}

bool Model::appGlancesStart()
{
    return mBackend.sendEventToBackend(G2BEvent::UserAppGlancesStart{});
}

bool Model::appGlancesStop()
{
    return mBackend.sendEventToBackend(G2BEvent::UserAppGlancesStop{});
}

bool Model::appGlancesUpdate()
{
    return mBackend.sendEventToBackend(G2BEvent::UserAppGlancesListReq{});
}

bool Model::appResume()
{
    LOG_DEBUG("appResume\n");

    auto pCtx = getActiveApp();

    if (pCtx) {
        LOG_DEBUG("Current active app [pid = 0x%08X] -> resume\n", pCtx->pid);
        mBackend.sendEventToBackend(G2BEvent::UserAppResume{ pCtx->pid });
        return true;
    }

    // No apps in list
    LOG_DEBUG("There are no apps in the list.\n");
    return false;
}

void Model::appPause()
{
    LOG_DEBUG("appPause\n");

    auto pCtx = getActiveApp();

    if (pCtx) {
        LOG_DEBUG("Current active app [pid = 0x%08X] -> pause\n", pCtx->pid);
        mBackend.sendEventToBackend(G2BEvent::UserAppPause{ pCtx->pid });
        return;
    }

    // No apps in list
    LOG_DEBUG("There are no apps in the list.\n");
}

void Model::appStopAll()
{
    LOG_DEBUG("appStopAll\n");
    // Stop all apps
    mBackend.sendEventToBackend(G2BEvent::UserAppStop{ 0 });
    mActiveApps.clear();
}

// WatchFace APP
void Model::setScreenFrameRate(uint32_t rate)
{
    LOG_DEBUG("setScreenFrameRate %u\n", static_cast<unsigned>(rate));
    mBackend.sendEventToBackend(G2BEvent::ScreenFrameRate{ rate });
}

void Model::setHomeScreenShown(bool shown)
{
    LOG_DEBUG("setHomeScreenShown %u\n", static_cast<unsigned>(shown));
    mBackend.sendEventToBackend(G2BEvent::HomeScreenShown{ shown });
}

void Model::watchFacePreview(uint64_t show, uint64_t prev, uint64_t next)
{
    LOG_DEBUG("watchFacePreview\n");
    mBackend.sendEventToBackend(G2BEvent::WatchFacePreview{ show, prev, next });
}

void Model::watchFacePreviewEnd(uint64_t keep)
{
    LOG_DEBUG("watchFacePreviewEnd\n");
    mBackend.sendEventToBackend(G2BEvent::WatchFacePreviewEnd{ keep });
}

void Model::saveWatchFace(uint64_t appId)
{
    LOG_DEBUG("saveWatchFace\n");

    WatchSettingsLocal sett = mSettings.local;
    sett.watchfaceAppId = appId;
    saveLocalSettings(sett);
}

// Firmware update
bool Model::firmwareUpdateActive()
{ 
    return mFwUpdate.active; 
}

uint8_t Model::firmwareUpdateGetPct()
{ 
    return mFwUpdate.pct; 
}

uint32_t Model::appGetActivePID()
{
    auto pCtx = getActiveApp();
    if (pCtx) {
        return pCtx->pid;
    }
    return 0;
}

void Model::requestEpoUpdate()
{
    mBackend.sendEventToBackend(G2BEvent::RequestEpo{ });
}

uint32_t Model::getHR()
{
    return mDailyHealth.liveHeartRate;
}

uint32_t Model::getAvgHR()
{
    return mDailyHealth.averageHeartRate;
}

uint32_t Model::getRHR()
{
    return mDailyHealth.restingHeartRate;
}

uint32_t Model::getDailyActivityTime()
{
    return mDailyHealth.activityMinutes;
}

uint32_t Model::getDailySteps()
{
    return mDailyHealth.steps;
}

uint32_t Model::getDailyFloors()
{
    return mDailyHealth.floors;
}

void Model::systemErrorPlay(bool on)
{
    mBackend.sendEventToBackend(G2BEvent::PlayError{ on });
}

void Model::acknowledgeError()
{
    mBackend.sendEventToBackend(G2BEvent::AcknowledgeError{ });
}

void Model::ringing(bool state)
{
    mBackend.sendEventToBackend(G2BEvent::FindPhone{state});
}





// Private
Model::AppContext* Model::getActiveApp()
{
    if (mActiveApps.empty()) {
        return nullptr; // No apps
    }

    return &mActiveApps.front();
}


Model::AppContext* Model::findApp(uint32_t pid)
{
    auto it = std::find_if(mActiveApps.begin(), mActiveApps.end(),
        [&](const AppContext& ctx) {
            return ctx.pid == pid;
        });
    if (it != mActiveApps.end()) {
        return &*it;
    }
    return nullptr;
}

Model::AppContext& Model::moveAppToFront(uint32_t pid)
{
    auto it = std::find_if(mActiveApps.begin(), mActiveApps.end(),
        [&](const AppContext& ctx) {
            return ctx.pid == pid;
        });

    if (it != mActiveApps.end()) {
        // App already in active apps list, move to the first position
        std::rotate(mActiveApps.begin(), it, it + 1);
    }
    else {
        // Add app to active apps list
        AppContext newCtx {};
        newCtx.pid = pid;
        mActiveApps.insert(mActiveApps.begin(), newCtx);
    }

    it = mActiveApps.begin(); // Get first app in active apps list

    return *it;
}

void Model::removeApp(uint32_t pid)
{
    auto it = std::find_if(mActiveApps.begin(), mActiveApps.end(),
        [&](const AppContext& ctx) {
            return ctx.pid == pid;
        });

    if (it != mActiveApps.end()) {
        it = mActiveApps.erase(it);
    }
}



void Model::decPendingLaunchTimer()
{
    if (mPendingLaunch.ticksLeft > 0) {
        mPendingLaunch.ticksLeft--;
        if (mPendingLaunch.ticksLeft == 0) {
            LOG_WARNING("Launch for pid = 0x%08X expired unconfirmed\n", mPendingLaunch.pid);
            const Gui::Config::AppLaunchOrigin origin = mPendingLaunch.origin;
            mExpiredLaunch = { mPendingLaunch.pid, mPendingLaunch.attempt,
                               origin, true };
            mPendingLaunch = {};

            // Nothing came back at all - a wedged backend, or an event lost
            // on the way. Report it as a timeout rather than leaving the
            // launcher looking like the press did nothing.
            showAppLaunchFailure(App::LaunchFailure::TIMEOUT, origin);
        }
    }
}

void Model::decIdleTimer()
{
    if (mIdleTimer > 0) {
        mIdleTimer--;
        if (mIdleTimer == 0) {
            //LOG_DEBUG("Screen timeout\n");
            modelListener->onIdleTimeout();
        }
    }
}


void Model::onDataEvent(const B2GEvent::AppSurfaceConfig &evt)
{
    LOG_DEBUG("AppSurface [%dx%d]\n", evt.surface.width, evt.surface.height);
    mAppSurface = evt.surface;
}

void Model::onDataEvent(const B2GEvent::WatchFaceRunning &evt)
{
    LOG_DEBUG("WatchFaceRunning [pid = 0x%08X]\n", evt.guiPid);
    mWatchFacePid = evt.guiPid;
    modelListener->onWatchFaceRunning(evt.guiPid);
}

void Model::onDataEvent(const B2GEvent::Booted &evt)
{
    LOG_DEBUG("Booted\n");
    mIsBooted = true;

    // Fetched now so the first opening of either menu is as whole as every one
    // after it, rather than assembling itself while the user watches. The boot
    // scan has finished by the time this arrives, and a list is only a few
    // kilobytes; each leads on to the icons its own menu opens with.
    appRequestList(App::Type::kUtility);
    appRequestList(App::Type::kActivity);

    modelListener->onBooted();
}

void Model::onDataEvent(const B2GEvent::DevInfo &evt)
{
    LOG_DEBUG("DeviceInfo\n");
    mDeviceInfo = evt.info;
}

void Model::onDataEvent(const B2GEvent::BleBond &evt)
{
    LOG_DEBUG("BleBond [count = %u]\n", evt.count);
    mBle.bonded = evt.count > 0;
    mBle.bondedCount = evt.count;
    modelListener->onBleBond(mBle.bonded);
}

void Model::onDataEvent(const B2GEvent::BleConnect &evt)
{
    LOG_DEBUG("BleConnect [status = %s]\n", evt.status ? "connected" : "disconnected");
    if (mBle.connected != evt.status) {
        mBle.connected = evt.status;
        if (!mBle.connected) {
            mBle.pairingActive = false;
        }
        modelListener->onBleConnect(mBle.connected);
    }
}

void Model::onDataEvent(const B2GEvent::BlePinCode &evt)
{
    mBackend.sendEventToBackend(G2BEvent::BacklightCtrl{ true, 3000 });
    LOG_DEBUG("BlePinCode [code = %06u]\n", evt.code);
    mBle.pinCode = evt.code;
    mBle.pairingActive = true;
    modelListener->onBlePinCode();
}

void Model::onDataEvent(const B2GEvent::BlePair &evt)
{
    LOG_DEBUG("BlePair [status = %s]\n", evt.status ? "success" : "fail");
    mBle.reconnected = false;
    modelListener->onBlePair(evt.status);
    mBle.pairingActive = false;
}

void Model::onDataEvent(const B2GEvent::BleReconnect &evt)
{
    LOG_DEBUG("BleReconnect\n");
    if (mBle.pairingActive) {
        mBle.pairingActive = false;
        mBle.manualStart = false;
        mBle.reconnected = true;
        mBle.pinCode = 0;
        modelListener->onBleReconnect();
    }
}

void Model::onDataEvent(const B2GEvent::BatteryLevel &evt)
{
    LOG_DEBUG("BatteryLevel [level = %.1f]\n", evt.level);
    float lvl = evt.level;
    if (lvl > 100) {
        lvl = 100;
    }

    if (lvl < 0) {
        lvl = 0;
    }

    uint8_t level = static_cast<uint8_t>(lvl);

    // This event only drives the displayed level and the low-battery warning.
    // Shutdown is driven solely by the LowBatteryShutdown event.
    if (mBattery.level != level) {
        mBattery.level = level;

        modelListener->onBatteryLevel(mBattery.level);

        if (!mBattery.low && mBattery.level
                < Gui::Config::Battery::kLowBatteryThreshold) {
            mBattery.low = true;
            modelListener->onLowBattery();
        }

        if (mBattery.low && mBattery.level
                > Gui::Config::Battery::kLowBatteryThreshold) {
            mBattery.low = false;
            mBattery.muted = false;
        }
    }
}

void Model::onDataEvent(const B2GEvent::LowBatteryShutdown &evt)
{
    // The watch must power off. Latch it so switchToNextPriorityScreen and the
    // splash screen also route to shutdown if this event is missed, then
    // switch to the shutdown screen from any screen.
    mBattery.shutdown = true;

    appStopAll();
    modelListener->onLowBatteryShutdown();
}

void Model::onDataEvent(const B2GEvent::BatteryCharge &evt)
{
    LOG_DEBUG("BatteryCharge [status = %s]\n", evt.status ? "on" : "off");
    if (mUsb.charging != evt.status) {
        mUsb.charging = evt.status;

        auto app = getActiveApp();
        if (!app || app->caps.enUsbChargingScreen) {
            modelListener->onUsbState(mUsb.portOpened, mUsb.charging);
        }
    }
}

void Model::onDataEvent(const B2GEvent::ChargingBlocked &evt)
{
    LOG_DEBUG("ChargingBlocked [status = %s]\n", evt.status ? "yes" : "no");
    if (mUsb.blocked == evt.status) {
        return;
    }
    mUsb.blocked = evt.status;

    // Raising the block needs nothing here: whatever raised it also raised an
    // error, and that takes the screen. Dropping it does: the watch is
    // charging again while the cable never moved, so restore the screen the
    // plug would have shown instead of leaving it stale until the next
    // cable event.
    if (!mUsb.blocked && mUsb.connected) {
        switchToNextPriorityScreen();
    }
}

void Model::onDataEvent(const B2GEvent::UsbConnect &evt)
{
    LOG_DEBUG("UsbConnect [status = %s]\n", evt.status ? "connected" : "disconnected");
    if (mUsb.connected != evt.status) {
        mUsb.connected = evt.status;
        // A dismissal lasts for one plug-in only.
        mUsb.dismissed = false;
        // No cable, no mass storage. Cleared here as well as by MsdState, so
        // a dropped MsdState{false} cannot leave Back refused on the next
        // plug-in.
        if (!mUsb.connected) {
            mUsb.msdOpen = false;
        }
        auto app = getActiveApp();
        if (!app || app->caps.enUsbChargingScreen) {
            if (mUsb.connected) {
                modelListener->onUsbConnect();
            } else {
                modelListener->onUsbDisconnect();
            }
        }
    }
}

void Model::onDataEvent(const B2GEvent::UsbPortOpen &evt)
{
    LOG_DEBUG("UsbPortOpen [status = %s]\n", evt.status ? "opened" : "closed");
    if (mUsb.portOpened != evt.status) {
        mUsb.portOpened = evt.status;
        modelListener->onUsbState(mUsb.portOpened, mUsb.charging);
    }
}

void Model::onDataEvent(const B2GEvent::MsdOpeningRequest &evt)
{
    LOG_DEBUG("MSD Opening Request\n");
    mBackend.sendEventToBackend(G2BEvent::MsdOpen{true});
}

void Model::onDataEvent(const B2GEvent::MsdState &evt)
{
    LOG_DEBUG("MsdState [open = %s]\n", evt.open ? "yes" : "no");
    if (mUsb.msdOpen == evt.open) {
        return;
    }
    mUsb.msdOpen = evt.open;

    // A charging screen backed out of before the host enumerated would leave
    // the user in the menu with the volume gone. Withdraw the dismissal and
    // bring the screen back; an app still running gives way when the backend
    // stops it, which re-runs this choice.
    if (mUsb.msdOpen) {
        mUsb.dismissed = false;
        switchToNextPriorityScreen();
    }
}

void Model::onDataEvent(const B2GEvent::Time &evt)
{
    //LOG_DEBUG("Time\n");
    const std::tm &timeNow = evt.time;

    bool dateChanged = (timeNow.tm_year != mDateTime.tm_year ||
        timeNow.tm_mon != mDateTime.tm_mon ||
        timeNow.tm_mday != mDateTime.tm_mday ||
        timeNow.tm_wday != mDateTime.tm_wday);

    // The backend decides how often the event arrives (per minute or per
    // second), so seconds count as a change here and the screen follows
    // whatever rate it is fed.
    bool timeChanged = (timeNow.tm_hour != mDateTime.tm_hour ||
        timeNow.tm_min != mDateTime.tm_min ||
        timeNow.tm_sec != mDateTime.tm_sec);
    mDateTime = timeNow;

    if (dateChanged) {
        modelListener->onDate(mDateTime.tm_mon, mDateTime.tm_mday, mDateTime.tm_wday);
    }

    if (timeChanged) {

        modelListener->onTime(mDateTime.tm_hour, mDateTime.tm_min, mDateTime.tm_sec);
    }
}

void Model::onDataEvent(const B2GEvent::FirmwareUpdate &evt)
{
    LOG_DEBUG("FirmwareUpdate [pct = %u%%]\n", evt.pct);
    if (mFwUpdate.active) {
        modelListener->onFirmwareUpdPct(evt.pct);
    }

    if (mFwUpdate.active != evt.status) {
        mFwUpdate.active = evt.status;
        if (mFwUpdate.active) {
            modelListener->onFirmwareUpdStart();
            modelListener->onFirmwareUpdPct(evt.pct);
        } else {
            modelListener->onFirmwareUpdEnd();
        }
    }
}

void Model::onDataEvent(const B2GEvent::PhoneNotificationAdd &evt)
{
    LOG_DEBUG("PhoneNotificationAdd #%d\n", evt.notification->getUid());

    auto it = std::find_if(mPhoneNotifications.begin(), mPhoneNotifications.end(),
        [&evt](const std::shared_ptr<PhoneNotification> &notification) {
        return *notification == *evt.notification;
    });

    if (it == mPhoneNotifications.end()) {
        bool evicted = false;
        if (mPhoneNotifications.size() >= skMaxPhoneNotifications) {
            evictOldestPhoneNotification();
            evicted = true;
        }
        mPhoneNotifications.push_back(evt.notification);
        // Hide notification if it disabled in APP
        auto app = getActiveApp();
        if (!app || app->caps.enPhoneNotification) {
            modelListener->onPhoneNotificationNew(evt.notification);
        } else {
            evt.notification->markAsShown();
        }
        if (evicted) {
            // An open list screen only re-renders on the list-changed event.
            modelListener->onPhoneNotificationListChanged(mPhoneNotifications);
        }
    } else {
        // Replace in place only when the user-visible content changed: the
        // update re-alerts and shows as unread. An identical re-post (e.g. a
        // persistent notification re-forwarded by the phone) is a no-op, so
        // the entry keeps its shown/read state and its received-at age.
        if (!(*it)->hasSameContent(*evt.notification)) {
            *it = evt.notification;
            auto app = getActiveApp();
            if (!app || app->caps.enPhoneNotification) {
                modelListener->onPhoneNotificationNew(evt.notification);
            } else {
                evt.notification->markAsShown();
            }
            modelListener->onPhoneNotificationListChanged(mPhoneNotifications);
        }
    }

    HeapTelemetry::PhoneNotificationCount().store(
            static_cast<uint32_t>(mPhoneNotifications.size()), std::memory_order_relaxed);
}

void Model::evictOldestPhoneNotification()
{
    // Oldest first (arrival order), but an ongoing/incoming call must not be
    // pushed out by a notification burst. If every slot somehow holds a call,
    // the oldest call is evicted anyway rather than dropping the new event.
    auto it = std::find_if(mPhoneNotifications.begin(), mPhoneNotifications.end(),
        [](const std::shared_ptr<PhoneNotification> &notification) {
            auto category = notification->getCategory();
            return category != PhoneNotification::Category::ACTIVE_CALL &&
                    category != PhoneNotification::Category::INCOMING_CALL;
        });

    if (it == mPhoneNotifications.end()) {
        it = mPhoneNotifications.begin();
    }

    mPhoneNotifications.erase(it);
}

void Model::onDataEvent(const B2GEvent::PhoneNotificationRemove &evt)
{
    LOG_DEBUG("PhoneNotificationRemove\n");

    if (!evt.all) {
        mPhoneNotifications.erase(
            std::remove_if(mPhoneNotifications.begin(), mPhoneNotifications.end(),
                [evt](const std::shared_ptr<PhoneNotification> &notification) {
            return notification->getUid() == evt.uid;
        }),
            mPhoneNotifications.end());
        
    } else {
        mPhoneNotifications.clear();
    }

    HeapTelemetry::PhoneNotificationCount().store(
            static_cast<uint32_t>(mPhoneNotifications.size()), std::memory_order_relaxed);

    modelListener->onPhoneNotificationRemoved(evt.uid, evt.all);
    modelListener->onPhoneNotificationListChanged(mPhoneNotifications);
}

void Model::onDataEvent(const B2GEvent::MediaNowPlaying &evt)
{
    mNowPlaying = evt.nowPlaying;
    if (modelListener) {
        modelListener->onMediaNowPlaying(mNowPlaying);
    }
}

void Model::onDataEvent(const B2GEvent::HomeWidget &evt)
{
    mHomeWidget = evt.data;
    if (modelListener) {
        modelListener->onHomeWidget(mHomeWidget);
    }
}

void Model::onDataEvent(const B2GEvent::MediaControlRequest &evt)
{
    (void) evt;

    // The screen up is using the hold for itself -- a list scrolling on while
    // the button is down -- so the hold has already been spent and must not
    // also land the user here when they let go.
    if (mL2HoldClaimed) {
        return;
    }

    // A long L2 press requests the media-control screen. Honor the active app's
    // capability (mirrors the phone-notification gate); with no active app it
    // always opens. Leaving the app screen pauses the app via the transition, and
    // exiting with R2 resumes it through switchToNextPriorityScreen().
    auto app = getActiveApp();
    if (app && !app->caps.enMusicControl) {
        return;
    }

    application().gotoMediaControlScreenNoTransition();
}

void Model::onDataEvent(const B2GEvent::FindPhoneConfirmation &evt)
{
    LOG_DEBUG("FindPhoneConfirmation\n");
    modelListener->onFindPhoneConfirmed();
}

void Model::onDataEvent(const B2GEvent::Settings &evt)
{
    LOG_DEBUG("Settings\n");
    if (mSettings.common != evt.settings) {
        mSettings.common = evt.settings;
        modelListener->onCommonSettings(mSettings.common);
    }
}

void Model::onDataEvent(const B2GEvent::SettingsLocal &evt)
{
    LOG_DEBUG("SettingsLocal\n");
    if (mSettings.local != evt.settings) {
        mSettings.local = evt.settings;
        modelListener->onLocalSettings(mSettings.local);
    }
}

void Model::onDataEvent(const B2GEvent::UserAppList &evt)
{
    LOG_DEBUG("UserAppList [type = %u, len = %u]\n", evt.type, evt.list->size());

    if (AppCache* cache = cacheFor(evt.type)) {
        cache->list = evt.list;

        // The list a boot fetch was waiting for: now it can be turned into a
        // window. Only while there are no icons at all -- after that the menu
        // itself is asking, and it knows better than this which it needs.
        //
        // Counted, not compared against null: icons is an array of handles, so
        // testing it for null asked whether its address was zero, which it
        // never is. The boot fetch therefore never ran, and the first opening
        // of either menu drew its labels a frame before its icons.
        if (cache->iconCount == 0) {
            prefetchIcons(evt.type);
        }
    }

    modelListener->onAppList(evt.type, evt.list);
}

void Model::onDataEvent(const B2GEvent::UserAppIds &evt)
{
    LOG_DEBUG("UserAppIds [type = %u, len = %u]\n", evt.type,
            static_cast<unsigned>(evt.ids->size()));
    modelListener->onAppIds(evt.type, evt.ids);
}

void Model::onDataEvent(const B2GEvent::UserAppIcons &evt)
{
    LOG_DEBUG("UserAppIcons [type = %u, len = %u]\n", evt.type,
            static_cast<unsigned>(evt.count));

    // An empty set is what a screen on its way out asks for, and is no use to
    // the next opening -- keep the last set that had something in it.
    if (AppCache* cache = cacheFor(evt.type)) {
        if (evt.count > 0) {
            for (uint8_t i = 0; i < App::kIconWindowMax; ++i) {
                cache->icons[i] = (i < evt.count) ? evt.icons[i] : App::IconRef();
            }
            cache->iconCount = evt.count;
        }

        // Short of what was asked for: something could not be found, or could
        // not be held. Forget the ask so the next look at the same window goes
        // through instead of being coalesced away as already in hand -- without
        // this the menu would keep whatever it had and never ask again until
        // the selection moved.
        if (evt.count < cache->askedCount) {
            cache->askedCount = 0;
        }
    }

    modelListener->onAppIcons(evt.type, evt.icons, evt.count);
}

void Model::onDataEvent(const B2GEvent::UserAppStartFailed& evt)
{
    LOG_WARNING("UserAppStartFailed [pid = 0x%08X, reason = %u]\n",
            evt.pid, static_cast<unsigned>(evt.reason));

    // Take the origin before releasing the entry - it is the only record of
    // where the user pressed, and the screen needs it to hand them back.
    // A failure for a pid we have no entry for (a backend-raised start) has
    // no origin to inherit, so it falls back to HOME.
    // Matched on the attempt rather than the pid. A failure for an attempt
    // the user has already moved on from must not touch the current one, and
    // retrying the same app reuses the pid, so the pid cannot distinguish
    // them.
    Gui::Config::AppLaunchOrigin origin = Gui::Config::AppLaunchOrigin::HOME;

    if (mPendingLaunch.ticksLeft > 0 && mPendingLaunch.attempt == evt.attempt) {
        origin = mPendingLaunch.origin;
        mPendingLaunch = {};
    } else if (mExpiredLaunch.valid && mExpiredLaunch.attempt == evt.attempt) {
        // The timer already reported this attempt as a timeout; this is its
        // real reason arriving a tick or more later. Same launch, so the same
        // way back - and the reason is better than the guess it replaces.
        origin = mExpiredLaunch.origin;
        mExpiredLaunch = {};
    } else if (evt.attempt != 0) {
        // A user attempt that matches neither the pending launch nor the
        // expired one is an answer to something nobody is waiting for. The
        // user has moved past it - into a newer launch, into the app that
        // launch started, or away from launching altogether - and reporting
        // it now would interrupt whatever replaced it. Only a launch the user
        // is still waiting on gets to speak.
        LOG_DEBUG("Stale launch failure for attempt %u, ignoring\n",
                evt.attempt);
        return;
    }

    showAppLaunchFailure(evt.reason, origin);
}

void Model::showAppLaunchFailure(App::LaunchFailure reason,
                                 Gui::Config::AppLaunchOrigin origin)
{
    mAppLaunchFailure = reason;
    mAppLaunchFailureOrigin = origin;

    // Through the listener, not straight to the screen: this can arrive on a
    // screen that must not be replaced (the shutdown countdown, an error), and
    // only the presenter knows that.
    modelListener->onAppLaunchFailed();
}

void Model::returnFromAppLaunchFailure()
{
    // Anything that came up while the message was on screen outranks the menu
    // the user was in. Both of these latch, so they are still true here and
    // would otherwise be dropped: the screens that raise them cannot re-raise
    // on their own.
    if (mBattery.shutdown) {
        application().gotoLowBattShutDownScreenNoTransition();
        return;
    }

    if (mErrorToShow != SystemError::NONE) {
        application().gotoErrorScreenNoTransition();
        return;
    }

    // Same mapping as the app-exit path above. Kept here rather than in the
    // view so there is one place that turns an origin into a screen.
    switch (mAppLaunchFailureOrigin) {
        case Gui::Config::AppLaunchOrigin::TOP_MENU:
            application().gotoTopMenuScreenNoTransition();
            return;
        case Gui::Config::AppLaunchOrigin::TRACKING_MENU:
            application().gotoTrackingMenuScreenNoTransition();
            return;
        case Gui::Config::AppLaunchOrigin::HOME:
            break;
    }

    application().gotoHomeScreenNoTransition();
}

void Model::onDataEvent(const B2GEvent::UserAppStartReq& evt)
{
    LOG_DEBUG("UserAppStartReq [pid = 0x%08X]\n", evt.pid);
    
    auto active = getActiveApp();
    if (active && active->pid != evt.pid) {
        LOG_DEBUG("Current active app [pid = 0x%08X] -> pause\n", active->pid);
        mBackend.sendEventToBackend(G2BEvent::UserAppPause{ active->pid });
    }

    // Move app to first position in active apps list
    auto &ctx = moveAppToFront(evt.pid);

    // Claim the origin recorded by appStart(), so exiting the app returns the
    // user to the menu they launched it from. Matched on the attempt, not the
    // pid: a start the backend raised by itself (attempt 0) for an app a
    // launch is already pending for would otherwise consume the entry and
    // leave a later failure with no way back. The timer bounds an entry
    // nobody claims.
    if (evt.attempt != 0 && mPendingLaunch.attempt == evt.attempt) {
        ctx.origin = mPendingLaunch.origin;
        mPendingLaunch = {};
    } else if (evt.attempt != 0 && mExpiredLaunch.valid
               && mExpiredLaunch.attempt == evt.attempt) {
        // The timer gave up on this attempt and the start arrived anyway. It
        // is the same launch, so it keeps the same way out - without this the
        // app starts with no origin and exiting it lands on Home.
        ctx.origin = mExpiredLaunch.origin;
        mExpiredLaunch = {};
    }

    const uint32_t pid = ctx.pid; // ctx may not outlive the callback
    modelListener->onAppStartReq(pid);
    // Not run the app here
}

void Model::onDataEvent(const B2GEvent::UserAppStopped& evt)
{
    LOG_DEBUG("UserAppStopped [pid = 0x%08X]\n", evt.pid);

    // If the app is stopped/paused, you can remove it from the list, 
    // the GUI has no more business with it.

    auto active = getActiveApp();
    if (active && active->pid == evt.pid) {
        LOG_DEBUG("Current active app [pid = 0x%08X] -> remove\n", active->pid);
        const Gui::Config::AppLaunchOrigin origin = active->origin;
        removeApp(active->pid);

        // If the active app is removed, we need to find the next active app
        active = getActiveApp();
        if (active) {
            LOG_DEBUG("Next active [pid = 0x%08X]\n", active->pid);
            modelListener->onAppStartReq(active->pid);
            // Not run the app here
        } else {
            // No active app, switch to next screen. Publish the origin only
            // across the callback so switchToNextPriorityScreen() can send the
            // user back to the menu the app was launched from.
            mAppExitOrigin = origin;
            modelListener->onAppStop();
            mAppExitOrigin = Gui::Config::AppLaunchOrigin::HOME;
        }
    } else {
        LOG_DEBUG("Background app [pid = 0x%08X] -> remove\n", evt.pid);
        removeApp(evt.pid);
    }
}

void Model::onDataEvent(const B2GEvent::UserAppCapabilities &evt)
{
    LOG_DEBUG("UserAppCapabilities [pid = 0x%08X]\n", evt.pid);
    auto pCtx = findApp(evt.pid);

    if (pCtx) {
        pCtx->caps = evt.caps;
    }
}

void Model::onDataEvent(const B2GEvent::UserAppGlanceList &evt)
{
    LOG_DEBUG("UserAppGlanceList [len = %u]\n", evt.list->size());
    modelListener->onAppGlanceList(evt.list);
}


void Model::onDataEvent(const B2GEvent::GpsFix &evt)
{
    LOG_DEBUG("GpsFix\n");
    if (mGpsSignalAcquired != evt.status) {
        mGpsSignalAcquired = evt.status;
        modelListener->onGpsSignal(mGpsSignalAcquired);
    }
}

void Model::onDataEvent(const B2GEvent::GpsEpoTime& evt)
{
    LOG_DEBUG("GpsEpoTime hoursLeft = %u\n", evt.hoursLeft);
    mGpsEpoTimeLeft = evt.hoursLeft;
    modelListener->onGpsEpoTime(mGpsEpoTimeLeft);
}

void Model::onDataEvent(const B2GEvent::GpsEpoSync& evt)
{
    LOG_DEBUG("GpsEpoSync = %u\n", evt.state);
    mGpsEpoSyncState = evt.state;
    modelListener->onGpsEpoSyncState(mGpsEpoSyncState);
}


void Model::onDataEvent(const B2GEvent::DailyHealth &evt)
{
    //LOG_DEBUG("DailyHealth\n");

    if (mDailyHealth.steps != evt.steps || mDailyHealth.floors != evt.floors || mDailyHealth.activityMinutes != evt.activityMinutes) {
        mDailyHealth.steps = evt.steps;
        mDailyHealth.floors = evt.floors;
        mDailyHealth.activityMinutes = evt.activityMinutes;
        modelListener->onDailyHealth(mDailyHealth.steps, mDailyHealth.floors, mDailyHealth.activityMinutes);
    }

    if (mDailyHealth.averageHeartRate != evt.averageHeartRate || mDailyHealth.restingHeartRate != evt.restingHeartRate) {
        mDailyHealth.averageHeartRate = evt.averageHeartRate;
        mDailyHealth.restingHeartRate = evt.restingHeartRate;
        modelListener->onHeartRate(mDailyHealth.liveHeartRate, mDailyHealth.averageHeartRate, mDailyHealth.restingHeartRate);
    }
}

void Model::onDataEvent(const B2GEvent::HeartRate &evt)
{
    //LOG_DEBUG("HeartRate %f\n", evt.heartRate);
    if (mDailyHealth.liveHeartRate != static_cast<uint32_t>(evt.heartRate)) {
        mDailyHealth.liveHeartRate = static_cast<uint32_t>(evt.heartRate);
        modelListener->onHeartRate(mDailyHealth.liveHeartRate, mDailyHealth.averageHeartRate, mDailyHealth.restingHeartRate);
    }
}

void Model::onDataEvent(const B2GEvent::Error &evt)
{
    LOG_DEBUG("Error display %u\n", static_cast<uint32_t>(evt.error));
    // Only update the data. mErrorToShow also keeps the Error slot in
    // switchToNextPriorityScreen so any screen that exits returns to the error
    // while it is still active. onError() owns the show/leave navigation.
    mErrorToShow = evt.error;
    modelListener->onError();
}

void Model::onDataEvent(const B2GEvent::AccessoryScanResult &evt)
{
    // Append unless we've already listed this address (the backend dedups per
    // session, but guard anyway). Consumed by the Settings ▸ Sensors screen.
    for (const auto &r : mAccessoryScanResults) {
        if (memcmp(r.addr, evt.addr, sizeof(r.addr)) == 0) {
            return;
        }
    }
    mAccessoryScanResults.push_back(evt);
    HeapTelemetry::AccessoryScanResultCount().store(
            static_cast<uint32_t>(mAccessoryScanResults.size()), std::memory_order_relaxed);
    modelListener->onAccessoryScanResult();
}

void Model::onDataEvent(const B2GEvent::AccessoryStatus &evt)
{
    mAccessoryStatus = evt;
    modelListener->onAccessoryStatus();
}

void Model::onDataEvent(const B2GEvent::AccessoryConfig &evt)
{
    mAccessoryConfig = evt;
    modelListener->onAccessoryConfig();
}

// Compass events are forwarded, not stored: only the compass screen listens.
void Model::onDataEvent(const B2GEvent::MagAzimuth &evt)
{
    modelListener->onMagAzimuth(evt.degrees, evt.valid, evt.faceTiltDeg);
}

void Model::onDataEvent(const B2GEvent::MagCalProgress &evt)
{
    modelListener->onMagCalProgress(evt.percent);
}

void Model::onDataEvent(const B2GEvent::MagCalDone &evt)
{
    modelListener->onMagCalDone(evt.completed, evt.stored);
}

void Model::accessorySetMode(uint8_t mode)
{
    mBackend.sendEventToBackend(G2BEvent::AccessorySetMode{mode});
}

void Model::magFeedStart()
{
    mBackend.sendEventToBackend(G2BEvent::MagFeedStart{});
}

void Model::magFeedStop()
{
    mBackend.sendEventToBackend(G2BEvent::MagFeedStop{});
}

void Model::magCalStart(uint32_t seconds)
{
    mBackend.sendEventToBackend(G2BEvent::MagCalStart{seconds});
}

void Model::magCalCancel()
{
    mBackend.sendEventToBackend(G2BEvent::MagCalCancel{});
}

void Model::accessoryScanStart()
{
    clearAccessoryScanResults();
    mBackend.sendEventToBackend(G2BEvent::AccessoryScanStart{});
}

void Model::accessoryScanStop()
{
    mBackend.sendEventToBackend(G2BEvent::AccessoryScanStop{});
}

void Model::accessoryForget(uint8_t index)
{
    mBackend.sendEventToBackend(G2BEvent::AccessoryForget{index});
}

void Model::accessoryRemember(const B2GEvent::AccessoryScanResult &dev)
{
    G2BEvent::AccessoryRemember r {};
    memcpy(r.name, dev.name, sizeof(r.name));
    memcpy(r.addr, dev.addr, sizeof(r.addr));
    r.addrType = dev.addrType;
    r.kind = dev.kind;
    mBackend.sendEventToBackend(r);
}

