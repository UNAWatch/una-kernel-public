#include <gui/paired_screen/PairedView.hpp>
#include <gui/paired_screen/PairedPresenter.hpp>

PairedPresenter::PairedPresenter(PairedView& v)
    : view(v)
{

}

void PairedPresenter::activate()
{
    if (model->bleIsReconnected()) {
        view.showReconnected();
    }
}

void PairedPresenter::deactivate()
{

}

void  PairedPresenter::exit()
{
    model->switchToNextPriorityScreen();
}