#include <gui/qrcode_screen/QRCodeView.hpp>
#include <gui/qrcode_screen/QRCodePresenter.hpp>

QRCodePresenter::QRCodePresenter(QRCodeView& v)
    : view(v)
{
}

void QRCodePresenter::activate()
{
    if (!model->bleIsPairingActive()) {
        model->bleStartPairing();
    }
    view.setInfo(model->getDeviceInfo());
}

void QRCodePresenter::deactivate()
{
}

void QRCodePresenter::onBlePinCode()
{
    model->application().gotoPairCodeScreenNoTransition();
}

void QRCodePresenter::onBleConnect(bool connected)
{
    if (connected == false) {
        onBlePair(false);
    }
}

void QRCodePresenter::onBlePair(bool paired)
{
    if (paired) {
        model->application().gotoPairedScreenNoTransition();
    }
    else {
        if (model->bleIsPairingManual()) {
			// stay at the same screen

            // Previous screen
            //model->application().gotoPairingInfoScreenNoTransition();
        }
        else {
            model->switchToNextPriorityScreen();
        }
    }
}

void QRCodePresenter::onBleReconnect()
{
    model->application().gotoPairedScreenNoTransition();
}
