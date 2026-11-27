
#include <queue>
#include <algorithm>
#include <chrono>
#include <ctime>

#include "touchgfx/UIEventListener.hpp"

#include "../../../../Libs/Header/Components/Shared/GuiBackendInterface.hpp"
#include "../../../../Libs/Header/Components/App/IconPool.hpp"
#include "../../../../Libs/Header/Components/SleepMode/SleepSchedule.hpp"


#include "SDK/Glance/GlanceControl.hpp"

#ifndef BACKEND_HPP
#define BACKEND_HPP


class Backend : virtual public IGuiBackend, public touchgfx::UIEventListener {

public:

    static Backend &GetInstance()
    {
        static Backend sInstance;
        return sInstance;
    }

    // Tick
    virtual void handleTickEvent() override
    {
        // Everything this stub produced last tick becomes visible to the GUI
        // now, and not before. See sendToGui().
        while (!mPendingTx.empty()) {
            mGuiTxQueue.push(mPendingTx.front());
            mPendingTx.pop();
        }

        updTime();
        updGps();
        updMag();



        while (!mGuiRxQueue.empty()) {
            auto data = mGuiRxQueue.front();
            mGuiRxQueue.pop();

            std::visit([this](auto &&value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return;
                } else if constexpr (std::is_same_v<T, G2BEvent::SettingsLocal>) {
                    // Kept, so the sleep answer below does not hand the GUI
                    // back settings older than the ones it just saved.
                    mLocalSettings = SleepMode::mergeFromGui(mLocalSettings, value.settings);
                } else if constexpr (std::is_same_v<T, G2BEvent::SleepModeSet>) {
                    LOG_DEBUG("Backend. Sleep mode %s\n", value.on ? "on" : "off");
                    SleepMode::set(mLocalSettings, value.on);
                    sendToGui(B2GEvent::SettingsLocal {mLocalSettings});
                } else  if constexpr (std::is_same_v<T, G2BEvent::BleStartPairing>) {
                    LOG_DEBUG("Backend. Start pairing\n");
                    mBlePairingWait = true;
                } else  if constexpr (std::is_same_v<T, G2BEvent::BleAbortPairing>) {
                    LOG_DEBUG("Backend. Abort pairing\n");
                    mBlePairingWait = false;
                    mBleCodeWait = false;
                } else if constexpr (std::is_same_v<T, G2BEvent::MagFeedStart>) {
                    mMagFeed     = true;
                    mMagFeedTick = 0;
                } else if constexpr (std::is_same_v<T, G2BEvent::MagFeedStop>) {
                    // Ends a run too; nobody listens for its MagCalDone.
                    mMagFeed     = false;
                    mMagRunTicks = 0;
                } else if constexpr (std::is_same_v<T, G2BEvent::MagCalStart>) {
                    if ((mMagRunTicks > 0) || (value.seconds == 0U)) {
                        // Refused, and answered so the screen does not wait.
                        sendToGui(B2GEvent::MagCalDone {false, false});
                    } else {
                        mMagRunTicks = value.seconds * 10U;   // 10 Hz tick
                        mMagRunLeft  = mMagRunTicks;
                        mMagRunTick  = 0;

                        sendToGui(B2GEvent::MagCalProgress {0});
                    }
                } else if constexpr (std::is_same_v<T, G2BEvent::MagCalCancel>) {
                    // Always answered, as on the watch.
                    mMagRunTicks = 0;

                    sendToGui(B2GEvent::MagCalDone {false, false});
                } else  if constexpr (std::is_same_v<T, G2BEvent::BleUnpair>) {
                    mBlePairingWait = false;
                    mBleCodeWait = false;
                    mBleBonded = false;
                } else if constexpr (std::is_same_v<T, G2BEvent::PhoneNotificationNegativeAction>) { 
                    sendToGui(B2GEvent::PhoneNotificationRemove {value.notification->getUid(), false});
                    if (value.notification->getCategory() == PhoneNotification::Category::INCOMING_CALL) {
                        std::tm timeNow = getSystemTime();
                        auto notification = std::make_shared<PhoneNotification>(
                            getTimeMs(),
                            PhoneNotification::Platform::IOS,
                            static_cast<uint32_t>(timeNowUTC()),
                            PhoneNotification::Category::MISSED_CALL, timeNow,
                            "Joe Doe",
                            "",
                            "Missed call",
                            "com.google.Gmail", "Dial", "Clear");

                        sendToGui(B2GEvent::PhoneNotificationAdd {notification, true});
                    }
                } else if constexpr (std::is_same_v<T, G2BEvent::PhoneNotificationPositiveAction>) {
                    if (value.notification->getCategory() == PhoneNotification::Category::INCOMING_CALL) {
                        std::tm timeNow = getSystemTime();
                        auto notification = std::make_shared<PhoneNotification>(
                            getTimeMs(),
                            PhoneNotification::Platform::IOS, static_cast<uint32_t>(timeNowUTC()),
                            PhoneNotification::Category::ACTIVE_CALL, timeNow,
                            "Joe Doe",
                            "",
                            "Active call",
                            "com.google.Gmail", "", "End Call");

                        sendToGui(B2GEvent::PhoneNotificationAdd {notification, true});

                        sendToGui(B2GEvent::PhoneNotificationRemove {value.notification->getUid(), false});
                    }
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppListReq>) {
                    B2GEvent::UserAppList rsp{};
                    rsp.type = value.type;
                    if (value.type == App::Type::kActivity) {
                        rsp.list = std::make_shared<std::vector<App::Item>>(mApps);
                    } else if (value.type == App::Type::kUtility) {
                        rsp.list = std::make_shared<std::vector<App::Item>>(mApps);
                    } else if (value.type == App::Type::kClockface) {
                        rsp.list = std::make_shared<std::vector<App::Item>>(mApps);
                    } else if (value.type == App::Type::kGlance) {
                        rsp.list = nullptr;
                    }
                    sendToGui(rsp);
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppIconWindow>) {
                    // Mirrors Backend::guiHandler: hold exactly the window
                    // asked for, reuse what is already in it, and let the rest
                    // go. Taking a fresh slot for every id on every request --
                    // which is the obvious thing to write here -- burns through
                    // the icon pool at a rate the watch never would, and makes
                    // the sim disagree with the firmware about when it runs out.
                    B2GEvent::UserAppIcons rsp{};
                    rsp.type  = value.type;
                    rsp.count = 0;

                    App::IconRef kept[App::kIconWindowMax];
                    uint8_t keptCount = 0;

                    for (uint8_t i = 0; i < value.count; ++i) {
                        const uint64_t appId = value.appIds[i];

                        App::IconRef found;
                        for (uint8_t h = 0; h < mIconWindowCount; ++h) {
                            if (mIconWindow[h] && mIconWindow[h]->appId == appId) {
                                found = mIconWindow[h];
                                break;
                            }
                        }

                        if (!found) {
                            for (const auto& set : mAppIcons) {
                                if (set.appId != appId) {
                                    continue;
                                }

                                found = App::IconPool::take();
                                if (found) {
                                    found.mutableSet() = set;
                                }
                                break;
                            }
                        }

                        if (!found) {
                            continue;
                        }

                        if (keptCount < App::kIconWindowMax) {
                            kept[keptCount++] = found;
                        }
                        if (rsp.count < B2GEvent::UserAppIcons::kMax) {
                            rsp.icons[rsp.count++] = found;
                        }
                    }

                    for (uint8_t i = 0; i < App::kIconWindowMax; ++i) {
                        mIconWindow[i] = (i < keptCount) ? kept[i] : App::IconRef();
                    }
                    mIconWindowCount = keptCount;

                    sendToGui(rsp);
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppStart>) {
                    LOG_DEBUG("Backend. UserAppStart [pid= 0x%08X]\n", value.pid);

                    // 'F' arms a reason: answer the way the watch does when a
                    // launch cannot start, so each sentence can be read on the
                    // screen that shows it. Without this the stub always
                    // succeeds, and the only failure the simulator could reach
                    // was the GUI's own timeout.
                    if (mForcedLaunchFailure != App::LaunchFailure::NONE) {
                        LOG_DEBUG("Backend. Forced launch failure %u\n",
                                static_cast<unsigned>(mForcedLaunchFailure));
                        sendToGui(B2GEvent::UserAppStartFailed{
                                value.pid, value.attempt, mForcedLaunchFailure });
                    } else if (auto item = findApp(value.pid)) {
                        sendToGui(B2GEvent::UserAppStartReq{ item->pidGUI,
                                                            value.attempt });

                        // Move it to the front, as App::SorterFreqUse does on
                        // the watch: it bumps the launched app's count and
                        // decays the rest, so a launch puts that app first in
                        // every list handed out afterwards. Without this the
                        // stub answered every request with the same order, so
                        // nothing exercised a menu holding its place across a
                        // re-sorted list -- and the tracking wheel restored the
                        // row rather than the app, landing one place off.
                        const auto pid = value.pid;
                        const auto it = std::find_if(mApps.begin(), mApps.end(),
                            [pid](const App::Item &a) { return a.pidGUI == pid; });
                        if (it != mApps.end() && it != mApps.begin()) {
                            std::rotate(mApps.begin(), it, it + 1);
                        }
                    }
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppResume>) {
                    LOG_DEBUG("Backend. UserAppResume [pid= 0x%08X]\n", value.pid);
                    // No confirmation, just resume app
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppPause>) {
                    LOG_DEBUG("Backend. UserAppPause [pid= 0x%08X]\n", value.pid);
                    // No confirmation, just pause app
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppStop>) {
                    LOG_DEBUG("Backend. UserAppStop [pid= 0x%08X]\n", value.pid);
                    // No confirmation, just stop app
                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppGlancesStart>) {
                    LOG_DEBUG("Backend. UserAppGlancesStart\n");
                    mGlanceCounter = 0;

                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppGlancesStop>) {
                    LOG_DEBUG("Backend. UserAppGlancesStop\n");

                } else if constexpr (std::is_same_v<T, G2BEvent::UserAppGlancesListReq>) {
                    LOG_DEBUG("Backend. UserAppGlancesListReq\n");

                    B2GEvent::UserAppGlanceList rsp{};

                    std::vector<App::GlanceData> v;
                    v.reserve(skGlancesCount);

                    if (mGlanceCounter > 0) {
                        for (uint32_t i = 0; i < skGlancesCount; ++i) {
                            mGlance[i].glanceUpdate();
                            v.push_back(mGlance[i].getData());
                            if (i == mGlanceCounter) {
                                break;
                            }
                        }
                    }
                    rsp.list = std::make_shared<std::vector<App::GlanceData>>(v);

                    sendToGui(rsp);

                    mGlanceCounter++;
                }
            }, data);
        }

    }

    // Keys
    virtual void handleKeyEvent(uint8_t c) override
    {
        LOG_DEBUG("%c\n", c);

        // Battery level
        if ('+' == c) {
            if (mBatteryLevel < 100) { 
                mBatteryLevel++;
                sendToGui(B2GEvent::BatteryLevel {static_cast<float>(mBatteryLevel)});
            }
        }
        if ('-' == c) {
            if (mBatteryLevel > 0) {
                mBatteryLevel--;
                sendToGui(B2GEvent::BatteryLevel {static_cast<float>(mBatteryLevel)});
            }
        }

        // BLE
        // p - pin code, s - success, f - fail
        if (!mBlePairingWait && !mBleCodeWait && 'p' == c) {
            sendToGui(B2GEvent::BlePinCode {123456});
            mBleCodeWait = true;
        } else if (mBlePairingWait) {
            mBlePairingWait = false;
            if ('s' == c) {
                sendToGui(B2GEvent::BleConnect {true});
                sendToGui(B2GEvent::BlePinCode {123456});
                mBleCodeWait = true;
            } else if ('f' == c) {
                sendToGui(B2GEvent::BleConnect {true});
                sendToGui(B2GEvent::BlePair {false});
                sendToGui(B2GEvent::BleConnect {false});
            }
        } else if (mBleCodeWait) {
            mBleCodeWait = false;
            if ('s' == c) {
                sendToGui(B2GEvent::BlePair {true});

                mBleBonded = true;
                sendToGui(B2GEvent::BleBond {mBleBonded});
            } else if ('f' == c) {
                sendToGui(B2GEvent::BlePair {false});
            }
        }


        // USB
        if ('U' == c) {
            mUsbConnected = !mUsbConnected;
            sendToGui(B2GEvent::UsbConnect {mUsbConnected});
        }
        
        if ('u' == c) {
            mUsbPortOpened = !mUsbPortOpened;
            sendToGui(B2GEvent::UsbPortOpen {mUsbPortOpened});
        }

        if ('N' == c) {
            std::tm timeNow = getSystemTime();
            auto notification = std::make_shared<PhoneNotification>(
                getTimeMs(),
                PhoneNotification::Platform::IOS,
                static_cast<uint32_t>(timeNowUTC()),
                PhoneNotification::Category::INCOMING_CALL, timeNow,
                "Joe Doe", 
                "",
                "Incoming call", 
                "com.google.Gmail", "Answer", "Decline");

            sendToGui(B2GEvent::PhoneNotificationAdd {notification, true});
        }

        if ('M' == c) {
            std::tm timeNow = getSystemTime();
            char timestr[20] {};
            sprintf_s(timestr, "%u", timeNow.tm_sec);

            auto notification = std::make_shared<PhoneNotification>(
                getTimeMs(),
                PhoneNotification::Platform::IOS,
                static_cast<uint32_t>(timeNowUTC()),
                PhoneNotification::Category::MESSAGE, timeNow,
                "Message",
                timestr,
                "Hey mate, how are you? We are meeting Marcus at the station pub at 10am tomorrow."
                "Will you join us ? \nWe can grab a bite as well! qwerty",
                "com.google.Gmail", "", "Clear");

            sendToGui(B2GEvent::PhoneNotificationAdd {notification, true});
        }

        if ('C' == c) {
            std::tm timeNow = getSystemTime();
            auto notification = std::make_shared<PhoneNotification>(
                getTimeMs(),
                PhoneNotification::Platform::IOS, static_cast<uint32_t>(timeNowUTC()),
                PhoneNotification::Category::ACTIVE_CALL, timeNow,
                "Joe Doe",
                "",
                "Active call",
                "com.google.Gmail", "", "End Call");

            sendToGui(B2GEvent::PhoneNotificationAdd {notification, true});
        }

        if ('K' == c) {
            sendToGui(B2GEvent::PhoneNotificationRemove {0, true});
        }

        if ('R' == c) {
            sendToGui(B2GEvent::UserAppStartReq{ mApps[0].pidGUI, 0 });
        }
        if ('r' == c) {
            sendToGui(B2GEvent::UserAppStartReq{ mApps[1].pidGUI, 0 });
        }
        if ('E' == c) {
            sendToGui(B2GEvent::UserAppStopped{ mApps[0].pidGUI });
        }
        if ('e' == c) {
            sendToGui(B2GEvent::UserAppStopped{ mApps[1].pidGUI });
        }

        // Charging refused by a protection rule - toggles the charging screen
        // out of the priority list while USB stays connected.
        if (';' == c) {
            mChargingBlocked = !mChargingBlocked;
            sendToGui(B2GEvent::ChargingBlocked {mChargingBlocked});
        }

        // Launch failure: each F press arms the next reason, so the next app
        // the user presses fails with it and the wording for every reason can
        // be read on the screen. Wraps back to NONE, which launches normally
        // again.
        if ('F' == c) {
            static constexpr App::LaunchFailure kReasons[] = {
                App::LaunchFailure::NONE,
                App::LaunchFailure::OUT_OF_MEMORY,
                App::LaunchFailure::BAD_IMAGE,
                App::LaunchFailure::TIMEOUT,
                App::LaunchFailure::NOT_FOUND,
                App::LaunchFailure::OTHER,
            };
            mForcedReason = (mForcedReason + 1)
                    % (sizeof(kReasons) / sizeof(kReasons[0]));
            mForcedLaunchFailure = kReasons[mForcedReason];
            LOG_DEBUG("Backend. Launch failure armed: %u\n",
                    static_cast<unsigned>(mForcedLaunchFailure));
        }

        // System error: each ' press raises the next SystemError code so the
        // error screen text can be checked for every code. Wraps at COUNT.
        if ('\'' == c) {
            mErrorCode = (mErrorCode + 1) % static_cast<uint32_t>(SystemError::COUNT);
            LOG_DEBUG("Backend. Error code %u\n", mErrorCode);
            sendToGui(B2GEvent::Error {static_cast<SystemError>(mErrorCode)});
        }

    }





    virtual bool receiveGuiEvent(B2GEvent::Data &data) override
    {
        if (!mGuiTxQueue.empty()) {
            data = mGuiTxQueue.front();
            mGuiTxQueue.pop();
            return true;
        }
        return false;
    }

    virtual bool sendEventToBackend(const G2BEvent::Data &data) override
    {
        mGuiRxQueue.emplace(data);
        return true;
    }

    uint32_t getTimeMs()
    { 
        return static_cast<uint32_t>(GetTickCount());
    }

private:
    static constexpr uint32_t skGlancesCount = 3;
    uint32_t mGlanceCounter = 0;

    Backend()
    {
        // Initial setup
        startTimeS = timeNowUTC();
        updTime();

        // Device info
        mDeviceInfo.name = "UNA Watch";
        mDeviceInfo.manufacturer = "UNA";
        mDeviceInfo.id = "654321";
        mDeviceInfo.serialNumber = "ABCDEFABCDEFABCDEFABCDEF";
        mDeviceInfo.swVersion = "1.0.23";
        mDeviceInfo.hwVersion = "N/A";
        mDeviceInfo.btVersion = "34.5.78";
        mDeviceInfo.fccId = "N/A";
        mDeviceInfo.ce = "N/A";

        sendToGui(B2GEvent::DevInfo {mDeviceInfo});

        // Battery
        sendToGui(B2GEvent::BatteryLevel {static_cast<float>(mBatteryLevel)});

        mCommonSettings.heartRateZones = {};

        sendToGui(B2GEvent::Settings {mCommonSettings});

        sendToGui(B2GEvent::SettingsLocal {mLocalSettings});

        // BLE
        sendToGui(B2GEvent::BleBond {mBleBonded});


        // APPs
        mAppIcons.resize(mApps.size());
        for (size_t i = 0; i < mApps.size(); ++i) {
            mApps[i].type = App::Type::kActivity;
            mApps[i].appId = skAppIdBase + i;

            // Deliberately not in menu order: these arrive most-used first, so
            // the pinning has to override the order it is given.
            static const struct { uint8_t idx; uint64_t id; const char *name; } kPinned[] = {
                { 3, 0xA19C2A7E4F8B6D31ULL, "Alarm"     },
                { 4, 0xA159B1C005CFD2A5ULL, "Stopwatch" },
                { 1, 0xA1D74A3F0CFF98D3ULL, "Timer"     },
            };

            const char *pinnedName = nullptr;
            for (const auto &p : kPinned) {
                if (p.idx == i) {
                    mApps[i].appId = p.id;
                    pinnedName = p.name;
                    break;
                }
            }
            mApps[i].pidService = 100 + i;
            mApps[i].pidGUI = 200 + i;
            if (pinnedName != nullptr) {
                snprintf(mApps[i].name, App::kNameLen, "%s", pinnedName);
            } else {
                snprintf(mApps[i].name, App::kNameLen, "App_%u", i);
            }

            // Kept out of the Item, as on the watch, and handed over only when
            // a menu asks for this app's window.
            //
            // Cycled rather than derived from the index: ABGR2222 opaque red,
            // green, blue, yellow, cyan, magenta, so neighbouring apps are
            // told apart at a glance however many there are. 0xF0 + i ran off
            // the end of the opaque range past sixteen apps, and anything
            // starting at 0xC0 makes the first app opaque black on black.
            static const uint8_t kShades[] = { 0xC3, 0xCC, 0xF0, 0xCF, 0xFC, 0xF3 };
            const uint8_t shade = kShades[i % (sizeof(kShades) / sizeof(kShades[0]))];

            mAppIcons[i].appId = mApps[i].appId;
            mAppIcons[i].icon.fill(shade);
            mAppIcons[i].smallIcon.fill(shade);
        }

        // Glances
        for (uint32_t i = 0; i < skGlancesCount; ++i) {
            char tmpName[App::kNameLen]{};
            snprintf(tmpName, App::kNameLen, "glance_%u", i);
            mGlance[i].createGlance(tmpName);
        }

        // Daily health
        sendToGui(B2GEvent::DailyHealth {mSteps, mFloors, mActivityMinutes, mRestingHeartRate, mAverageHeartRate});
        sendToGui(B2GEvent::HeartRate {mHeartRate});

        // Final
        sendToGui(B2GEvent::Booted {});
    }
    
    virtual ~Backend() = default;

    std::queue<G2BEvent::Data> mGuiRxQueue;
    std::queue<B2GEvent::Data> mGuiTxQueue;

    /// Written this tick, readable the next. Nothing goes straight onto
    /// mGuiTxQueue -- see sendToGui().
    std::queue<B2GEvent::Data> mPendingTx;

    /**
     * @brief Hand an event to the GUI, arriving no earlier than the next tick.
     *
     * On the watch the backend is its own FreeRTOS task, so a request the GUI
     * sends cannot be answered before its next frame. This stub runs inside the
     * GUI's tick, so replying straight onto mGuiTxQueue would land the answer
     * in the very frame that asked for it -- and anything whose correctness
     * depends on whether the answer has arrived yet then passes here and fails
     * on hardware. It has done, twice: the top menu's icon window served a
     * stale cache on open, and later opened the lower half of its arc a frame
     * late, both verified clean here first.
     *
     * So every event waits a tick. It is not the full picture -- the real
     * backend can take longer, and does when it reads an icon off the eMMC --
     * but it is the difference between "same frame" and "not the same frame",
     * which is what that class of bug turns on.
     */
    void sendToGui(const B2GEvent::Data &event)
    {
        mPendingTx.push(event);
    }

    // Time
    time_t startTimeS = 0;
    std::tm mTime;

    time_t timeNowUTC() { 
        return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }

    std::tm getSystemTime()
    {
        auto currentTime = std::chrono::system_clock::now();

        auto time = std::chrono::system_clock::to_time_t(currentTime);
        std::tm localTime;
        localtime_s(&localTime, &time);
        return localTime;
    }

    void updTime() { 
        std::tm timeNow = getSystemTime();
        if (timeNow.tm_hour != mTime.tm_hour || timeNow.tm_min != mTime.tm_min || timeNow.tm_sec != mTime.tm_sec) {
            mTime = timeNow;
            sendToGui(B2GEvent::Time {mTime});
        }
    }

    // Device info
    DeviceInfo mDeviceInfo {};

    // GPS Fix
    bool mIsGpsFix = false;
    void updGps() { 
        if (std::difftime(timeNowUTC(), startTimeS) > 10 && !mIsGpsFix) { 
            mIsGpsFix = true;
            sendToGui(B2GEvent::GpsFix {mIsGpsFix});
        }
    }

    // Magnetometer
    //
    // Enough of the part to drive the compass screen: a bearing only once a
    // calibration has been done, and a run that fills at a fixed rate.
    static constexpr uint32_t kMagFeedTicks     = 2;   // 200 ms at a 10 Hz tick
    static constexpr uint32_t kMagProgressTicks = 5;   // 500 ms

    bool     mMagFeed       = false;
    bool     mMagCalibrated = false;
    float    mMagHeading    = 0.0f;
    uint32_t mMagFeedTick   = 0;
    uint32_t mMagRunTicks   = 0;   // 0 when no run is going
    uint32_t mMagRunLeft    = 0;
    uint32_t mMagRunTick    = 0;

    void updMag()
    {
        if (mMagRunTicks > 0) {
            mMagRunLeft--;
            mMagRunTick++;

            if (mMagRunLeft == 0) {
                mMagRunTicks   = 0;
                mMagCalibrated = true;

                sendToGui(B2GEvent::MagCalProgress {100});
                sendToGui(B2GEvent::MagCalDone {true, true});
            } else if (mMagRunTick >= kMagProgressTicks) {
                mMagRunTick = 0;

                const uint32_t done = mMagRunTicks - mMagRunLeft;

                sendToGui(B2GEvent::MagCalProgress {
                        static_cast<uint8_t>((done * 100U) / mMagRunTicks)});
            }
        }

        if (!mMagFeed) {
            return;
        }

        mMagFeedTick++;

        if (mMagFeedTick < kMagFeedTicks) {
            return;
        }

        mMagFeedTick = 0;

        // Turning on its own, so the rose can be watched moving.
        mMagHeading += 3.0f;

        if (mMagHeading >= 360.0f) {
            mMagHeading -= 360.0f;
        }

        // Face up: the simulator has no tilt to model.
        sendToGui(B2GEvent::MagAzimuth {mMagHeading, mMagCalibrated, 0.0f});
    }

    // Battery
    uint8_t mBatteryLevel = 73;

    // Settings
    WatchSettings mCommonSettings {};
    WatchSettingsLocal mLocalSettings {};

    // APPs
    static constexpr uint64_t skAppIdBase = 0x1000;
    // Six, not two, and three of them carrying the uapp ids the top menu pins
    // to fixed places -- Alarm first, Stopwatch and Timer just after Settings.
    // With ids the order does not recognise, the stub never put an app and a
    // fixed entry next to each other, so nothing here exercised the interleave
    // and two bugs in it reached hardware: the restored position was thrown
    // away, and the alerts mute state was lost on every open.
    std::vector<App::Item> mApps { 6 };
    std::vector<App::IconSet> mAppIcons {};
    App::IconRef mIconWindow[App::kIconWindowMax];
    uint8_t      mIconWindowCount = 0;
    App::Item* findApp(uint32_t pid)
    {
        auto it = std::find_if(mApps.begin(), mApps.end(),
            [&](const App::Item& ctx) {
                return ctx.pidGUI == pid;
            });
        if (it != mApps.end()) {
            return &*it;
        }
        return nullptr;
    }


    // BLE
    bool mBleBonded = true;
    bool mBlePairingWait = false;
    bool mBleCodeWait = false;

    // USB
    bool mUsbConnected = false;
    bool mUsbPortOpened = false;

    // System error (test): current code cycled by the ' key
    uint32_t mErrorCode = 0;
    bool     mChargingBlocked = false;

    /// Reason the next launch is answered with, armed by 'F'. NONE launches
    /// normally, which is where it starts.
    App::LaunchFailure mForcedLaunchFailure = App::LaunchFailure::NONE;
    size_t             mForcedReason = 0;


    // Daily health
    uint32_t mSteps = 5000;
    uint32_t mFloors = 50;
    uint32_t mActivityMinutes = 70;
    uint32_t mRestingHeartRate = 67;
    uint32_t mAverageHeartRate = 85;
    float mHeartRate = 83.1f;




    /* Source: Clock24.png | 24x24 | 576 bytes ABGR2222 */
#define CLOCK24_WIDTH  24
#define CLOCK24_HEIGHT 24
#define CLOCK24_SIZE   576
    static constexpr uint8_t CLOCK24_ABGR2222[576] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x2A, 0x2A, 0x00, 0x00, 0x80, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x40, 0x15, 0x6A, 0x2A,
        0x00, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x95, 0xAA, 0x2A, 0x40, 0xC1, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xEA, 0x2A,
        0x80, 0xC0, 0xC0, 0xC0, 0xC0, 0xD5, 0xD5, 0xD5, 0xD6, 0xD6, 0xD5, 0xD5, 0xD6, 0xDA, 0xD6, 0xD5,
        0xD6, 0xC1, 0xC0, 0xC0, 0xC0, 0xC0, 0xD5, 0x2A, 0x80, 0xC0, 0xC0, 0xC0, 0xEA, 0xFF, 0xFF, 0xD5,
        0xFF, 0xFF, 0xFF, 0xD5, 0xFF, 0xFF, 0xFF, 0xEA, 0xFF, 0xFF, 0xD5, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xD5, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xD5, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xEA, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0xD5, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xEA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xD5, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,
        0xEA, 0xD5, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xD5, 0xEA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xD5, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xEA, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEA, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A,
        0x80, 0xC0, 0xC0, 0xC0, 0xD5, 0xFF, 0xFF, 0xD5, 0xFF, 0xFF, 0xFF, 0xD5, 0xFF, 0xFF, 0xFF, 0xEA,
        0xFF, 0xFF, 0xD5, 0xC0, 0xC0, 0xC0, 0xD5, 0x6A, 0x80, 0xC0, 0xC0, 0xC0, 0xC0, 0xC5, 0xD5, 0xD5,
        0xD5, 0xD5, 0xD5, 0xC0, 0xD5, 0xD5, 0xD5, 0xD5, 0xD5, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xD6, 0x6A,
        0x40, 0xC1, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xEA, 0x2A, 0x00, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0x55, 0xAA, 0x2A,
        0x00, 0x00, 0x80, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0,
        0xC0, 0xC0, 0xC0, 0xC0, 0x40, 0x16, 0x6A, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x2A, 0x2A
    };



    class GlanceProvider {
    public:
        GlanceProvider(){}

        App::GlanceData getData() {
            App::GlanceData d{};
            d.count = static_cast<uint8_t>(mUI.size());
            for (uint8_t i = 0; i < d.count; i++) {
                d.ctrls[i] = mUI.data()[i];
            }
            snprintf(d.altname, sizeof(d.altname), "%s", mAltName);

            return d;
        }
        
        void glanceUpdate()
        {
            mTemperature += 0.1f;
            if (mTemperature > 25) {
                mTemperature = 23.5f;
            }

            mTextTemperature.print("%.1f", mTemperature);

            std::time_t t = time(NULL);
            struct tm tm;
            localtime_s(&tm, &t);

            mTextTime.print("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        }


        void createGlance(const char* name)
        {
            GlanceProvider& data = *this;
            snprintf(data.mAltName, sizeof(data.mAltName), "%s", name);

            // first row
            data.mUI.createRect().init(
                { 50,   0 },
                { 140, 30 },
                GlanceColor_t::GLANCE_COLOR_BLUE,
                GlanceColor_t::GLANCE_COLOR_BLACK);

            SDK::Glance::ControlRectangle panelClock = data.mUI.createRect().init({ 50,  0 },
                { 30, 30 },
                GlanceColor_t::GLANCE_COLOR_BLUE,
                GlanceColor_t::GLANCE_COLOR_BLACK);

            GlanceSize_t  clockImageSize = { CLOCK24_WIDTH, CLOCK24_HEIGHT };
            GlancePoint_t clockImagePos = SDK::Glance::Align::placeWithin(panelClock.pos(),
                panelClock.size(),
                clockImageSize,
                GlanceAlignH_t::GLANCE_ALIGN_H_CENTER,
                GlanceAlignV_t::GLANCE_ALIGN_V_CENTER);

            data.mUI.createImage().init(clockImagePos, clockImageSize, CLOCK24_ABGR2222);

            data.mTextTime = data.mUI.createText();
            data.mTextTime.pos({ 80, 2 }, { 110, 26 })
                .color(GlanceColor_t::GLANCE_COLOR_YELLOW_DARK)
                .setText("00:00:00")
                .alignment(GlanceAlignH_t::GLANCE_ALIGN_H_CENTER);

            // second row
            data.mUI.createRect().init({ 50,  30 }, { 95, 30 },
                GlanceColor_t::GLANCE_COLOR_BLUE,
                GlanceColor_t::GLANCE_COLOR_RED);

            data.mUI.createText().init({ 50, 32 }, { 90, 26 },
                "Temp, C:",
                GlanceFont_t::GLANCE_FONT_POPPINS_REGULAR_18,
                GlanceColor_t::GLANCE_COLOR_WHITE,
                GlanceAlignH_t::GLANCE_ALIGN_H_RIGHT);


            data.mUI.createRect().init({ 145, 30 }, { 45, 30 },
                GlanceColor_t::GLANCE_COLOR_BLUE,
                GlanceColor_t::GLANCE_COLOR_GREEN);

            data.mTextTemperature = data.mUI.createText();
            data.mTextTemperature.pos({ 150, 32 }, { 40, 26 })
                .color(GlanceColor_t::GLANCE_COLOR_WHITE)
                .alignment(GlanceAlignH_t::GLANCE_ALIGN_H_CENTER)
                .setText("23.5");
        }


    private:
        char mAltName[App::kNameLen]{};
        SDK::Glance::Form mUI{ 240, 60 };
        SDK::Glance::ControlText mTextTime;
        SDK::Glance::ControlText mTextTemperature;
        float                    mTemperature;
    };

    GlanceProvider mGlance[skGlancesCount];
};

#endif // BACKEND_HPP
