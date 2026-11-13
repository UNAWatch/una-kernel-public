#ifndef SETTINGSPHONE2PRESENTER_HPP
#define SETTINGSPHONE2PRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class SettingsPhone2View;

class SettingsPhone2Presenter : public touchgfx::Presenter, public ModelListener
{
public:
    SettingsPhone2Presenter(SettingsPhone2View& v);

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

    virtual ~SettingsPhone2Presenter() {}

    virtual void onIdleTimeout() override { model->application().gotoHomeScreenNoTransition(); }

    virtual void onBleBond(bool paired) override;
    virtual void onBleConnect(bool connected) override;

    void unpair();
    void startPairing();

private:
    SettingsPhone2Presenter();

    SettingsPhone2View& view;
};

#endif // SETTINGSPHONE2PRESENTER_HPP
