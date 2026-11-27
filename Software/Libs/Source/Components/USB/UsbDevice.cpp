/**
 ******************************************************************************
 * @file    UsbDevice.cpp
 * @brief   USB Device driver. Manages full USB lifecycle including BCD
 *          detection and stack start/stop under a single mutex.
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "UsbDevice.hpp"

#define LOG_MODULE_PRX      "UsbDevice"
#define LOG_MODULE_LEVEL    LOG_LEVEL_DEBUG
#include "Logger.h"

#include <cassert>

#include "usbd_core.h"
#include "usbd_desc.h"

#if (USB_CDC_ENABLE == 1)
#include "usbd_cdc_acm.h"
#include "usbd_cdc_acm_if.h"
#define USBD_CDC_CH (0)
#endif // (USB_CDC_ENABLE == 1)

#if (USB_MSC_ENABLE == 1)
#include "flash_diskio.h"
#include "usbd_msc.h"
#include "usbd_msc_if.h"
extern "C" void USBD_COMPOSITE_EnableMSC(bool enable);
#endif // (USB_MSC_ENABLE == 1)

extern "C" void USBD_COMPOSITE_SetConfiguredCallback(void (*cb)(void));

// USB Device handle
static USBD_HandleTypeDef hUsbDevice;

/* --------------------------------------------------------------------------
 * VBUS detection entry point
 * -------------------------------------------------------------------------- */

void UsbDevice::onUsbDetected(bool state)
{
    LOG_DEBUG("USB cable %s\n", state ? "plugged" : "unplugged");

    if (!lock()) {
        return;
    }

    // Guard against duplicate events using mPortType as connection state indicator:
    // NONE means no active connection - either not yet connected or BCD gave no result.
    if (state == (mPortType != DRIVER_USB_PORT_NONE)) {
        unlock();
        return;
    }

    if (state) {
        DriverUsb_SetBcdCallback(OnBcdResult);
        DriverUsb_PhyEnable();
        DriverUsb_VBUSDetect();     // blocking ~500ms, result via OnBcdResult
    } else {
        if (mIsOn) {
            USBD_Stop(&hUsbDevice);
            DriverUsb_Delay(10);
            mIsOn = false;
        }
        DriverUsb_PhyDisable();
        mPortType = DRIVER_USB_PORT_NONE;
    }

    DriverUsb_PortType_t portType = mPortType;
    unlock();

    // Notify after mutex release to avoid deadlock if callback re-enters UsbDevice
    if (state) {
        switch (portType) {
            case DRIVER_USB_PORT_SDP:
            case DRIVER_USB_PORT_CDP:
                // CDP is a host port, so it takes the PC path. That only sets
                // the charge current: BCD also reads some wall chargers as SDP
                // or CDP, so MSD waits for onHostConfigured(), which only a
                // real host can cause.
                // Both currents are equal today (BATTERY_CHARGE_CURRENT_PC ==
                // BATTERY_CHARGE_CURRENT). Should they diverge, CDP must keep
                // this notification and gain the charger current - it must not
                // move to the DCP case, which never enumerates.
                mObserver.notify(&Callback::onConnectedPC);
                break;
            case DRIVER_USB_PORT_DCP:
                mObserver.notify(&Callback::onConnectedCharger);
                break;
            default:
                break;
        }
    } else {
        mObserver.notify(&Callback::onDisconnect);
    }
}

/* --------------------------------------------------------------------------
 * BCD result - called from DriverUsb in same task context as onUsbDetected.
 * Mutex is already held by onUsbDetected at this point.
 * Only updates state and starts/stops stack - no notify, no unlock.
 * -------------------------------------------------------------------------- */

void UsbDevice::OnBcdResult(DriverUsb_PortType_t portType)
{
    GetInstance().handleBcdResult(portType);
}

void UsbDevice::handleBcdResult(DriverUsb_PortType_t portType)
{
    mPortType = portType;

    switch (portType) {
        case DRIVER_USB_PORT_SDP:
        case DRIVER_USB_PORT_CDP:
            // A CDP is a host port that also supplies charger-level current,
            // so it must enumerate exactly like an SDP. Only DCP is a bus
            // with no host behind it.
            LOG_DEBUG("BCD: PC detected (%s)\n",
                    portType == DRIVER_USB_PORT_SDP ? "SDP" : "CDP");
            if (mIsInit) {
                mIsOn = USBD_Start(&hUsbDevice) == USBD_OK;
                if (!mIsOn) {
                    LOG_ERROR("USBD_Start failed\n");
                    DriverUsb_PhyDisable();
                }
            } else {
                LOG_ERROR("BCD: PC detected but stack not initialized\n");
                DriverUsb_PhyDisable();
            }
            break;

        case DRIVER_USB_PORT_DCP:
            LOG_DEBUG("BCD: Charger detected (DCP)\n");
            DriverUsb_PhyDisable();
            break;

        default:
            LOG_DEBUG("BCD: No port detected\n");
            DriverUsb_PhyDisable();
            break;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

bool UsbDevice::init(bool mscEnable, uint8_t mscDisk)
{
    if (mIsInit) {
        return true;
    }

    if (!mMutex) {
        const osMutexAttr_t kAttr = {
            .name      = LOG_MODULE_PRX,
            .attr_bits = osMutexRecursive,
            .cb_mem    = &mMutexControlBlock,
            .cb_size   = sizeof(mMutexControlBlock)
        };
        mMutex = osMutexNew(&kAttr);
    }

    if (!lock()) {
        return false;
    }

#if (USB_CDC_ENABLE == 1)
    mStreamBuffer.init(LOG_MODULE_PRX);
#endif

    mIsMscEnabled = mscEnable;
    mMscDisk      = mscDisk;

    // PHY clocks required for HAL_PCD_Init register access
    DriverUsb_PhyEnable();
    mIsInit = initDevice();
    // PHY not needed until enable() - shut it down to save power
    DriverUsb_PhyDisable();

    unlock();
    return mIsInit;
}

bool UsbDevice::deinit()
{
    if (!mIsInit) {
        return true;
    }

    if (!lock()) {
        return false;
    }

    if (mIsOn) {
        USBD_Stop(&hUsbDevice);
        DriverUsb_Delay(10);
        mIsOn = false;
    }

    // PHY clocks required for USBD_DeInit register access
    DriverUsb_PhyEnable();
    USBD_DeInit(&hUsbDevice);
    DriverUsb_PhyDisable();

    mIsInit = false;
    unlock();
    return true;
}

bool UsbDevice::reinit(bool mscEnable, uint8_t mscDisk)
{
    if (!mIsInit) {
        return false;
    }

    if (!lock()) {
        return false;
    }

    // Under the lock onUsbDetected() takes: a cable pulled before this has
    // cleared mPortType, so mass storage stays off; one pulled after finds it
    // on and the disconnect takes it off. Never on with no cable, as nothing
    // would take it off before a replug handed the host the volume.
    if (mscEnable && mPortType == DRIVER_USB_PORT_NONE) {
        LOG_WARNING("reinit: no cable, mass storage left off\n");
        mscEnable = false;
    }

    // Remember if stack was running - restore state after reinit
    bool wasOn = mIsOn;

    if (wasOn) {
        USBD_Stop(&hUsbDevice);
    }

    // PHY clocks required for USBD_DeInit register access
    bool phyWasActive = DriverUsb_IsPhyEnabled();
    if (!phyWasActive) {
        DriverUsb_PhyEnable();
    }
    USBD_DeInit(&hUsbDevice);
    if (!phyWasActive) {
        DriverUsb_PhyDisable();
    }
    mIsOn   = false;
    mIsInit = false;

    mIsMscEnabled = mscEnable;
    mMscDisk      = mscDisk;
    mIsInit = initDevice();

    if (mIsInit && wasOn) {
        mIsOn = USBD_Start(&hUsbDevice) == USBD_OK;
        if (!mIsOn) {
            LOG_ERROR("reinit: USBD_Start failed\n");
        }
    }

    unlock();
    return mIsInit;
}

bool UsbDevice::enable()
{
    if (!mIsInit) {
        return false;
    }

    if (mIsOn) {
        return true;
    }

    if (!lock()) {
        return false;
    }

    DriverUsb_PhyEnable();
    mIsOn = USBD_Start(&hUsbDevice) == USBD_OK;

    if (!mIsOn) {
        DriverUsb_PhyDisable();
        LOG_ERROR("USBD_Start failed\n");
    }

    unlock();
    return mIsOn;
}

bool UsbDevice::disable()
{
    if (!mIsOn) {
        return true;
    }

    if (!lock()) {
        return false;
    }

    USBD_Stop(&hUsbDevice);
    DriverUsb_Delay(10);
    DriverUsb_PhyDisable();
    mIsOn = false;

    unlock();
    return true;
}

bool UsbDevice::isHostConfigured()
{
    if (!lock()) {
        return false;
    }

    const bool configured = mIsOn && !mIsMscEnabled &&
            hUsbDevice.dev_state == USBD_STATE_CONFIGURED;

    unlock();
    return configured;
}

/* --------------------------------------------------------------------------
 * Private
 * -------------------------------------------------------------------------- */

bool UsbDevice::initDevice()
{
#if (USB_CDC_ENABLE == 1)
    USBD_CDC_config(&hUsbDevice, portStateCb, receiveCb);
#endif

#if (USB_MSC_ENABLE == 1)
    USBD_DiskIO io {};
    io.status       = MSC_Status;
    io.read         = MSC_Read;
    io.write        = MSC_Write;
    io.getBlockSize = MSC_BlockSize;
    io.getBlockNum  = MSC_BlockNum;
    USBD_STORAGE_config(mMscDisk, io);
    USBD_COMPOSITE_EnableMSC(mIsMscEnabled);
#endif

    USBD_COMPOSITE_Mount_Class();
    USBD_COMPOSITE_SetConfiguredCallback(configuredCb);

#if defined(USB_SPEED_HS) && (USB_SPEED_HS == 1)
    uint8_t id = DRIVER_USB_HS;
#else
    uint8_t id = DRIVER_USB_FS;
#endif

    if (USBD_Init(&hUsbDevice, &USBD_Desc, id) != USBD_OK) {
        return false;
    }

    if (USBD_RegisterClass(&hUsbDevice, &USBD_COMPOSITE) != USBD_OK) {
        return false;
    }

#if (USB_CDC_ENABLE == 1)
    static USBD_CDC_ACM_ItfTypeDef cdc_fops = USBD_CDC_getInterface();
    if (USBD_CDC_ACM_RegisterInterface(&hUsbDevice, &cdc_fops) != USBD_OK) {
        return false;
    }
#endif

#if (USB_MSC_ENABLE == 1)
    if (mIsMscEnabled) {
        static USBD_StorageTypeDef storage_fops = USBD_STORAGE_getInterface();
        if (USBD_MSC_RegisterStorage(&hUsbDevice, &storage_fops) != USBD_OK) {
            return false;
        }
    }
#endif

    return true;
}

bool UsbDevice::lock()
{
    return osMutexAcquire(mMutex, osWaitForever) == osOK;
}

void UsbDevice::unlock()
{
    osMutexRelease(mMutex);
}

void UsbDevice::configuredCb()
{
    // USB interrupt: no logging, for the reason given in portStateCb.
    //
    // Only while Mass Storage is off. Opening it re-enumerates the device with
    // MSC on, and the host configures it again; reporting that one would
    // open Mass Storage a second time.
    UsbDevice &self = GetInstance();
    if (!self.mIsMscEnabled) {
        self.mObserver.notifyISR(&Callback::onHostConfigured);
    }
}

/* --------------------------------------------------------------------------
 * CDC callbacks
 * -------------------------------------------------------------------------- */

#if (USB_CDC_ENABLE == 1)

bool UsbDevice::isSerialPortOpen()
{
    return USBD_CDC_isPortOpen(USBD_CDC_CH);
}

bool UsbDevice::transmit(const uint8_t *pData, uint32_t size, bool blocking)
{
    bool status = false;

    assert(pData != nullptr);

    const uint32_t kTimeoutMs = 500U;
    uint32_t startMs = DriverUsb_GetTime();

    if (lock()) {
        while (USBD_CDC_isTxBusy(USBD_CDC_CH)) {
            if ((DriverUsb_GetTime() - startMs) >= kTimeoutMs
                    || !isSerialPortOpen()) {
                break;
            }
            DriverUsb_Delay(5);
        }

        if (isSerialPortOpen()) {
            status = USBD_CDC_Transmit(USBD_CDC_CH, pData, size);
        }

        if (status && blocking) {
            startMs = DriverUsb_GetTime();
            while (USBD_CDC_isTxBusy(USBD_CDC_CH)) {
                if ((DriverUsb_GetTime() - startMs) >= kTimeoutMs
                        || !isSerialPortOpen()) {
                    status = false;
                    break;
                }
                DriverUsb_Delay(5);
            }
        }

        unlock();
    }

    return status;
}

bool UsbDevice::receive(uint8_t *pData, uint32_t size, uint32_t &br,
        uint32_t timeout)
{
    assert(pData != nullptr);
    br = mStreamBuffer.receive(pData, size, timeout);
    return br > 0;
}

void UsbDevice::portStateCb(uint8_t ch, bool state)
{
    (void)ch;
    // Deliberately does not log. This runs in the USB interrupt, on the MSP
    // stack every handler shares, and the logger reaches _vsnprintf - about
    // 420 bytes of it. notifyISR hands the same state to the task side,
    // which logs it there, so nothing is lost.

    if (state) {
        GetInstance().mObserver.notifyISR(&Callback::onUsbOpenVCP);
    } else {
        GetInstance().mObserver.notifyISR(&Callback::onUsbCloseVCP);
    }
}

void UsbDevice::receiveCb(uint8_t ch, uint8_t *buf, uint32_t len)
{
    (void)ch;
    uint32_t sent = GetInstance().mStreamBuffer.sendFromISR(buf, len);
    if (sent != len) {
        LOG_ERROR("No space in RX queue\n");
    }
}

#endif // (USB_CDC_ENABLE == 1)

/* --------------------------------------------------------------------------
 * MSC callbacks
 * -------------------------------------------------------------------------- */

#if (USB_MSC_ENABLE == 1)

USBD_StatusTypeDef UsbDevice::MSC_Status(uint8_t drv)
{
    return flash_diskio_status(drv) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef UsbDevice::MSC_Read(uint8_t drv, uint8_t *buf,
        uint32_t blk_addr, uint16_t blk_len)
{
    return flash_diskio_read(drv, buf, blk_addr, blk_len) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef UsbDevice::MSC_Write(uint8_t drv, const uint8_t *buf,
        uint32_t blk_addr, uint16_t blk_len)
{
    return flash_diskio_write(drv, buf, blk_addr, blk_len) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef UsbDevice::MSC_BlockSize(uint8_t drv, uint32_t *block_size)
{
    return flash_diskio_sector_size(drv, block_size) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef UsbDevice::MSC_BlockNum(uint8_t drv, uint32_t *block_num)
{
    return flash_diskio_sector_count(drv, block_num) ? USBD_OK : USBD_FAIL;
}

#endif // (USB_MSC_ENABLE == 1)
