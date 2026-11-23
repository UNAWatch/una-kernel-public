/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : TouchGFXHAL.cpp
  ******************************************************************************
  * This file was created by TouchGFX Generator 4.23.0. This file is only
  * generated once! Delete this file from your project and re-generate code
  * using STM32CubeMX or change this file manually to update it.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#include <TouchGFXHAL.hpp>

/* USER CODE BEGIN TouchGFXHAL.cpp */

#define LOG_MODULE_PRX      "TouchGFXHAL"
#define LOG_MODULE_LEVEL    LOG_LEVEL_DEBUG
#include "Logger.h"

#include <cassert>
#include "main.h"
#include <gui/common/FrontendHeap.hpp>
#include <platform/driver/button/ButtonController.hpp>
#include <touchgfx/hal/OSWrappers.hpp>

#include "OS.hpp"
#include "HWButtons.hpp"
#include "AppSurface.hpp"
#include "ComponentFactory.hpp"
#include "FrameCapture.hpp"
#include "app_touchgfx.h"

#include <atomic>

using namespace touchgfx;



class HWButtonController : public touchgfx::ButtonController,
                           public Interface::IButtons::Callback {

public:
    virtual void init() override
    {
        mQueue.init("HWButtonController");
        HWButtons::GetInstance().attachCallback(this);
    }

    virtual bool sample(uint8_t &key)
    {
        key = 0;
        if (mQueue.pop(key, 1) && key != 0) {
            return true;
        }

        return false;
    }

private:
    /// Deep enough for several button actions in flight at once. TouchGFX takes
    /// one code per frame out of here, so at the frame rate the queue drains at
    /// ten a second while one press of one button now puts up to three in
    /// (press, the click a short release turns into, then the release) where it
    /// used to put one. At a depth of three a second tap arriving inside 300 ms
    /// overflowed, and the code most costly to lose is a release: a screen that
    /// scrolls while a button is held would go on scrolling with nothing held.
    OS::Queue<uint8_t, 16> mQueue;

    virtual void onCode(Interface::IButtons::Code code) override
    {
        mQueue.push(static_cast<uint8_t>(code), 1);
    }
};

static HWButtonController sButtonController;

static const int16_t  skWidth      = LCD_WIDTH;
static const int16_t  skHeight     = LCD_WIDTH;
static const uint32_t skBufferSize = skWidth * skHeight;

static Interface::ILcd *spLcd;

// Frame buffers
static uint8_t sFrameBuffer[skBufferSize];
//uint8_t sDoubleBuffer[skBufferSize];      // Not used
//uint8_t sAnimationBuffer[skBufferSize];   // Not used

// Pointer to the current active buffer
static uint8_t *spActiveBuffer;

// Request to flush active buffer at the end of frame
static bool sFlushBufferReq;

// Keeps frames off the panel while the kernel boots, so it goes on showing the
// bootloader's splash image. See releaseBootHold().
static bool sBootHold = true;

// FrameCapture: a thread asks for a frame, endFrame() tells it the frame is
// complete and waits until it has been read.
static std::atomic<bool> sCaptureRequested;

static OS::Semaphore& captureReady()
{
    static OS::Semaphore sem(1, 0);
    return sem;
}

static OS::Semaphore& captureDone()
{
    static OS::Semaphore sem(1, 0);
    return sem;
}

bool FrameCapture::hold(uint32_t timeoutMs)
{
    // Leftovers from a capture that timed out.
    while (captureReady().take(0)) {}
    while (captureDone().take(0)) {}

    sCaptureRequested = true;
    TouchGFX_ForceFrame();

    if (captureReady().take(timeoutMs)) {
        return true;
    }

    // Withdraw the request, unless endFrame() took it meanwhile: then it is
    // about to say the frame is ready.
    if (sCaptureRequested.exchange(false)) {
        return false;
    }
    return captureReady().take(1000);
}

void FrameCapture::release()
{
    captureDone().give();
}

const uint8_t* FrameCapture::pixels()
{
    return sFrameBuffer;
}

uint16_t FrameCapture::width()
{
    return skWidth;
}

uint16_t FrameCapture::height()
{
    return skHeight;
}

void TouchGFXHAL::initialize()
{
    // Calling parent implementation of initialize().
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.
    // Please note, HAL::initialize() must be called to initialize the framework.

    //TouchGFXGeneratedHAL::initialize();

    HAL::initialize();
    HAL::setFrameRateCompensation(true);

    registerEventListener(*(Application::getInstance()));

    Interface::ILcd *pLcd = &ComponentFactory::GetInstance().getLcd();
    if (pLcd != nullptr) {
        pLcd->setResolution(skWidth, skHeight);
        int16_t width = 0;
        int16_t height = 0;
        pLcd->getResolution(width, height);

        assert(skWidth == width);
        assert(skHeight == height);

        if (skWidth == width && skHeight == height) {
            spLcd = pLcd;

            spLcd->init();
            spLcd->enable();

            // The display has an image installed by the bootloader,
            // so we will not clear it. It stays up until the boot hold
            // is released (see releaseBootHold()).
#if 0
            spLcd->write(sFrameBuffer);
#endif
        } else {
            LOG_ERROR("Resolution %d x %d not supported\n", width, height);
        }
    }

    spActiveBuffer = sFrameBuffer;
    setFrameBufferStartAddresses((void*)sFrameBuffer, nullptr, nullptr);
    //setFrameBufferStartAddresses((void*)sFrameBuffer, (void*)sDoubleBuffer, (void*)sAnimationBuffer);

    sButtonController.init();
    setButtonController(&sButtonController);
}

/**
 * Gets the frame buffer address used by the TFT controller.
 *
 * @return The address of the frame buffer currently being displayed on the TFT.
 */
uint16_t* TouchGFXHAL::getTFTFrameBuffer() const
{
    // Calling parent implementation of getTFTFrameBuffer().
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.

    //return TouchGFXGeneratedHAL::getTFTFrameBuffer();
    return (uint16_t*)spActiveBuffer;
}

/**
 * Sets the frame buffer address used by the TFT controller.
 *
 * @param [in] address New frame buffer address.
 */
void TouchGFXHAL::setTFTFrameBuffer(uint16_t* address)
{
    // Calling parent implementation of setTFTFrameBuffer(uint16_t* address).
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.

    //TouchGFXGeneratedHAL::setTFTFrameBuffer(address);
    spActiveBuffer = (uint8_t*)address;
}

/**
 * This function is called whenever the framework has performed a partial draw.
 *
 * @param rect The area of the screen that has been drawn, expressed in absolute coordinates.
 *
 * @see flushFrameBuffer().
 */
void TouchGFXHAL::flushFrameBuffer(const touchgfx::Rect& rect)
{
    // Calling parent implementation of flushFrameBuffer(const touchgfx::Rect& rect).
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.
    // Please note, HAL::flushFrameBuffer(const touchgfx::Rect& rect) must
    // be called to notify the touchgfx framework that flush has been performed.
    // To calculate he start adress of rect,
    // use advanceFrameBufferToRect(uint8_t* fbPtr, const touchgfx::Rect& rect)
    // defined in TouchGFXGeneratedHAL.cpp

    sFlushBufferReq = true;
    TouchGFXGeneratedHAL::flushFrameBuffer(rect);
}

bool TouchGFXHAL::blockCopy(void* RESTRICT dest, const void* RESTRICT src, uint32_t numBytes)
{
    return TouchGFXGeneratedHAL::blockCopy(dest, src, numBytes);
}

/**
 * Configures the interrupts relevant for TouchGFX. This primarily entails setting
 * the interrupt priorities for the DMA and LCD interrupts.
 */
void TouchGFXHAL::configureInterrupts()
{
    // Calling parent implementation of configureInterrupts().
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.

    TouchGFXGeneratedHAL::configureInterrupts();
}

/**
 * Used for enabling interrupts set in configureInterrupts()
 */
void TouchGFXHAL::enableInterrupts()
{
    // Calling parent implementation of enableInterrupts().
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.

    TouchGFXGeneratedHAL::enableInterrupts();
}

/**
 * Used for disabling interrupts set in configureInterrupts()
 */
void TouchGFXHAL::disableInterrupts()
{
    // Calling parent implementation of disableInterrupts().
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.

    TouchGFXGeneratedHAL::disableInterrupts();
}

/**
 * Configure the LCD controller to fire interrupts at VSYNC. Called automatically
 * once TouchGFX initialization has completed.
 */
void TouchGFXHAL::enableLCDControllerInterrupt()
{
    // Calling parent implementation of enableLCDControllerInterrupt().
    //
    // To overwrite the generated implementation, omit call to parent function
    // and implemented needed functionality here.

    TouchGFXGeneratedHAL::enableLCDControllerInterrupt();
}

// Held while the framework renders, so a frame the app delivers mid-render
// cannot land in the surface half way through being read. Tracked rather than
// assumed: HAL::beginFrame() can decline a frame, and there is no contract in
// this tree saying endFrame() follows it either way, so a lock taken and never
// released would wedge the display.
static bool sSurfaceLocked;

bool TouchGFXHAL::beginFrame()
{
    bool render = TouchGFXGeneratedHAL::beginFrame();

    if (render && !sSurfaceLocked) {
        sSurfaceLocked = AppSurface::GetInstance().lock();
    }

    return render;
}

void TouchGFXHAL::endFrame()
{
    static bool sLcdOn;

    // Before the panel write, which is long and reads only the framebuffer:
    // holding the surface across it would stall the app for a whole frame.
    if (sSurfaceLocked) {
        AppSurface::GetInstance().unlock();
        sSurfaceLocked = false;
    }

    if (spLcd != nullptr) {
        bool isOn = spLcd->isEnabled();
        // Update the LCD if the image has changed or if it has been turned on,
        // unless the bootloader's splash image is still being held
        if (!sBootHold && isOn && (sFlushBufferReq || (sLcdOn != isOn))) {
            spLcd->write(spActiveBuffer);
        }
        sFlushBufferReq = false;
        sLcdOn = isOn;
    }

    // The frame is complete: hold it for a capture until it has been read.
    if (sCaptureRequested.exchange(false)) {
        captureReady().give();
        captureDone().take(3000);
    }

    TouchGFXGeneratedHAL::endFrame();
}

void TouchGFXHAL::releaseBootHold()
{
    if (sBootHold) {
        // Whatever is pending was drawn for the boot splash; drop it so the
        // first write is the next screen, which redraws all of it.
        sFlushBufferReq = false;
        sBootHold = false;
    }
}

/* USER CODE END TouchGFXHAL.cpp */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
