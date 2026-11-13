#include <gui/pairingcancel_screen/PairingCancelView.hpp>
#include <gui/pairingcancel_screen/PairingCancelPresenter.hpp>

PairingCancelPresenter::PairingCancelPresenter(PairingCancelView& v)
    : view(v)
{

}

void PairingCancelPresenter::activate()
{
}

void PairingCancelPresenter::deactivate()
{
}

void PairingCancelPresenter::onBlePinCode()
{
    model->application().gotoPairCodeScreenNoTransition();
}

void PairingCancelPresenter::abortPairing()
{
    model->bleAbortPairing();
}

void PairingCancelPresenter::onBleConnect(bool connected)
{
    if (connected == false) {
        onBlePair(false);
    }
}

void PairingCancelPresenter::onBlePair(bool paired)
{
    if (paired) {
        model->application().gotoPairedScreenNoTransition();
    }
    else {
        if (model->bleIsPairingManual()) {
            // Previous screen
            model->application().gotoQRCodeScreenNoTransition();
        }
        else {
            model->switchToNextPriorityScreen();
        }
    }
}

void PairingCancelPresenter::onBleReconnect()
{
    model->application().gotoPairedScreenNoTransition();
}
