#ifndef PAIREDVIEW_HPP
#define PAIREDVIEW_HPP

#include <gui_generated/paired_screen/PairedViewBase.hpp>
#include <gui/paired_screen/PairedPresenter.hpp>

class PairedView : public PairedViewBase
{
public:
    PairedView();
    virtual ~PairedView() {}
    virtual void setupScreen();
    virtual void tearDownScreen();
    void showReconnected();
protected:
    uint16_t mCounter = Gui::Config::kPairedScreenTimeout;
    virtual void handleTickEvent() override;
};

#endif // PAIREDVIEW_HPP
