/**
 ******************************************************************************
 * @file    HWButtons.cpp
 * @brief   HWButtons component.
 ******************************************************************************
 *
 ******************************************************************************
 */

#define LOG_MODULE_PRX      "HWButtons"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include "HWButtons.hpp"


bool HWButtons::init()
{
    if (mIsInit) {
        return true;
    }

    assert(mpPins[static_cast<uint16_t>(Id::SW1)] != nullptr);
    assert(mpPins[static_cast<uint16_t>(Id::SW2)] != nullptr);
    assert(mpPins[static_cast<uint16_t>(Id::SW3)] != nullptr);
    assert(mpPins[static_cast<uint16_t>(Id::SW4)] != nullptr);

    for (size_t i = 0; i < mButtons.size(); i++) {
        mButtons[i].init(static_cast<Interface::IButtons::Id>(i), mpPins[i], this);
    }

    mIsInit = true;

    return true;
}

bool HWButtons::deinit()
{
    if (!mIsInit) {
        return true;
    }

    for (size_t i = 0; i < mButtons.size(); i++) {
        mButtons[i].deinit();
    }

    mIsInit = false;
    return true;
}

void HWButtons::inject(Interface::IButtons::Id id, bool pressed)
{
    const size_t i = static_cast<size_t>(id);
    if (!mIsInit || i >= mButtons.size()) {
        return;
    }

    mButtons[i].inject(pressed);
}

void HWButtons::onPress(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onPress, id);

    // Ahead of the chord check below, and outside its guard: a press is told as
    // it happens. Whether it turns out to be a click, a long press or half of a
    // chord is not known yet, and a screen that acts while the button is down
    // cannot wait to find out. A release always follows, so nothing is left
    // believing a button is still held.
    switch (id) {
        case Id::SW1:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L1_PRESS);
            break;
        case Id::SW2:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R1_PRESS);
            break;
        case Id::SW3:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L2_PRESS);
            break;
        case Id::SW4:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R2_PRESS);
            break;
        default:
            break;
    }

    bool sw1 = mButtons[static_cast<uint8_t>(Id::SW1)].get();
    bool sw2 = mButtons[static_cast<uint8_t>(Id::SW2)].get();
    bool sw3 = mButtons[static_cast<uint8_t>(Id::SW3)].get();
    bool sw4 = mButtons[static_cast<uint8_t>(Id::SW4)].get();

    Code code;

    if (sw1 && !sw2 && sw3 && !sw4) {
        code = Code::L1_L2_PRESS;
    } else
    if (!sw1 && sw2 && !sw3 && sw4) {
        code = Code::R1_R2_PRESS;
    } else
    if (sw1 && sw2 && !sw3 && !sw4) {
        code = Code::L1_R1_PRESS;
    } else
    if (!sw1 && !sw2 && sw3 && sw4) {
        code = Code::L2_R2_PRESS;
    } else
    if (sw1 && !sw2 && !sw3 && sw4) {
        code = Code::L1_R2_PRESS;
    } else
    if (!sw1 && sw2 && sw3 && !sw4) {
        code = Code::R1_L2_PRESS;
    } else {
        return;
    }

    if (!mGuard) {
        mGuard = true;
        mObserver.notify(&Interface::IButtons::Callback::onCode, code);
    }
}

void HWButtons::onRelease(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onRelease, id);

    // Unguarded, and for the same reason as the press: this is the one event a
    // screen holding state about a held button must never miss, whatever else
    // the press turned into.
    switch (id) {
        case Id::SW1:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L1_RELEASE);
            break;
        case Id::SW2:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R1_RELEASE);
            break;
        case Id::SW3:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L2_RELEASE);
            break;
        case Id::SW4:
            mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R2_RELEASE);
            break;
        default:
            break;
    }

    bool stillPressed = false;
    for (auto &btn : mButtons) {
        if (btn.get()) {
            stillPressed = true;
        }
    }

    if (!stillPressed) {
        mGuard = false;
    }
}

void HWButtons::onClick(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onClick, id);

    if (!mGuard) {
        switch(id) {
            case Id::SW1:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L1_CLICK);
                break;
            case Id::SW2:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R1_CLICK);
                break;
            case Id::SW3:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L2_CLICK);
                break;
            case Id::SW4:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R2_CLICK);
                break;
            default:
                break;
        }
    }
}

void HWButtons::onLongPress(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onLongPress, id);

    if (!mGuard) {
        switch(id) {
            case Id::SW1:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L1_LONG_PRESS);
                break;
            case Id::SW2:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R1_LONG_PRESS);
                break;
            case Id::SW3:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::L2_LONG_PRESS);
                break;
            case Id::SW4:
                mObserver.notify(&Interface::IButtons::Callback::onCode, Code::R2_LONG_PRESS);
                break;
            default:
                break;
        }
    }
}

void HWButtons::onHold_1s(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onHold_1s, id);
}

void HWButtons::onHold_5s(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onHold_5s, id);
}

void HWButtons::onHold_10s(Id id)
{
    mObserver.notify(&Interface::IButtons::Callback::onHold_10s, id);
}

void HWButtons::Btn::init(Interface::IButtons::Id id, Interface::IExti *pPin,
        Interface::IButtons::Callback *pCallback)
{
    assert(pPin != nullptr);
    assert(pCallback != nullptr);

    mId = id;
    mpPin = pPin;
    mpCallback = pCallback;

    mTim_1s.init("But1s", this, &Btn::timCallback, false, reinterpret_cast<void*>(1));
    mTim_5s.init("But5s", this, &Btn::timCallback, false, reinterpret_cast<void*>(5));
    mTim_10s.init("But10s", this, &Btn::timCallback, false, reinterpret_cast<void*>(10));

    // First, initialize the pins as outputs with level "1" to quickly charge
    // the filtering capacitors. Pins will later be reinitialized as inputs.
    mpPin->init(Driver::Gpio::Type::PP, true);

    mFilter.linkExtiObj(mpPin);
    mFilter.init(Driver::Gpio::Pull::UP, Driver::Gpio::Edge::RISING_FALLING, 5);
    mFilter.configTimings(skDebounceTime, skDebounceTime);
    mFilter.setExtiHandler(this);
    mFilter.setOut(mpPin->get());

    mFilter.clearInt();
    mFilter.enableInt(true);
}

void HWButtons::Btn::deinit()
{
    mFilter.enableInt(false);
    mFilter.setExtiHandler(nullptr);
    mFilter.deinit();

    mTim_1s.deinit();
    mTim_5s.deinit();
    mTim_10s.deinit();
}

void HWButtons::Btn::inject(bool pressed)
{
    // The pin is low while pressed. The edge interrupt feeds the same filter,
    // so keep it out while this thread does.
    taskENTER_CRITICAL();
    mFilter.add(!pressed);
    taskEXIT_CRITICAL();
}

bool HWButtons::Btn::get()
{
    // Inversed value 0 - pressed, 1 - released
    return !mFilter.get();
}

uint32_t HWButtons::Btn::getTime()
{
    uint32_t timeNow = OS::GetTimeMs();
    if (get()) {
        return timeNow - mPressedTimestamp;
    } else {
        return timeNow - mReleasedTimestamp;
    }
}

void HWButtons::Btn::extiHandler(Interface::IExti *pExti,
                Interface::IExti::Edge edge)
{
    // Inversed value 0 - pressed, 1 - released
    if (edge == Interface::IExti::Edge::FALLING) {
        mPressedTimestamp = OS::GetTimeMs();
        LOG_DEBUG("Button %d pressed\n", mId);
        mpCallback->onPress(mId);
        mTim_1s.start(1000);
        mTim_5s.start(5000);
        mTim_10s.start(10000);
    } else if (edge == Interface::IExti::Edge::RISING) {
        mTim_1s.stop();
        mTim_5s.stop();
        mTim_10s.stop();
        mReleasedTimestamp = OS::GetTimeMs();
    }

    if (!get()) {
        uint32_t time = mReleasedTimestamp - mPressedTimestamp;
        // Click  < 500 ms
        if (time < 500) {
            LOG_DEBUG("Button %d click\n", mId);
            mpCallback->onClick(mId);
        }
        // Long press 1000 - 5000 ms
        if (time > 1000 && time  < 5000) {
            LOG_DEBUG("Button %d long press\n", mId);
            mpCallback->onLongPress(mId);
        }
    }

    // Released - the last event
    if (edge == Interface::IExti::Edge::RISING) {
        LOG_DEBUG("Button %d released\n", mId);
        mpCallback->onRelease(mId);
    }
}

void HWButtons::Btn::timCallback(void *arg)
{
    uint32_t evt = reinterpret_cast<uint32_t>(arg);
    switch (evt) {
        case 1:
            LOG_DEBUG("Button %d hold 1s\n", mId);
            mpCallback->onHold_1s(mId);
            break;
        case 5:
            LOG_DEBUG("Button %d hold 5s\n", mId);
            mpCallback->onHold_5s(mId);
            break;
        case 10:
            LOG_DEBUG("Button %d hold 10s\n", mId);
            mpCallback->onHold_10s(mId);
            break;
        default:
            break;
    }
}
