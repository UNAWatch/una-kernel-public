#ifndef CHARGINGVIEW_HPP
#define CHARGINGVIEW_HPP

#include <gui_generated/charging_screen/ChargingViewBase.hpp>
#include <gui/charging_screen/ChargingPresenter.hpp>

class ChargingView : public ChargingViewBase
{
public:
    ChargingView();
    virtual ~ChargingView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    void setBatteryLevel(uint8_t level);
    void setTime(uint8_t h, uint8_t m, uint8_t s);
    void setDate(uint8_t m, uint8_t d, uint8_t wd);
    void setClockSettings(const WatchSettingsLocal::Clock &settings);

protected:
    virtual void handleKeyEvent(uint8_t key) override;

private:
    uint8_t mHour   = 0xFF;
    uint8_t mMinute = 0xFF;
};

#endif // CHARGINGVIEW_HPP
