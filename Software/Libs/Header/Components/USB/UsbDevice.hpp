/**
 ******************************************************************************
 * @file    UsbDevice.hpp
 * @brief   USB Device driver. Manages full USB lifecycle including BCD
 *          detection and stack start/stop under a single mutex.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __DRIVER_USB_DEVICE_HPP
#define __DRIVER_USB_DEVICE_HPP

#include <cstdint>
#include <cstdbool>

#include "cmsis_os2.h"
#include "FreeRTOS.h"

#include "usbd_conf.h"
#include "usbd_composite.h"
#include "Usb.h"
#include "Observer.hpp"

#if (USB_CDC_ENABLE == 1)
#include "OS.hpp"
#include "ISerial.hpp"
#endif // (USB_CDC_ENABLE == 1)

/**
 * @brief   USB Device driver.
 *
 * Manages full USB lifecycle: BCD detection, stack init/start/stop.
 * All operations are protected by a single recursive mutex.
 *
 * Typical usage via PMIC event:
 *   onUsbDetected(true)  - VBUS appeared, runs BCD (~500ms), then either
 *                          starts USB stack (PC) or disables PHY (charger).
 *   onUsbDetected(false) - VBUS gone, stops stack and disables PHY.
 *
 * Direct usage without PMIC:
 *   init() -> enable()   - start USB stack directly.
 *   disable() -> deinit()- stop USB stack.
 */
class UsbDevice
#if (USB_CDC_ENABLE == 1)
        : virtual public Interface::ISerial
#endif // (USB_CDC_ENABLE == 1)
{
public:

    /**
     * @brief   Unified callback interface for all USB events.
     */
    class Callback {
    public:
        virtual void onConnectedPC()      {}
        virtual void onConnectedCharger() {}
        virtual void onDisconnect()       {}
        /**
         * @brief   A host has configured the device (SET_CONFIGURATION) while
         *          Mass Storage is off. Unlike onConnectedPC(), which only
         *          reflects the BCD result, this proves a host is talking to
         *          the device - a charger BCD mistakes for a PC never gets
         *          here. Delivered on the observer dispatcher task.
         */
        virtual void onHostConfigured()   {}
#if (USB_CDC_ENABLE == 1)
        virtual void onUsbOpenVCP()       {}
        virtual void onUsbCloseVCP()      {}
#endif
    protected:
        virtual ~Callback() = default;
    };

    /**
     * @brief   Get singleton instance.
     * @retval  Reference to UsbDevice instance.
     */
    static UsbDevice& GetInstance()
    {
        static UsbDevice sInstance;
        return sInstance;
    }

    /**
     * @brief   Called when VBUS appears or disappears (e.g. from PMIC event).
     *          On connect: runs BCD detection (~500ms), then starts USB stack
     *          if PC detected, or disables PHY if charger detected.
     *          Does not guarantee USB will be started - depends on BCD result.
     *          On disconnect: stops stack and disables PHY.
     * @param   state: true if VBUS present, false if VBUS gone.
     */
    void onUsbDetected(bool state);

    /**
     * @brief   Initialize USB device stack. Call once at startup.
     *          PHY is enabled temporarily for register init then disabled.
     * @param   mscEnable: Enable Mass Storage class.
     * @param   mscDisk: Mass Storage disk index.
     * @retval  true if success, false otherwise.
     */
    bool init(bool mscEnable = false, uint8_t mscDisk = 0);

    /**
     * @brief   Deinitialize USB device stack.
     * @retval  true if success, false otherwise.
     */
    bool deinit();

    /**
     * @brief   Reinitialize USB stack with new MSC configuration.
     *          Can be called while USB is connected to PC - host will
     *          see a short reconnect and remount with new configuration.
     *          PHY and Sleep state are preserved across the call.
     * @param   mscEnable: Enable Mass Storage class. Ignored (left off) when
     *          no cable is attached.
     * @param   mscDisk: Mass Storage disk index.
     * @retval  true if success, false otherwise.
     */
    bool reinit(bool mscEnable, uint8_t mscDisk);

    /**
     * @brief   Start USB stack (soft-connect).
     *          Enables PHY if not already active.
     * @retval  true if success, false otherwise.
     */
    bool enable();

    /**
     * @brief   Stop USB stack (soft-disconnect).
     *          Disables PHY after stack is stopped.
     * @retval  true if success, false otherwise.
     */
    bool disable();

    /**
     * @brief   Check if USB stack is running.
     * @retval  true if enabled, false otherwise.
     */
    bool isEnabled()
    {
        return mIsOn;
    }

    /**
     * @brief   Check if a host has configured the device while Mass Storage
     *          is off: the state onHostConfigured() reports. Lets an observer
     *          that attaches late catch up on a notification it missed.
     * @retval  true if configured by a host with Mass Storage off.
     */
    bool isHostConfigured();

    /**
     * @brief   Check if Mass Storage is part of the device's configuration.
     * @retval  true if the host is (or will be) offered the drive.
     */
    bool isMscEnabled()
    {
        return mIsMscEnabled;
    }

    /**
     * @brief   Get last detected port type from BCD.
     * @retval  DriverUsb_PortType_t value.
     */
    DriverUsb_PortType_t getPortType()
    {
        return mPortType;
    }

    /**
     * @brief   Attach observer for USB events.
     * @param   callback: Pointer to callback to attach.
     */
    void attachCallback(Callback *callback)
    {
        mObserver.add(callback);
    }

    /**
     * @brief   Detach observer from USB events.
     * @param   callback: Pointer to callback to detach.
     */
    void detachCallback(Callback *callback)
    {
        mObserver.del(callback);
    }

#if (USB_CDC_ENABLE == 1)

    /**
     * @brief   Check if CDC serial port is open.
     * @retval  true if port is open, false otherwise.
     */
    bool isSerialPortOpen();

    /**
     * @brief   Transmit data over CDC.
     * @param   pData: Pointer to data buffer.
     * @param   size: Number of bytes to transmit.
     * @param   blocking: true for blocking mode, false for non-blocking.
     * @retval  true if success, false otherwise.
     */
    virtual bool transmit(const uint8_t *pData, uint32_t size,
            bool blocking = true) override;

    /**
     * @brief   Receive data from CDC.
     * @param   pData: Pointer to receive buffer.
     * @param   size: Maximum number of bytes to receive.
     * @param   br: Number of bytes actually received.
     * @param   timeout: Timeout in ms waiting for data.
     * @retval  true if at least 1 byte received, false otherwise.
     */
    virtual bool receive(uint8_t *pData, uint32_t size, uint32_t &br,
            uint32_t timeout = 0) override;

    /**
     * @brief   Get number of bytes available in RX buffer.
     * @retval  Byte count.
     */
    uint32_t getRxBytesAvailable()
    {
        return mStreamBuffer.getCount();
    }

#endif // (USB_CDC_ENABLE == 1)

private:

    bool                 mIsInit       = false;
    bool                 mIsOn         = false;
    bool                 mIsMscEnabled = false;
    uint8_t              mMscDisk      = 0;
    DriverUsb_PortType_t mPortType     = DRIVER_USB_PORT_NONE;

    osMutexId_t       mMutex {};
    StaticSemaphore_t mMutexControlBlock {};

    Observer::Holder<Callback, 5> mObserver;

    UsbDevice()          = default;
    virtual ~UsbDevice() = default;

    bool initDevice();
    bool lock();
    void unlock();

    // BCD result handler
    static void OnBcdResult(DriverUsb_PortType_t portType);
    void handleBcdResult(DriverUsb_PortType_t portType);

    // SET_CONFIGURATION from the host (USB interrupt)
    static void configuredCb();

#if (USB_CDC_ENABLE == 1)
    OS::StreamBuffer<256> mStreamBuffer {};

    static void portStateCb(uint8_t ch, bool state);
    static void receiveCb(uint8_t ch, uint8_t *buf, uint32_t len);
#endif // (USB_CDC_ENABLE == 1)

#if (USB_MSC_ENABLE == 1)
    static USBD_StatusTypeDef MSC_Status(uint8_t drv);
    static USBD_StatusTypeDef MSC_Read(uint8_t drv, uint8_t *buf,
            uint32_t blk_addr, uint16_t blk_len);
    static USBD_StatusTypeDef MSC_Write(uint8_t drv, const uint8_t *buf,
            uint32_t blk_addr, uint16_t blk_len);
    static USBD_StatusTypeDef MSC_BlockSize(uint8_t drv, uint32_t *block_size);
    static USBD_StatusTypeDef MSC_BlockNum(uint8_t drv, uint32_t *block_num);
#endif // (USB_MSC_ENABLE == 1)
};

#endif /* __DRIVER_USB_DEVICE_HPP */
