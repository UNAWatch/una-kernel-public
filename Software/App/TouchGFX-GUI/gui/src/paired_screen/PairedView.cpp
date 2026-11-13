#include <gui/paired_screen/PairedView.hpp>

PairedView::PairedView()
{

}

void PairedView::setupScreen()
{
    PairedViewBase::setupScreen();
}

void PairedView::tearDownScreen()
{
    PairedViewBase::tearDownScreen();
}

void PairedView::showReconnected()
{
    Unicode::snprintf(textBuffer, TEXT_SIZE, "%s", touchgfx::TypedText(T_TEXT_RECONNECTED).getText());
    text.resizeToCurrentText();
    text.setX((240 - text.getWidth()) / 2);
    text.invalidate();
}

void PairedView::handleTickEvent()
{
    if (mCounter > 0) {
        mCounter--;
    }
    if (mCounter == 0) {
        presenter->exit();
    }
}