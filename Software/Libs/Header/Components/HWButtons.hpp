/**
 ******************************************************************************
 * @file    HWButtons.hpp
 * @brief   HWButtons component.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __COMPONENTS_HWBUTTONS_HPP
#define __COMPONENTS_HWBUTTONS_HPP

#include <cstdint>
#include <cstdbool>
#include <array>

#include "IButtons.hpp"

#include "Gpio.hpp"
#include "DebounceExti.hpp"
#include "Observer.hpp"
#include "OS.hpp"



class HWButtons : virtual public Interface::IButtons,
                  virtual public Interface::IButtons::Callback {

public:

    /**
     * @brief   Get HWButtons instance.
     * @retval  HWButtons instance.
     */
    static HWButtons& GetInstance()
    {
        static HWButtons sInstance;
        return sInstance;
    }

    /**
     * @brief   Configure inputs for each button.
     * @param   sw1: Interrupt pin for SW1.
     * @param   sw2: Interrupt pin for SW2.
     * @param   sw2: Interrupt pin for SW3.
     * @param   sw4: Interrupt pin for SW4.
     * @retval  Execution status. 'true' if success, 'false' otherwise.
     */
    bool config(Interface::IExti &sw1, Interface::IExti &sw2,
            Interface::IExti &sw3, Interface::IExti &sw4)
    {
        mpPins[static_cast<uint8_t>(Id::SW1)] = &sw1;
        mpPins[static_cast<uint8_t>(Id::SW2)] = &sw2;
        mpPins[static_cast<uint8_t>(Id::SW3)] = &sw3;
        mpPins[static_cast<uint8_t>(Id::SW4)] = &sw4;
        return true;
    }

    /**
     * @brief   Initialization of the HWButtons instance.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    bool init();

    /**
     * @brief   Deinitialization of the HWButtons instance.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    bool deinit();

    /**
     * @copydoc   Interface::IButtons::attachCallback()
     */
    virtual void attachCallback(Interface::IButtons::Callback *pCallback) override
    {
        mObserver.add(pCallback);
    }

    /**
     * @copydoc   Interface::IButtons::detachCallback()
     */
    virtual void detachCallback(Interface::IButtons::Callback *pCallback) override
    {
        mObserver.del(pCallback);
    }

    /**
     * @copydoc   Interface::IButtons::get()
     */
    virtual bool get(Interface::IButtons::Id id) override
    {
        return mButtons[static_cast<uint8_t>(id)].get();
    }

    /**
     * @copydoc   Interface::IButtons::getTime()
     */
    virtual uint32_t getTime(Interface::IButtons::Id id) override
    {
        return mButtons[static_cast<uint8_t>(id)].getTime();
    }

    /**
     * @brief   Press or release a button as if by hand.
     *
     * @details The level goes into the button's debounce filter where the
     *          pin's own level would, so every observer, the click and long
     *          press timing, the hold timers and get() see what a finger gives.
     *          Used by the developer-mode USB commands. Call from a thread,
     *          not an interrupt.
     *
     * @param   id      The button.
     * @param   pressed true to press it, false to release it.
     */
    void inject(Interface::IButtons::Id id, bool pressed);

protected:

    HWButtons() = default;;

    virtual ~HWButtons() = default;

private:

    bool mIsInit {};

    /// Array of an observers
    Observer::Holder<Interface::IButtons::Callback, 5> mObserver { };

    std::array<Interface::IExti*, static_cast<uint8_t>(Id::MAX_NUM)> mpPins;


    class Btn : virtual public Interface::IExti::Handler {
    public:
        void init(Interface::IButtons::Id id, Interface::IExti *pPin, Interface::IButtons::Callback *pCallback);
        void deinit();
        bool get();
        uint32_t getTime();
        void inject(bool pressed);

        virtual ~Btn() = default;
    private:
        Interface::IButtons::Id mId { };
        Interface::IExti *mpPin { };

        static const uint32_t skDebounceTime = 50;
        Filter::DebounceExti mFilter { };

        uint32_t mPressedTimestamp { };
        uint32_t mReleasedTimestamp { };

        Interface::IButtons::Callback *mpCallback { };

        OS::Timer<Btn> mTim_1s;
        OS::Timer<Btn> mTim_5s;
        OS::Timer<Btn> mTim_10s;

        /**
         * @copydoc   Interface::IExti::extiHandler().
         */
        virtual void extiHandler(Interface::IExti *pExti,
                Interface::IExti::Edge edge) override;

        void timCallback(void *arg);


    };

    /// Buttons list
    std::array<Btn, static_cast<uint8_t>(Id::MAX_NUM)> mButtons;

    // A flag to ignore any events after a combination
    // is detected until all buttons are released.
    bool mGuard { };


    virtual void onPress(Id id) override;
    virtual void onRelease(Id id) override;
    virtual void onClick(Id id) override;
    virtual void onLongPress(Id id) override;
    virtual void onHold_1s(Id id) override;
    virtual void onHold_5s(Id id) override;
    virtual void onHold_10s(Id id) override;

};


#endif /* __COMPONENTS_HWButtons_HPP */
