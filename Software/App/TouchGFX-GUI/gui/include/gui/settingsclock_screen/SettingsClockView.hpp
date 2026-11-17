#ifndef SETTINGSCLOCKVIEW_HPP
#define SETTINGSCLOCKVIEW_HPP

#include <gui_generated/settingsclock_screen/SettingsClockViewBase.hpp>
#include <gui/settingsclock_screen/SettingsClockPresenter.hpp>

class SettingsClockView : public SettingsClockViewBase
{
public:
    SettingsClockView();
    virtual ~SettingsClockView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();

    void setPositionId(uint16_t id);
    uint16_t getPositionId();

    void setFormat(WatchSettingsLocal::Clock::Format format);
    void setDateOrder(WatchSettingsLocal::Clock::DateOrder order);
    void setTimeZone(WatchSettingsLocal::Clock::TimeZone tz);

protected:
    WatchSettingsLocal::Clock::Format mFormat {};
    WatchSettingsLocal::Clock::DateOrder mDateOrder {};
    WatchSettingsLocal::Clock::TimeZone mTimeZone {};
    touchgfx::Unicode::UnicodeChar mTzTipBuf[20] {};

    virtual void handleKeyEvent(uint8_t key) override;
};

#endif // SETTINGSCLOCKVIEW_HPP
