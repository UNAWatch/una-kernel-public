#include <gui/settingsgpsassist_screen/SettingsGpsAssistView.hpp>

SettingsGpsAssistView::SettingsGpsAssistView()
{

}

void SettingsGpsAssistView::setupScreen()
{
    SettingsGpsAssistViewBase::setupScreen();

    title.set(T_TEXT_GPS_ASSIST_DOT);
}

void SettingsGpsAssistView::tearDownScreen()
{
    SettingsGpsAssistViewBase::tearDownScreen();
}

void SettingsGpsAssistView::setEpoTimeH(uint32_t h)
{
    uint32_t maxH = 7 * 24;
    uint32_t days = h / 24;
    uint32_t hours = h % 24;
    char msg[150];

    snprintf(msg, sizeof(msg), 
        "GPS Satellite\n"
        "data will expire in\n"
        "%lu day%s and %lu hour%s.\n"
        "Sync with the UNA\n"
        "watch app to\n"
        "refresh data", static_cast<unsigned long>(days), days != 1 ? "s" : "",
        static_cast<unsigned long>(hours), hours != 1 ? "s" : "");

    Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(msg), textBuffer, TEXT_SIZE);
    text.invalidate();

    const float kEnd = 305.0f;
    const float kStart = 55.0f;

    float pct = 100.0f * h / maxH;

    if (pct > 100.0f) {
        pct = 100.0f;
    }

    if (pct < 0.0f) {
        pct = 0.0f;
    }

    float end = kEnd;
    float start = kStart + (kEnd - kStart) * (1.0f - pct / 100.0f);
    progress.setArc<float>(start, end);
    progress.invalidate();
    background.invalidate();
}

void SettingsGpsAssistView::setEpoSync()
{
    setEpoTimeH(0); // Just draw screen

    char msg[50];

    // Overwrite message
    snprintf(msg, sizeof(msg),
        "\n"
        "\n"
        "GPS assistance\n"
        "data updating");

    Unicode::fromUTF8(reinterpret_cast<const uint8_t*>(msg), textBuffer, TEXT_SIZE);
    text.invalidate();
}

void SettingsGpsAssistView::handleKeyEvent(uint8_t key)
{
    if (key == Gui::Config::Button::L1) {
    }

    if (key == Gui::Config::Button::L2) {
    }

    if (key == Gui::Config::Button::R1) {
    }

    if (key == Gui::Config::Button::R2) {
        application().gotoSettingsGNSSScreenNoTransition();
    }
}
