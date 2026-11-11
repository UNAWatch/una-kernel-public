#ifndef CHARGINGPRESENTER_HPP
#define CHARGINGPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class ChargingView;

class ChargingPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    ChargingPresenter(ChargingView& v);

    /**
     * The activate function is called automatically when this screen is "switched in"
     * (ie. made active). Initialization logic can be placed here.
     */
    virtual void activate();

    /**
     * The deactivate function is called automatically when this screen is "switched out"
     * (ie. made inactive). Teardown functionality can be placed here.
     */
    virtual void deactivate();

    virtual ~ChargingPresenter() {}

    virtual void onUsbDisconnect() override;
    virtual void onUsbState(bool portOpen, bool charging) override;
    virtual void onLowBattery() override {}  // Not called in this screen
    virtual void onBatteryLevel(uint8_t lvl) override;
    virtual void onTime(uint8_t h, uint8_t m, uint8_t s) override;
    virtual void onDate(uint8_t m, uint8_t d, uint8_t wd) override;
    virtual void onLocalSettings(const WatchSettingsLocal &sett) override;
    virtual void onAppStartReq(uint32_t pid) override {}  // Not called in this screen
    virtual void onAppLaunchFailed() override {}  // Not shown over this screen

    /// Back: leave the screen, and keep it away until the cable is replugged.
    /// Does nothing while mass storage is open.
    void exit();

private:
    ChargingPresenter();

    ChargingView& view;
};

#endif // CHARGINGPRESENTER_HPP
