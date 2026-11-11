#include <gui/charging_screen/ChargingView.hpp>

ChargingView::ChargingView()
{

}

void ChargingView::setupScreen()
{
    ChargingViewBase::setupScreen();
    progress.setColor(Gui::Config::Color::TEAL);
}

void ChargingView::tearDownScreen()
{
    ChargingViewBase::tearDownScreen();
}

void ChargingView::handleKeyEvent(uint8_t key)
{
    if (key == Gui::Config::Button::R2) {
        presenter->exit();
    }
}

void ChargingView::setBatteryLevel(uint8_t level)
{
    if (level < 100) {
        Unicode::snprintf(textBuffer, TEXT_SIZE, "%s", touchgfx::TypedText(T_TEXT_CHARGING).getText());
    } else { 
        Unicode::snprintf(textBuffer, TEXT_SIZE, "%s", touchgfx::TypedText(T_TEXT_CHARGED).getText());
    }
    text.invalidate();

    Unicode::snprintf(valueBuffer, VALUE_SIZE, "%d%s", level, touchgfx::TypedText(T_TEXT_PERCENT).getText());
    value.invalidate();

    progress.setProgress(static_cast<float>(level));
}

void ChargingView::setTime(uint8_t h, uint8_t m, uint8_t s)
{
    // The time comes every second but the clock shows no seconds: redraw it
    // only when the minute changes.
    if (h == mHour && m == mMinute) {
        return;
    }
    mHour   = h;
    mMinute = m;
    clock.setTime(h, m, s);
}

void ChargingView::setDate(uint8_t m, uint8_t d, uint8_t wd)
{
    clock.setDate(m, d, wd);
}

void ChargingView::setClockSettings(const WatchSettingsLocal::Clock &settings)
{
    // As on the home screen (WatchFace1).
    clock.setFormat(settings.format);
    clock.setSeconds(settings.seconds);
    clock.setDateOrder(settings.dateOrder);
}