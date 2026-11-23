#include <gui/settingsdevice_screen/SettingsDeviceView.hpp>
#include <gui/settingsdevice_screen/SettingsDevicePresenter.hpp>

SettingsDevicePresenter::SettingsDevicePresenter(SettingsDeviceView& v)
    : view(v)
{

}

void SettingsDevicePresenter::activate()
{
    // Reset idle timer
    model->resetIdleTimer();

    view.setInfo(model->getDeviceInfo());
    view.setDeveloperMode(model->getLocalSettings().developerMode);
}

void SettingsDevicePresenter::saveDeveloperMode(bool on)
{
    WatchSettingsLocal sett = model->getLocalSettings();
    sett.developerMode = on;
    model->saveLocalSettings(sett);
}

void SettingsDevicePresenter::deactivate()
{

}
