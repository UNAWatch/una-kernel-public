#ifndef SETTINGSDEVICEPRESENTER_HPP
#define SETTINGSDEVICEPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class SettingsDeviceView;

class SettingsDevicePresenter : public touchgfx::Presenter, public ModelListener
{
public:
    SettingsDevicePresenter(SettingsDeviceView& v);

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

    virtual ~SettingsDevicePresenter() {}

    void saveDeveloperMode(bool on);

    virtual void onIdleTimeout() override { model->application().gotoHomeScreenNoTransition(); }

private:
    SettingsDevicePresenter();

    SettingsDeviceView& view;
};

#endif // SETTINGSDEVICEPRESENTER_HPP
