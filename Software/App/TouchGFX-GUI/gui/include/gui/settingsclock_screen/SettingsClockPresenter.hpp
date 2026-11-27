#ifndef SETTINGSCLOCKPRESENTER_HPP
#define SETTINGSCLOCKPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class SettingsClockView;

class SettingsClockPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    SettingsClockPresenter(SettingsClockView& v);

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

    virtual ~SettingsClockPresenter() {}

    virtual void onIdleTimeout() override { model->application().gotoHomeScreenNoTransition(); }

    /** @brief Choose which value the shared FORMAT picker edits (time vs date). */
    void setClockPickerDate(bool date) { model->setClockPickerDate(date); }

    /** @brief Persist the selected timezone/DST preset. */
    void saveTimeZone(WatchSettingsLocal::Clock::TimeZone tz);

private:
    SettingsClockPresenter();

    SettingsClockView& view;
};

#endif // SETTINGSCLOCKPRESENTER_HPP
