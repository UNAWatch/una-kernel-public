#include <gui/charging_screen/ChargingView.hpp>
#include <gui/charging_screen/ChargingPresenter.hpp>

ChargingPresenter::ChargingPresenter(ChargingView& v)
    : view(v)
{

}

void ChargingPresenter::activate()
{
    view.setBatteryLevel(model->batteryGetLevel());

    // The home face's clock, set up the same way as in HomePresenter.
    view.setClockSettings(model->getLocalSettings().clock);

    uint8_t month;
    uint8_t day;
    uint8_t wday;
    model->getDate(month, day, wday);
    view.setDate(month, day, wday);

    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    model->getTime(hour, minute, second);
    view.setTime(hour, minute, second);
}

void ChargingPresenter::onTime(uint8_t h, uint8_t m, uint8_t s)
{
    view.setTime(h, m, s);
}

void ChargingPresenter::onDate(uint8_t m, uint8_t d, uint8_t wd)
{
    view.setDate(m, d, wd);
}

void ChargingPresenter::onLocalSettings(const WatchSettingsLocal &sett)
{
    view.setClockSettings(sett.clock);
}

void ChargingPresenter::deactivate()
{

}

void ChargingPresenter::onUsbDisconnect()
{
    model->switchToNextPriorityScreen();
}

void ChargingPresenter::onUsbState(bool portOpen, bool charging)
{ 
    if (portOpen) {
        model->application().gotoUSBScreenNoTransition();
    }
}

void ChargingPresenter::onBatteryLevel(uint8_t lvl)
{
    view.setBatteryLevel(lvl);
}

void ChargingPresenter::exit()
{
    if (model->chargingDismiss()) {
        model->switchToNextPriorityScreen();
    }
}
