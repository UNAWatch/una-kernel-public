#include <gui/settingsclockformat_screen/SettingsClockFormatView.hpp>

SettingsClockFormatView::SettingsClockFormatView()
{

}

void SettingsClockFormatView::setupScreen()
{
    SettingsClockFormatViewBase::setupScreen();

    menu.setTitle(T_TEXT_FORMAT_UC);

    // Both modes have exactly two options; configure() fills the labels. The
    // shared item count relies on the two enums staying the same size.
    static_assert(static_cast<int>(WatchSettingsLocal::Clock::Format::FMT_COUNT)
                      == static_cast<int>(WatchSettingsLocal::Clock::DateOrder::DATE_ORDER_COUNT),
                  "Time-format and date-order pickers must have the same item count");
    menu.setNumberOfItems(WatchSettingsLocal::Clock::Format::FMT_COUNT);
}

void SettingsClockFormatView::tearDownScreen()
{
    SettingsClockFormatViewBase::tearDownScreen();
}

void SettingsClockFormatView::configure(bool dateMode)
{
    mDateMode = dateMode;

    // Index 0 = FMT_24H / DATE_DMY, index 1 = FMT_12H / DATE_MDY.
    const TypedTextId item0 = dateMode ? T_TEXT_DD_MM : T_TEXT_24_HR;
    const TypedTextId item1 = dateMode ? T_TEXT_MM_DD : T_TEXT_12_HR;

    MenuItemSelected *pS = nullptr;
    MenuItemNotSelected *pN = nullptr;

    pS = menu.getSelectedItem(0);
    pS->config(item0);
    pN = menu.getNotSelectedItem(0);
    pN->config(item0);

    pS = menu.getSelectedItem(1);
    pS->config(item1);
    pN = menu.getNotSelectedItem(1);
    pN->config(item1);

    menu.invalidate();
}

void SettingsClockFormatView::setSelection(int16_t index)
{
    menu.selectItem(index);
}

void SettingsClockFormatView::handleKeyEvent(uint8_t key)
{
    if (key == Gui::Config::Button::L1) {
        menu.selectPrev();
    }

    if (key == Gui::Config::Button::L2) {
        menu.selectNext();
    }

    if (key == Gui::Config::Button::R1) {
        uint32_t id = menu.getSelectedItem();
        if (mDateMode) {
            presenter->saveDateOrder(static_cast<WatchSettingsLocal::Clock::DateOrder>(id));
        } else {
            presenter->saveFormat(static_cast<WatchSettingsLocal::Clock::Format>(id));
        }
        application().gotoSettingsClockScreenNoTransition();
    }

    if (key == Gui::Config::Button::R2) {
        application().gotoSettingsClockScreenNoTransition();
    }
}
