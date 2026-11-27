#include <gui/settingsclock_screen/SettingsClockView.hpp>

SettingsClockView::SettingsClockView()
{

}

namespace {
const char* tzPresetLabel(WatchSettingsLocal::Clock::TimeZone tz)
{
    using TZ = WatchSettingsLocal::Clock::TimeZone;
    switch (tz) {
        case TZ::TZ_CET:        return "CET DST";
        case TZ::TZ_UK:         return "UK DST";
        case TZ::TZ_EET:        return "EET DST";
        case TZ::TZ_UTC:        return "UTC";
        case TZ::TZ_UTC_PLUS_1: return "UTC+1";
        case TZ::TZ_UTC_PLUS_2: return "UTC+2";
        case TZ::TZ_EST:        return "EST DST";
        case TZ::TZ_PST:        return "PST DST";
        case TZ::TZ_AUTO_BLE:
        default:                return "AUTO";
    }
}
} // namespace

void SettingsClockView::setTimeZone(WatchSettingsLocal::Clock::TimeZone tz)
{
    mTimeZone = tz;

    const size_t maxLen = sizeof(mTzTipBuf) / sizeof(mTzTipBuf[0]);
    touchgfx::Unicode::strncpy(mTzTipBuf, tzPresetLabel(mTimeZone), maxLen - 1);
    mTzTipBuf[maxLen - 1] = 0;

    MenuItemSelected *pS = menu.getSelectedItem(Gui::Config::Menu::Top::Settings::Clock::ID_TIMEZONE);
    MenuItemNotSelected *pN = menu.getNotSelectedItem(Gui::Config::Menu::Top::Settings::Clock::ID_TIMEZONE);

    pS->configTip(T_TEXT_TIME, mTzTipBuf);
    pN->configTip(T_TEXT_TIME, mTzTipBuf);

    menu.invalidate();
}

void SettingsClockView::setupScreen()
{
    SettingsClockViewBase::setupScreen();

    menu.setTitle(T_TEXT_CLOCK_UP);

    menu.setNumberOfItems(Gui::Config::Menu::Top::Settings::Clock::ID_COUNT);

    // SECONDS is temporarily hidden - see GuiConfig.hpp.

    // Each entry carries its current value as a tip, so both of its items are
    // configured by setFormat() / setDateOrder().
    setFormat(mFormat);
    setDateOrder(mDateOrder);
    setTimeZone(mTimeZone);
}

void SettingsClockView::tearDownScreen()
{
    SettingsClockViewBase::tearDownScreen();
}

void SettingsClockView::setPositionId(uint16_t id)
{
    menu.selectItem(id);
}

uint16_t SettingsClockView::getPositionId()
{
    return menu.getSelectedItem();
}

void SettingsClockView::setFormat(WatchSettingsLocal::Clock::Format format)
{
    mFormat = format;

    const TypedTextId tipId = (mFormat == WatchSettingsLocal::Clock::FMT_12H)
            ? T_TEXT_12_HR : T_TEXT_24_HR;

    MenuItemSelected *pS = nullptr;
    MenuItemNotSelected *pN = nullptr;
    pS = menu.getSelectedItem(Gui::Config::Menu::Top::Settings::Clock::ID_TIME_FORMAT);
    pN = menu.getNotSelectedItem(Gui::Config::Menu::Top::Settings::Clock::ID_TIME_FORMAT);

    pS->configTip(T_TEXT_TIME_FORMAT, tipId);
    pN->configTip(T_TEXT_TIME_FORMAT, tipId);

    menu.invalidate();
}

void SettingsClockView::setDateOrder(WatchSettingsLocal::Clock::DateOrder order)
{
    mDateOrder = order;

    const TypedTextId tipId = (mDateOrder == WatchSettingsLocal::Clock::DATE_MDY)
            ? T_TEXT_MM_DD : T_TEXT_DD_MM;

    MenuItemSelected *pS = nullptr;
    MenuItemNotSelected *pN = nullptr;
    pS = menu.getSelectedItem(Gui::Config::Menu::Top::Settings::Clock::ID_DATE_FORMAT);
    pN = menu.getNotSelectedItem(Gui::Config::Menu::Top::Settings::Clock::ID_DATE_FORMAT);

    pS->configTip(T_TEXT_DATE_FORMAT, tipId);
    pN->configTip(T_TEXT_DATE_FORMAT, tipId);

    menu.invalidate();
}

void SettingsClockView::handleKeyEvent(uint8_t key)
{
    if (key == Gui::Config::Button::L1) {
        menu.selectPrev();
    }

    if (key == Gui::Config::Button::L2) {
        menu.selectNext();
    }

    if (key == Gui::Config::Button::R1) {
        uint32_t id = menu.getSelectedItem();

        switch (id) {
            // ID_SECONDS is temporarily hidden - see GuiConfig.hpp.
            case Gui::Config::Menu::Top::Settings::Clock::ID_TIME_FORMAT:
                presenter->setClockPickerDate(false);
                application().gotoSettingsClockFormatScreenNoTransition();
                break;
            case Gui::Config::Menu::Top::Settings::Clock::ID_DATE_FORMAT:
                presenter->setClockPickerDate(true);
                application().gotoSettingsClockFormatScreenNoTransition();
                break;
            
            /* each press cycles to the next timezone preset, 
            updates the row immediately, and persists it*/

            case Gui::Config::Menu::Top::Settings::Clock::ID_TIMEZONE:
            {
                using TZ = WatchSettingsLocal::Clock::TimeZone;
                const auto next = static_cast<TZ>((static_cast<uint8_t>(mTimeZone) + 1) % TZ::TZ_COUNT);
                setTimeZone(next);
                presenter->saveTimeZone(next);
                break;
            }
            default:
                break;
        };
    }

    if (key == Gui::Config::Button::R2) {
        application().gotoSettingsScreenNoTransition();
    }
}
