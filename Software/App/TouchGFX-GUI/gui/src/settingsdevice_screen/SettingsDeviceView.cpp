#include <gui/settingsdevice_screen/SettingsDeviceView.hpp>

SettingsDeviceView::SettingsDeviceView()
{

}

void SettingsDeviceView::setupScreen()
{
    SettingsDeviceViewBase::setupScreen();

    menu.setTitle(T_TEXT_DEVICE_UC);

    menu.setNumberOfItems(Gui::Config::Menu::Top::Settings::Device::ID_COUNT);
    update();
}

void SettingsDeviceView::tearDownScreen()
{
    SettingsDeviceViewBase::tearDownScreen();
}

void SettingsDeviceView::setInfo(const DeviceInfo &devInfo)
{
    mDevInfo = devInfo;
    update();
}

void SettingsDeviceView::setDeveloperMode(bool on)
{
    mDeveloperMode = on;
    update();
}

void SettingsDeviceView::handleKeyEvent(uint8_t key)
{
    if (key == Gui::Config::Button::L1) {
        menu.selectPrev(); 
    }

    if (key == Gui::Config::Button::L2) {
        menu.selectNext();
    }

    if (key == Gui::Config::Button::R1) {
        if (menu.getSelectedItem() == Gui::Config::Menu::Top::Settings::Device::ID_DEVELOPER_MODE) {
            setDeveloperMode(!mDeveloperMode);
            presenter->saveDeveloperMode(mDeveloperMode);
        }
    }

    if (key == Gui::Config::Button::R2) {
        application().gotoSettingsScreenNoTransition();
    }
}

void SettingsDeviceView::update()
{
    MenuDeviceItem *pItem = nullptr;

    const uint16_t kBuffSize = 32;
    char buffer[kBuffSize] { };

    // DEVICE_ID
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_DEVICE_ID);
    pItem->setTitle(T_TEXT_DEVICE_ID);
    pItem->setValue(mDevInfo.id);

    // SERIAL_NUM
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_SERIAL_NUM);
    pItem->setTitle(T_TEXT_SERIAL_NUMBER);

    // split serial number into two lines
    memcpy(buffer, mDevInfo.serialNumber, strlen(mDevInfo.serialNumber)/2);

    buffer[strlen(mDevInfo.serialNumber) / 2] = '\n';

    memcpy(&buffer[strlen(mDevInfo.serialNumber) / 2 + 1], 
        &mDevInfo.serialNumber[strlen(mDevInfo.serialNumber) / 2], 
        strlen(mDevInfo.serialNumber) - (strlen(mDevInfo.serialNumber) / 2));

    pItem->setValue(buffer);
    
    // SOFTWARE_VER
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_SOFTWARE_VER);
    pItem->setTitle(T_TEXT_SOFTWARE);
    pItem->setValue(mDevInfo.swVersion);

    // HARDWARE_VER
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_HARDWARE_VER);
    pItem->setTitle(T_TEXT_HARDWARE);
    pItem->setValue(mDevInfo.hwVersion);

    // BLUETOOTH
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_BLUETOOTH);
    pItem->setTitle(T_TEXT_BLUETOOTH);
    pItem->setValue(mDevInfo.btVersion);

    // FCC_ID
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_FCC_ID);
    pItem->setTitle(T_TEXT_FCC_ID_UC);
    pItem->setValue(mDevInfo.fccId);

    // CE
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_CE);
    pItem->setTitle(T_TEXT_CE_UC);
    pItem->setValue(mDevInfo.ce);

    // DEVELOPER_MODE: R1 switches it
    pItem = menu.getItem(Gui::Config::Menu::Top::Settings::Device::ID_DEVELOPER_MODE);
    pItem->setTitle(T_TEXT_DEVELOPER);
    pItem->setValue(mDeveloperMode ? "On" : "Off");

    menu.invalidate();
}
