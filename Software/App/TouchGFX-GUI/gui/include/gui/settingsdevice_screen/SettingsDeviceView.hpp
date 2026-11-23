#ifndef SETTINGSDEVICEVIEW_HPP
#define SETTINGSDEVICEVIEW_HPP

#include <gui_generated/settingsdevice_screen/SettingsDeviceViewBase.hpp>
#include <gui/settingsdevice_screen/SettingsDevicePresenter.hpp>

class SettingsDeviceView : public SettingsDeviceViewBase
{
public:
    SettingsDeviceView();
    virtual ~SettingsDeviceView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    void setInfo(const DeviceInfo &devInfo);
    void setDeveloperMode(bool on);
protected:
    DeviceInfo mDevInfo {};
    bool mDeveloperMode {};
    virtual void handleKeyEvent(uint8_t key) override;
    void update();
};

#endif // SETTINGSDEVICEVIEW_HPP
