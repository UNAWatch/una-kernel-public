#include <gui/pairing_screen/PairingView.hpp>
#include <gui/pairing_screen/PairingPresenter.hpp>

PairingPresenter::PairingPresenter(PairingView& v)
    : view(v)
{
}

void PairingPresenter::activate()
{
    view.setDeviceID(model->getDeviceInfo().id);

    // Reset idle timer
    model->resetIdleTimer();
}

void PairingPresenter::deactivate()
{
}

void PairingPresenter::onBlePinCode() 
{
    model->application().gotoPairCodeScreenNoTransition(); 
}

void PairingPresenter::onBleConnect(bool connected)
{
    if (connected == false) {
        onBlePair(false);
    }
}

void PairingPresenter::onBlePair(bool paired)
{
    if (paired) {
        model->application().gotoPairedScreenNoTransition();
    } else {
        if (model->bleIsPairingManual()) {
            // Previous screen
            model->application().gotoQRCodeScreenNoTransition();
        } else {
            model->switchToNextPriorityScreen();
        }
    }
}

void PairingPresenter::onBleReconnect()
{
    model->application().gotoPairedScreenNoTransition();
}
