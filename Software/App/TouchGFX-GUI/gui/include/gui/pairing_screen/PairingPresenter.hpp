#ifndef PAIRINGPRESENTER_HPP
#define PAIRINGPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class PairingView;

class PairingPresenter : public touchgfx::Presenter, public ModelListener
{
public:
    PairingPresenter(PairingView& v);

    /**
     * The activate function is called automatically when this screen is "switched in"
     * (ie. made active). Initialization logic can be placed here.
     */
    virtual void activate();

    /**
     * The deactivate function is called automatically when this screen is "switched out"
     * (ie. made inactive). Teardown functionality can be placed here.
     */
    virtual void deactivate();

    virtual ~PairingPresenter() {}

    virtual void onUsbConnect() override {}

    virtual void onBlePinCode() override;
    virtual void onBleConnect(bool connected) override;
    virtual void onBlePair(bool paired) override;
    virtual void onBleReconnect() override;

private:
    PairingPresenter();

    PairingView& view;
};

#endif // PAIRINGPRESENTER_HPP
