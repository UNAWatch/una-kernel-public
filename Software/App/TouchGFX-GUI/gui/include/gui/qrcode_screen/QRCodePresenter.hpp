#ifndef QRCODEPRESENTER_HPP
#define QRCODEPRESENTER_HPP

#include <gui/model/ModelListener.hpp>
#include <mvp/Presenter.hpp>

using namespace touchgfx;

class QRCodeView;

class QRCodePresenter : public touchgfx::Presenter, public ModelListener
{
public:
    QRCodePresenter(QRCodeView& v);

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

    virtual void onUsbConnect() override {}

    void onBlePinCode()               override;
    void onBleConnect(bool connected) override;
    void onBlePair(bool paired)       override;
    void onBleReconnect()             override;

    virtual ~QRCodePresenter() {}

private:
    QRCodePresenter();

    QRCodeView& view;
};

#endif // QRCODEPRESENTER_HPP
