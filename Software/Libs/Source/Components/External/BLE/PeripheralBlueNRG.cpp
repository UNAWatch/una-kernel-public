/**
 ******************************************************************************
 * @file    PeripheralBlueNRG.cpp
 * @brief   BlueNRG Peripheral component.
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "PeripheralBlueNRG.hpp"

#define LOG_MODULE_PRX      "PeripheralBlueNRG"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include <cstring>
#include <cassert>

// Macros to convert units
#define BLE_INTERVAL_TO_MS( interval )      (interval * 1.25)
#define BLE_MS_TO_ADV_INTERVAL( ms )        ((ms * 1000) / 625)

namespace Ble
{

/**
 * @brief   Retry helper for ACI/HCI commands that may temporarily fail
 *          with a busy chip (e.g. 0xFF) during connection/security events.
 *          Must be called with mCore mutex already held.
 * @param   attempts:  total number of attempts (must be >= 1).
 * @param   delayMs:   delay between attempts in milliseconds.
 * @param   fn:        callable returning una_ble_status.
 * @return  Last una_ble_status returned by fn.
 */
template<typename Fn>
static una_ble_status aciRetry(uint32_t attempts, uint32_t delayMs, Fn fn)
{
    una_ble_status status = UNA_BLE_STATUS_FAILED;
    uint32_t attempt = 0;
    while (attempts--) {
        status = fn();
        if (status == UNA_BLE_STATUS_SUCCESS) {
            break;
        }
        if (attempts > 0) {
            LOG_DEBUG("aciRetry: attempt %lu failed [0x%02X], retrying in %lu ms\n",
                    attempt, status, delayMs);
            OS::Delay(delayMs);
        }
        attempt++;
    }
    return status;
}

PeripheralBlueNRG *PeripheralBlueNRG::pInstance;

/**
 * @brief  Convert State enum value to a human-readable string.
 * @param  state: State value to convert.
 * @return Null-terminated string literal describing the state.
 */
static const char* stateToString(PeripheralBlueNRG::State state)
{
    switch (state) {
        case PeripheralBlueNRG::State::NO_INIT:     return "NO_INIT";
        case PeripheralBlueNRG::State::INITIALIZED: return "INITIALIZED";
        case PeripheralBlueNRG::State::ENABLED:     return "ENABLED";
        case PeripheralBlueNRG::State::ADVERTISE:   return "ADVERTISE";
        case PeripheralBlueNRG::State::CONNECTED:   return "CONNECTED";
        default:                                    return "UNKNOWN";
    }
}

PeripheralBlueNRG::PeripheralBlueNRG(CoreBlueNRG &core) : mCore(core)
{
    pInstance = this;

    // Own the watch's peripheral (phone) link: the router delivers slave-role
    // connections and their GATT-client events here.
    bleEventRouter().setRoleClient(LinkRole::Peripheral, this);

    // Reconcile our cached state when the coprocessor resets itself.
    mCore.setResetHandler(this);

    // Bind this link's ports to the process-wide GATT-client coordinator and
    // pace its discovery. Which service clients run is decided by their
    // consumers (which register on the coordinator), not by this owner.
    mGattScheduler.init("GattDisc", &blePeripheralClients(), &mCore);
    blePeripheralClients().bind(mGattOps, mGattScheduler);
    blePeripheralClients().setLinkActivity(this);
    blePeripheralClients().setBleThreadWork(&mCore);
}

PeripheralBlueNRG::~PeripheralBlueNRG()
{
    bleEventRouter().setRoleClient(LinkRole::Peripheral, nullptr);
    mCore.setResetHandler(nullptr);
    deinit();
    pInstance = nullptr;
}

bool PeripheralBlueNRG::init()
{
    bool result = false;

    mCore.lock();

    if (mState >= State::INITIALIZED) {
        mCore.release();
        return true;
    }

    result = mCore.init();

    if (result) {
        mState = State::INITIALIZED;
        LOG_DEBUG("Initialized\n");
    }

    mCore.release();

    return result;
}

bool PeripheralBlueNRG::deinit()
{
    mCore.lock();

    if (mState < State::INITIALIZED) {
        mCore.release();
        return true;
    }

    // Clear context
    mConnection.clear();

    mState = State::NO_INIT;
    LOG_DEBUG("Deinitialized\n");

    mCore.deinit();

    mCore.release();

    return true;
}

bool PeripheralBlueNRG::enable()
{
    una_ble_status status { };
    uint16_t hService, hDevNameChar, hAppearanceChar;
    uint8_t bondedDeviceNum = 0;
    una_ble_device_address bondedDeviceEntry[skBondedListSize] {};

    mCore.lock();

    if (mState < State::INITIALIZED) {
        LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        mCore.release();
        return false;
    }

    if (mState > State::INITIALIZED) {
        mCore.release();
        return true;    // Already enabled
    }

    // Base initialization
    if (!mCore.enable()) {
        mCore.release();
        return false;
    }

    // Specific initialization for peripheral device

    // Initialize the GATT layer for server
    status = una_ble_gatt_init();
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gatt_init() failed [0x%02X]\n", status);
        goto exit;
    }

    // Initialize the GAP layer
    status = una_ble_gap_init(UNA_BLE_GAP_PERIPHERAL_ROLE | UNA_BLE_GAP_CENTRAL_ROLE, 0x02, strlen(mName),
            &hService, &hDevNameChar, &hAppearanceChar);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_init() failed [0x%02X]\n", status);
        goto exit;
    }

    // Prefer Data Length Extension on every connection: the controller uses
    // this suggested default as the initial max TX for new links and
    // auto-initiates the DLE procedure, so ~200-byte notifications ride in one
    // LL PDU instead of ~8 fragments. An explicit per-link request is also made
    // post-encryption (una_ble_hci_encryption_change_event). Non-fatal.
    status = una_ble_hci_le_write_suggested_default_data_length(251, 2120);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_WARNING("una_ble_hci_le_write_suggested_default_data_length() failed [0x%02X]\n", status);
    }

    // Update device name
    status = una_ble_gatt_update_char_value(hService, hDevNameChar, 0,
            strlen(mName), (uint8_t*) mName);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gatt_update_char_value() failed [0x%02X]\n", status);
        goto exit;
    }

    // Update device appearance
    status = una_ble_gatt_update_char_value(hService, hAppearanceChar, 0,
            sizeof(kDeviceAppearance), (uint8_t*) &kDeviceAppearance);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gatt_update_char_value() failed [0x%02X]\n", status);
        goto exit;
    }

    // Set IO capability
    status = una_ble_gap_set_io_capability(UNA_BLE_IO_CAP_DISPLAY_ONLY);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_set_io_capability() failed [0x%02X]\n", status);
        goto exit;
    }

    // Set the authentication requirements
    status = una_ble_gap_set_authentication_requirement(
            UNA_BLE_BONDING,/*bonding is enabled */
            UNA_BLE_MITM_PROTECTION_REQUIRED,
            UNA_BLE_SC_IS_MANDATORY,/*Secure connection supported */
            UNA_BLE_KEYPRESS_IS_NOT_SUPPORTED,
            7, /* Min encryption key size */
            16, /* Max encryption key size */
            UNA_BLE_DONOT_USE_FIXED_PIN_FOR_PAIRING, /* fixed pin is not used*/
            0, /* fixed pin */
            0x01 /* Random (static) Identity Address */);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_set_authentication_requirement() failed [0x%02X]\n", status);
        goto exit;
    }

    // Get bonded devices
    status = una_ble_gap_get_bonded_devices(&bondedDeviceNum, bondedDeviceEntry);
    if (status == UNA_BLE_STATUS_SUCCESS) {
        if (bondedDeviceNum > 0) {
            for (uint32_t i = 0; i < bondedDeviceNum; i++) {
                Ble::Address addr {};
                addr.setAddress(bondedDeviceEntry[i].address);

                LOG_DEBUG("Bonded. Address type %d: %s\n",
                        bondedDeviceEntry[i].address_type,
                        addr.toString(tbuff));
            }
        } else {
            LOG_DEBUG("No bonded devices\n");
        }
    } else {
        LOG_ERROR("una_ble_gap_get_bonded_devices() failed [0x%02X]\n", status);
        goto exit;
    }

    // Add services
    if(!initServices()) {
        LOG_ERROR("Add services failed\n");
        goto exit;
    }

    mState = State::ENABLED;
    LOG_DEBUG("Enabled\n");

    // Notify observers
    mObserver.notify(&Interface::IBle::StatusCallback::onEnable);

exit:
    if (mState != State::ENABLED) {
        mCore.disable();    // Clear core state
    }
    mCore.release();

    return mState == State::ENABLED;
}

bool PeripheralBlueNRG::disable()
{
    mCore.lock();

    if (mState < State::ENABLED) {
        mCore.release();
        return true;    // Already disabled
    }

    // Clear context
    mConnection.clear();

    mState = State::INITIALIZED;
    LOG_DEBUG("Disabled\n");

    mCore.disable();

    mCore.release();

    return false;
}

bool PeripheralBlueNRG::isEnabled()
{
    return mState >= State::ENABLED;
}

bool PeripheralBlueNRG::isAlive()
{
    static constexpr uint32_t kPingAttempts = 3U;
    static constexpr uint32_t kPingDelayMs  = 10U;

    if (mState < State::ENABLED) {
        return false;
    }

    mCore.lock();

    bool alive = false;
    for (uint32_t i = 0; i < kPingAttempts; ++i) {
        alive = mCore.ping();
        if (alive) {
            break;
        }
        if (i + 1 < kPingAttempts) {
            LOG_DEBUG("ping attempt %lu failed, retrying in %lu ms\n",
                    i, kPingDelayMs);
            mCore.release();
            OS::Delay(kPingDelayMs);
            mCore.lock();
        }
    }

    mCore.release();

    return alive;
}

void PeripheralBlueNRG::setName(const char* name)
{
    memset(mName, 0, sizeof(mName));
    strncpy(mName, name, sizeof(mName) - 1);
}

const char* PeripheralBlueNRG::getName() const
{
    return mName;
}

bool PeripheralBlueNRG::startTxFCC(uint8_t channel)
{
    disable();
    return mCore.startTxFCC(channel);
}

bool PeripheralBlueNRG::startRxFCC(uint8_t channel)
{
    disable();
    return mCore.startRxFCC(channel);
}

bool PeripheralBlueNRG::stopTxFCC(uint32_t &numPackets)
{
    return mCore.stopTxFCC(numPackets);
}

bool PeripheralBlueNRG::stopRxFCC(uint32_t &numPackets)
{
    return mCore.stopRxFCC(numPackets);
}

bool PeripheralBlueNRG::runRxFCCScan(const uint8_t *channels, uint8_t count,
        uint32_t dwellMs, RxFCCScanResult *results)
{
    disable();
    return mCore.runRxFCCScan(channels, count, dwellMs, results);
}

ServiceHandle PeripheralBlueNRG::addService(const UUID &uuid)
{
    ServiceHandle hsrv = nullptr;

    mCore.lock();

    hsrv = findService(uuid);
    if (hsrv == nullptr) {
        for (uint32_t i = 0; i < skServicesMax; ++i) {
            if (mServiceHolder[i].uuid.isValid() == false) {
                mServiceHolder[i].uuid = uuid;
                mServiceHolder[i].handlesNum += 1;
                hsrv = &mServiceHolder[i];
                break;
            }
        }
    } else {
        LOG_WARNING("Service has already been added. UUID %s\n", uuid.toString(tbuff));
    }

    if (hsrv == nullptr) {
        LOG_ERROR("Maximum number of services %u\n", skServicesMax);
    }
    mCore.release();
    return hsrv;
}

bool PeripheralBlueNRG::delService(ServiceHandle handle)
{
    bool status = false;

    mCore.lock();

    for (uint32_t i = 0; i < skServicesMax; i++) {
        if (handle == &mServiceHolder[i]) {
            // Delete all characteristics related to this service
            for (uint32_t j = 0; j < skCharacteristicsMax; j++) {
                if (mCharacteristicHolder[j].service == handle) {
                    mCharacteristicHolder[j].clear();
                }
            }
            mServiceHolder[i].clear();
            status = true;
            break;
        }
    }

    mCore.release();

    return status;
}

CharacteristicHandle PeripheralBlueNRG::addCharacteristic(
        ServiceHandle srvHandle, const UUID &uuid, uint16_t maxlen,
        uint8_t properties)
{
    ServiceHolder *hsrv = nullptr;
    CharacteristicHandle hchr = nullptr;

    mCore.lock();

    // Check if service is present
    hsrv = findService(srvHandle);

    if (hsrv != nullptr) {

        // Check if characteristic is already present
        hchr = findCharacteristic(hsrv, uuid);
        if (hchr == nullptr) {
            for (uint32_t i = 0; i < skCharacteristicsMax; ++i) {
                if (mCharacteristicHolder[i].uuid.isValid() == false) {
                    mCharacteristicHolder[i].uuid = uuid;
                    mCharacteristicHolder[i].len = maxlen;
                    mCharacteristicHolder[i].properties = properties;
                    mCharacteristicHolder[i].service = hsrv;
                    hchr = &mCharacteristicHolder[i];

                    // Update handles number in service
                    uint8_t hnum = 2;
                    if (mCharacteristicHolder[i].properties & (CharProp::NOTIFY | CharProp::INDICATE)) {
                        hnum += 1;
                    }
                    mCharacteristicHolder[i].service->handlesNum += hnum;

                    break;
                }
            }
        } else {
            LOG_WARNING("Characteristic has already been added. UUID %s\n", uuid.toString(tbuff));
        }

        if (hchr == nullptr) {
            LOG_ERROR("Maximum number of characteristic %u\n", skCharacteristicsMax);
        }
    } else {
        LOG_ERROR("UUID %s. Services not found\n", uuid.toString(tbuff));
    }

    mCore.release();
    return hchr;
}

bool PeripheralBlueNRG::delCharacteristic(CharacteristicHandle handle)
{
    bool status = false;

    mCore.lock();

    for (uint32_t i = 0; i < skCharacteristicsMax; ++i) {
        if (handle == &mCharacteristicHolder[i]) {
            // Update handles number in service
            uint8_t hnum = 2;
            if (mCharacteristicHolder[i].properties & (CharProp::NOTIFY | CharProp::INDICATE)) {
                hnum += 1;
            }
            mCharacteristicHolder[i].service->handlesNum -= hnum;

            mCharacteristicHolder[i].clear();
            status = true;
            break;
        }
    }

    mCore.release();

    return status;
}

ServiceHandle PeripheralBlueNRG::getServiceByUUID(const UUID &uuid)
{
    mCore.lock();
    ServiceHolder* hsrv = findService(uuid);
    mCore.release();
    return hsrv;
}

UUID PeripheralBlueNRG::getServiceUUID(ServiceHandle handle)
{
    UUID uuid {};

    mCore.lock();
    ServiceHolder* hsrv = findService(handle);
    if (hsrv) {
        uuid = hsrv->uuid;
    }
    mCore.release();
    return uuid;
}

CharacteristicHandle PeripheralBlueNRG::getCharacteristicByUUID(
        ServiceHandle srvHandle, const UUID &uuid)
{
    CharacteristicHolder *hchr = nullptr;

    mCore.lock();
    ServiceHolder* hsrv = findService(srvHandle);
    if (hsrv) {
        hchr = findCharacteristic(hsrv, uuid);
    }
    mCore.release();
    return hchr;
}

UUID PeripheralBlueNRG::getCharacteristicUUID(CharacteristicHandle handle)
{
    UUID uuid {};

    mCore.lock();
    CharacteristicHolder *hchr = findCharacteristic(handle);
    if (hchr) {
        uuid = hchr->uuid;
    }
    mCore.release();
    return uuid;
}

uint32_t PeripheralBlueNRG::getMaxCharacteristicLen()
{
    // The characteristic can hold 220 bytes, but a notification/indication
    // cannot exceed the negotiated ATT payload (MTU - 3) -- a larger value is
    // silently TRUNCATED by the controller. Windowed FTS reads send ~204-byte
    // chunks + a 16-byte header = 220 > MTU-3 (217 at MTU 220), so each
    // notification lost 3 bytes -> silent read corruption. Cap to MTU-3 once
    // negotiated; fall back to 220 before the MTU exchange (e.g. at
    // characteristic-creation time, when mtu is still 0).
    uint32_t cap = (mConnection.mtu > 3) ? (uint32_t)(mConnection.mtu - 3) : 220u;
    return (cap < 220u) ? cap : 220u;
}

bool PeripheralBlueNRG::setCharacteristicValue(Ble::CharacteristicHandle handle,
        const uint8_t *pData, uint8_t len)
{
    // A full controller TX pool returns 0x64 (UNA_BLE_STATUS_INSUFFICIENT_RESOURCES).
    // The coprocessor drains it autonomously as the radio transmits queued
    // packets, so we poll-and-retry against a time budget instead of giving up
    // after a few attempts (which truncated windowed FTS bursts). Polling at a
    // short interval lets a burst resume within ~one connection event of a
    // buffer freeing, instead of the old 50 ms granularity that throttled
    // throughput to roughly one packet per ~2.5 connection events.
    //
    // IMPORTANT: this can run on the HCI service thread (FTS onWrite ->
    // readHandler -> sendResponce), which is also the thread that dispatches
    // una_ble_gatt_tx_pool_available_event. It must therefore NOT block waiting on
    // that event -- doing so would deadlock (the thread could never return to
    // una_ble_hci_user_evt_proc() to service the event it is waiting for). Polling the
    // command result is the deadlock-safe mechanism. mCore is released between
    // attempts so that when this is instead called from another thread, the HCI
    // thread can run and drain the pool. The characteristic is looked up on
    // every attempt so the handle stays valid if the mutex was released between
    // iterations.
    static constexpr uint32_t skPollMs = 5;

    // Indications need a peer ACK before a buffer frees, so allow longer.
    uint32_t budgetMs = 250;
    mCore.lock();
    CharacteristicHolder *hchr = findCharacteristic(handle);
    if (hchr && (hchr->properties & CharProp::INDICATE)) {
        budgetMs = 500;
    }
    mCore.release();

    una_ble_status status = UNA_BLE_STATUS_FAILED;
    const uint32_t startMs = OS::GetTimeMs();
    for (;;) {
        mCore.lock();
        hchr = findCharacteristic(handle);
        if (!hchr) {
            mCore.release();
            break;
        }
        status = una_ble_gatt_update_char_value(hchr->service->handle,
                hchr->handle, 0, len, (uint8_t*) pData);
        mCore.release();

        if (status == UNA_BLE_STATUS_SUCCESS) {
            noteTraffic();
            break;
        }
        if (OS::GetTimeMs() - startMs >= budgetMs) {
            break;
        }
        OS::Delay(skPollMs);
    }

    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_WARNING("una_ble_gatt_update_char_value() failed [0x%02X], UUID %s\n",
                status, hchr ? hchr->uuid.toString(tbuff) : "?");
    }

    return status == UNA_BLE_STATUS_SUCCESS;
}

bool PeripheralBlueNRG::trySetCharacteristicValue(Ble::CharacteristicHandle handle,
        const uint8_t *pData, uint8_t len, bool *pPoolFull)
{
    // Single non-blocking attempt. On a full TX pool (0x64) this returns false
    // immediately so the caller (FTS windowed pump) can park and resume from
    // una_ble_gatt_tx_pool_available_event -> onTxReady(). Must stay non-blocking:
    // it can run on the HCI event thread, which also dispatches that event, so
    // blocking here would deadlock.
    mCore.lock();
    CharacteristicHolder *hchr = findCharacteristic(handle);
    una_ble_status status = UNA_BLE_STATUS_FAILED;
    if (hchr) {
        status = una_ble_gatt_update_char_value(hchr->service->handle,
                hchr->handle, 0, len, (uint8_t*) pData);
    }
    mCore.release();

    if (status == UNA_BLE_STATUS_SUCCESS) {
        noteTraffic();
    } else if (pPoolFull) {
        // Distinguish "pool full" (a tx-pool-available event WILL follow, so
        // the caller may park and retry from onTxReady) from any other failure
        // (no event is coming -- the caller must recover, not park forever).
        *pPoolFull = (status == UNA_BLE_STATUS_INSUFFICIENT_RESOURCES);
    }
    return status == UNA_BLE_STATUS_SUCCESS;
}

void PeripheralBlueNRG::attachStatusCallback(
        Interface::IBle::StatusCallback *pCallback)
{
    mCore.lock();
    mObserver.add(pCallback);
    mCore.release();
}

void PeripheralBlueNRG::detachStatusCallback(
        Interface::IBle::StatusCallback *pCallback)
{
    mCore.lock();
    mObserver.del(pCallback);
    mCore.release();
}

void PeripheralBlueNRG::attachCharacteristicCallback(
        Ble::CharacteristicHandle handle,
        Interface::IBle::CharacteristicCallback *pCallback)
{
    mCore.lock();
    CharacteristicHolder *hchr = findCharacteristic(handle);
    if (hchr) {
        hchr->callback = pCallback;
    }
    mCore.release();
}

bool PeripheralBlueNRG::pair()
{
    bool status { };

    mCore.lock();

    if (mState != State::ENABLED && mState != State::ADVERTISE) {
        mCore.release();
        LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        return false;
    }

    if (mState == State::ADVERTISE) {
        stopAdv();
    }

    status = startAdv(true);

    mCore.release();
    return status;
}

bool PeripheralBlueNRG::unpair()
{
    mCore.lock();

    if (mState < State::ENABLED) {
        mCore.release();
        LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        return false;
    }


    if (mState == State::ADVERTISE) {
        stopAdv();
    }

    if (mState == State::CONNECTED) {
        terminate();
    }


    // Delete bonded devices
    una_ble_status status = una_ble_gap_clear_security_db();
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_clear_security_db() failed [0x%02X]\n", status);
    } else {
        LOG_DEBUG("Bonded devices have been removed\n");
    }

    mCore.release();
    return status == UNA_BLE_STATUS_SUCCESS;

}

bool PeripheralBlueNRG::isBonded()
{
    mCore.lock();

    if (mState < State::ENABLED) {
        mCore.release();
        LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        return false;
    }

    bool status = isBondedAtLeastOne();

    mCore.release();
    return status;
}

uint8_t PeripheralBlueNRG::getBondedCount()
{
    mCore.lock();

    if (mState < State::ENABLED) {
        mCore.release();
        LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        return 0;
    }

    uint8_t count = countBonded();

    mCore.release();
    return count;
}

bool PeripheralBlueNRG::startAdvertising()
{
    bool status { };

    mCore.lock();

    if (mState != State::ENABLED && mState != State::ADVERTISE) {
        mCore.release();
        LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        return false;
    }

    if (mState == State::ADVERTISE) {
        stopAdv();
    }

    status = startAdv();

    mCore.release();
    return status;
}

bool PeripheralBlueNRG::stopAdvertising()
{
    bool status { };

    mCore.lock();

    if (mState != State::ADVERTISE) {
        mCore.release();
        if (mState > State::ADVERTISE) {
            LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
        }
        return mState < State::ADVERTISE;   // true if already stopped, false if connected
    }

    status = stopAdv();

    mCore.release();
    return status;
}

bool PeripheralBlueNRG::isAdvertise()
{
    return mState >= State::ADVERTISE;
}

bool PeripheralBlueNRG::disconnect()
{
    bool status { };

    mCore.lock();

    if (mState != State::CONNECTED) {
        mCore.release();
        if (mState > State::CONNECTED) {
            // Unreachable with the current State enum, but guard for future states
            LOG_ERROR("Wrong state [%s]\n", stateToString(mState));
            return false;
        }
        return true;    // Already disconnected
    }

    status = terminate();

    mCore.release();
    return status;
}

bool PeripheralBlueNRG::isConnected()
{
    return mState == State::CONNECTED;
}

bool PeripheralBlueNRG::initServices()
{
    una_ble_status status { };

    for (uint32_t i = 0; i < skServicesMax; i++) {
        if (mServiceHolder[i].uuid.isValid()) {
            // Get UUID
            const UUID &srvuuid = mServiceHolder[i].uuid;

            una_ble_uuid blueSrvUuid { };
            uint8_t blueSrvUuidType = UNA_BLE_UUID_TYPE_16;

            if (srvuuid.is128()) {
                memcpy(&blueSrvUuid.uuid_128, srvuuid.get128(), UUID::skLen128);
                blueSrvUuidType = UNA_BLE_UUID_TYPE_128;
            } else {
                blueSrvUuid.uuid_16 = srvuuid.get16();
            }

            // Add service
            status = una_ble_gatt_add_service(blueSrvUuidType, &blueSrvUuid,
                    UNA_BLE_PRIMARY_SERVICE, mServiceHolder[i].handlesNum,
                    &mServiceHolder[i].handle);

            if (status != UNA_BLE_STATUS_SUCCESS) {
                LOG_ERROR("una_ble_gatt_add_service() failed [0x%02X].\nUUID %s\n",
                                            status, srvuuid.toString(tbuff));
                return false;
            }
            LOG_DEBUG("Service added. UUID %s\n", srvuuid.toString(tbuff));

            // Add characteristics
            for (uint32_t j = 0; j < skCharacteristicsMax; j++) {
                if (mCharacteristicHolder[j].uuid.isValid()
                        && mCharacteristicHolder[j].service == &mServiceHolder[i])
                {
                    // Get characteristic UUID
                    const UUID &chruuid = mCharacteristicHolder[j].uuid;

                    una_ble_uuid blueChrUuid { };
                    uint8_t blueChrUuidType = UNA_BLE_UUID_TYPE_16;

                    if (chruuid.is128()) {
                        memcpy(&blueChrUuid.uuid_128, chruuid.get128(),
                                UUID::skLen128);
                        blueChrUuidType = UNA_BLE_UUID_TYPE_128;
                    } else {
                        blueChrUuid.uuid_16 = chruuid.get16();
                    }

                    // Get properties
                    uint8_t props = mCharacteristicHolder[j].properties;

                    // Set permissions
                    uint8_t permissions = (UNA_BLE_ATTR_PERMISSION_ENCRY_READ
                                        | UNA_BLE_ATTR_PERMISSION_ENCRY_WRITE);

                    // Set notification events
                    uint8_t evt = UNA_BLE_GATT_DONT_NOTIFY_EVENTS;
                    if (props & (CharProp::WRITE | CharProp::WRITE_WO_RESP | CharProp::SIGNED_WRITE)) {
                        // The application will be notified when a client writes to this attribute.
                        evt |= UNA_BLE_GATT_NOTIFY_ATTRIBUTE_WRITE;
                    }

                    if (props & CharProp::READ) {
                        // The application will be notified when a read request of any type is
                        // received for this attribute.
                        // This is used so that the user can update the data if necessary.
                        // Read confirmation will be automatic after the callback is called.
                        evt |= UNA_BLE_GATT_NOTIFY_READ_REQ_AND_WAIT_FOR_APPL_RESP;
                    }

                    status = una_ble_gatt_add_char(mServiceHolder[i].handle,
                            blueChrUuidType, &blueChrUuid,
                            mCharacteristicHolder[j].len, props, permissions,
                            evt, 16, 1, &mCharacteristicHolder[j].handle);

                    if (status != UNA_BLE_STATUS_SUCCESS) {
                        LOG_ERROR("una_ble_gatt_add_char() failed [0x%02X].\nUUID %s\n",
                                                        status, chruuid.toString(tbuff));
                        return false;
                    }
                    LOG_DEBUG("Characteristic added. UUID %s\n", chruuid.toString(tbuff));
                }
            }
        }
    }

    return true;
}

bool PeripheralBlueNRG::startAdv(bool pair)
{
    una_ble_status status { };

    // Advertise interval
    const uint16_t periodMin = pair ? 50 : 1000;
    const uint16_t periodMax = pair ? 60 : 1200;
    const uint16_t kAdvIntervalMin = BLE_MS_TO_ADV_INTERVAL(periodMin);
    const uint16_t kAdvIntervalMax = BLE_MS_TO_ADV_INTERVAL(periodMax);
    // Connection intervals
    // 6 x 1.25ms = 7.5ms (Supported by Android).
    // IOS supports minimum connection interval of 20ms (20 / 1.25 = 16)
    const uint16_t kL2capConnIntervalMin = 6;
    const uint16_t kL2capConnIntervalMax = kL2capConnIntervalMin * 10;


    uint8_t nameLen = strlen(mName);
    uint8_t nameField[2 + nameLen] {};
    nameField[0] = 1 + nameLen;                 // Len
    nameField[1] = UNA_BLE_AD_TYPE_COMPLETE_LOCAL_NAME; // Type
    memcpy(&nameField[2], mName, nameLen);      // Data

    uint8_t manufLen = strlen(kManufMagic);
    uint8_t manufField[4 + manufLen] {};
    manufField[0] = 3 + manufLen;                       // Len
    manufField[1] = UNA_BLE_AD_TYPE_MANUFACTURER_SPECIFIC_DATA; // Type
    memcpy(&manufField[2], &kManufId, 2);               // Data
    memcpy(&manufField[4], kManufMagic, manufLen);

    if (pair) {

        // Set scan response
        uint8_t scanResponseData[sizeof(nameField) + sizeof(manufField)] {};
        memcpy(&scanResponseData[0], nameField, sizeof(nameField));
        memcpy(&scanResponseData[sizeof(nameField)], manufField, sizeof(manufField));
        status = aciRetry(3, 50, [&]{
            return una_ble_hci_le_set_scan_response_data(sizeof(scanResponseData),
                    (uint8_t*)scanResponseData);
        });
        if (status != UNA_BLE_STATUS_SUCCESS) {
            LOG_ERROR("una_ble_hci_le_set_scan_response_data() failed [0x%02X]\n", status);
            return false;
        }

        status = aciRetry(3, 50, [&]{
            return una_ble_gap_set_discoverable(UNA_BLE_ADV_IND,
                    kAdvIntervalMin, kAdvIntervalMax,
                    UNA_BLE_RESOLVABLE_PRIVATE_ADDR,
                    UNA_BLE_NO_WHITE_LIST_USE,
                    sizeof(nameField) - 1, (uint8_t*)&nameField[1], // It is not add for some reason
                    0, NULL, // advertise 0 services, no service list
                    kL2capConnIntervalMin, kL2capConnIntervalMax);
        });
        if (status != UNA_BLE_STATUS_SUCCESS) {
            LOG_ERROR("una_ble_gap_set_discoverable() failed [0x%02X]\n", status);
        } else {
            LOG_INFO("Pairing started\n");
            mState = State::ADVERTISE;
        }

        una_ble_gap_delete_ad_type(UNA_BLE_AD_TYPE_TX_POWER_LEVEL);
        una_ble_gap_delete_ad_type(UNA_BLE_AD_TYPE_SLAVE_CONN_INTERVAL);

    } else if (isBondedAtLeastOne()) {
        // Update white list every time before undirected advertisement
        updateWhitelist();

        // Set scan response
        uint8_t scanResponseData[sizeof(nameField) + sizeof(manufField)] {};
        memcpy(&scanResponseData[0], nameField, sizeof(nameField));
        memcpy(&scanResponseData[sizeof(nameField)], manufField, sizeof(manufField));
        status = aciRetry(3, 50, [&]{
            return una_ble_hci_le_set_scan_response_data(sizeof(scanResponseData),
                    (uint8_t*)scanResponseData);
        });
        if (status != UNA_BLE_STATUS_SUCCESS) {
            LOG_ERROR("una_ble_hci_le_set_scan_response_data() failed [0x%02X]\n", status);
            return false;
        }

        status = aciRetry(3, 50, [&]{
            return una_ble_gap_set_undirected_connectable(
                    kAdvIntervalMin, kAdvIntervalMax,
                    UNA_BLE_RESOLVABLE_PRIVATE_ADDR, UNA_BLE_WHITE_LIST_FOR_ALL);
        });
        if (status != UNA_BLE_STATUS_SUCCESS) {
            LOG_ERROR("una_ble_gap_set_undirected_connectable() failed [0x%02X]\n", status);
        } else {
            LOG_INFO("Advertising started\n");
            mState = State::ADVERTISE;
        }
    } else {
        LOG_WARNING("Advertising not started. No bonded devices\n");
        return false;
    }

    return status == UNA_BLE_STATUS_SUCCESS;
}

bool PeripheralBlueNRG::stopAdv()
{
    una_ble_status status = aciRetry(3, 50, []{
        return una_ble_gap_set_non_discoverable();
    });

    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_set_non_discoverable() failed [0x%02X]\n", status);
    } else {
        LOG_INFO("Advertising stopped\n");
        mState = State::ENABLED;
    }

    return status == UNA_BLE_STATUS_SUCCESS;
}

bool PeripheralBlueNRG::terminate()
{
    una_ble_status status = aciRetry(3, 50, [this]{
        return una_ble_gap_terminate(mConnection.handle, 0x13);
    });

    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_terminate() failed [0x%02X]\n", status);
    } else {
        LOG_INFO("Connection terminated\n");
        mState = State::ENABLED;
    }

    return status == UNA_BLE_STATUS_SUCCESS;
}

bool PeripheralBlueNRG::updateWhitelist()
{
    uint8_t bondedDeviceNum = 0;
    una_ble_device_address bondedDeviceEntry[skBondedListSize] {};

    una_ble_status status = aciRetry(3, 50, [&]{
        return una_ble_gap_get_bonded_devices(&bondedDeviceNum, bondedDeviceEntry);
    });

    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_get_bonded_devices() failed [0x%02X]\n", status);
        return false;
    }

    status = una_ble_gap_configure_whitelist();
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_configure_whitelist() failed [0x%02X]\n", status);
        return false;
    }

    status = una_ble_gap_add_devices_to_resolving_list(bondedDeviceNum,
            (una_ble_device_address*) bondedDeviceEntry, 1);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_add_devices_to_resolving_list() failed [0x%02X]\n", status);
        return false;
    }

    for (uint32_t i = 0; i < bondedDeviceNum; i++) {
        Ble::Address addr {};
        addr.setAddress(bondedDeviceEntry[i].address);
        LOG_DEBUG("Address type %d: %s\n",
                bondedDeviceEntry[i].address_type,
                addr.toString(tbuff));
    }

    return true;
}

bool PeripheralBlueNRG::isBondedAtLeastOne()
{
    return countBonded() > 0;
}

uint8_t PeripheralBlueNRG::countBonded()
{
    uint8_t bondedDeviceNum = 0;
    una_ble_device_address bondedDeviceEntry[skBondedListSize] {};

    una_ble_status status = aciRetry(3, 50, [&]{
        return una_ble_gap_get_bonded_devices(&bondedDeviceNum, bondedDeviceEntry);
    });

    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_get_bonded_devices() failed [0x%02X]\n", status);
        return 0;
    }

    return bondedDeviceNum;
}

void PeripheralBlueNRG::onConnected(uint16_t connHandle, uint8_t addrType,
        const uint8_t addr[6], uint16_t connInterval, uint16_t connLatency,
        uint16_t supTimeout)
{
    mConnection.handle = connHandle;
    mConnection.interval = connInterval;
    mConnection.latency = connLatency;
    mConnection.supervisionTimeout = supTimeout;
    mConnection.addr.setAddress(addr);
    mConnection.addr.setType(static_cast<Ble::Address::Type>(addrType));

    LOG_INFO("Connected to %s\n", mConnection.addr.toString(tbuff));

    // DLE is requested post-encryption (una_ble_hci_encryption_change_event) and via
    // the suggested default set at init -- requesting it here, pre-feature-
    // exchange, was accepted but never negotiated.

    mState = Ble::PeripheralBlueNRG::State::CONNECTED;

    mObserver.notify(&Interface::IBle::StatusCallback::onConnect, mConnection.addr);

    una_ble_status status {};

    // If device is not bonded
    // Next events:
    // una_ble_gap_addr_not_resolved_event
    // Security request as una_ble_gap_addr_not_resolved_event
    // may not be called
    status = una_ble_gap_slave_security_req(connHandle);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_slave_security_req() failed [0x%02X]\n", status);
    } else {
        LOG_DEBUG("Security request sent\n");
        // Next events:
        // una_ble_gap_slave_security_initiated_event
        // aci_gap_pass_key_req_even
    }

    // If device is bonded
    // Next events:
    // una_ble_gap_pairing_complete_event
    // una_ble_hci_encryption_change_event
    // una_ble_hci_encryption_key_refresh_complete_event (?)


}

void PeripheralBlueNRG::onDisconnected(uint16_t connHandle, uint8_t reason)
{
    (void)connHandle;

    mConnection.clear();

    LOG_INFO("Disconnected. Reason 0x%02X\n", reason);
    mState = Ble::PeripheralBlueNRG::State::ENABLED;

    blePeripheralClients().reset();

    // Restart advertisement
    startAdv();

    mObserver.notify(&Interface::IBle::StatusCallback::onDisconnect, reason);
}

void PeripheralBlueNRG::noteTraffic()
{
    mLastTrafficMs = OS::GetTimeMs();
}

bool PeripheralBlueNRG::isTransferQuiet(uint32_t quietMs) const
{
    // No traffic yet (mLastTrafficMs == 0) reads as quiet: a vacation that has
    // never seen a transfer must not be blocked forever.
    return (OS::GetTimeMs() - mLastTrafficMs) >= quietMs;
}

// --- GATT-client events for the phone link, forwarded to the coordinator ----
// Routed here by Ble::EventRouter (the phone link is bound to this owner). The
// coordinator dispatches them to the registered service clients (ANCS / AMS).

void PeripheralBlueNRG::onServiceGroupFound(uint16_t connHandle,
        uint16_t foundHandle, uint16_t endHandle)
{
    (void)connHandle;
    blePeripheralClients().onServiceGroupFound(foundHandle, endHandle);
}

void PeripheralBlueNRG::onCharDiscovered(uint16_t connHandle,
        const uint8_t *attrData, uint8_t len)
{
    (void)connHandle;
    blePeripheralClients().onCharDiscovered(attrData, len);
}

void PeripheralBlueNRG::onCharDescFound(uint16_t connHandle,
        uint16_t attrHandle, uint16_t uuid16)
{
    (void)connHandle;
    blePeripheralClients().onCharDescFound(attrHandle, uuid16);
}

void PeripheralBlueNRG::onNotification(uint16_t connHandle, uint16_t attrHandle,
        const uint8_t *data, uint8_t len)
{
    (void)connHandle;
    // Incoming client notifications are link traffic too: feed the transfer-quiet
    // gate so client setup (e.g. AMS entity registration) defers through a
    // notification burst instead of issuing writes the busy controller rejects.
    noteTraffic();
    blePeripheralClients().onNotification(attrHandle, data, len);
}

void PeripheralBlueNRG::onGattError(uint16_t connHandle, uint8_t reqOpcode,
        uint16_t attrHandle, uint8_t errorCode)
{
    (void)connHandle;
    blePeripheralClients().onGattError(reqOpcode, attrHandle, errorCode);
}

void PeripheralBlueNRG::onGattProcComplete(uint16_t connHandle, uint8_t errorCode)
{
    (void)connHandle;
    blePeripheralClients().onGattProcComplete(errorCode);
}

void PeripheralBlueNRG::onReset()
{
    // Called from the BlueNRG event thread (core mutex already held) when the
    // coprocessor reset itself. The chip is now blank, so we cannot touch it
    // here: just drop our cached state below ENABLED and clear any connection.
    // Backend::bleMonitorTimCallback then sees BLE as disabled and performs the
    // full enable()/startAdvertising() recovery on its next tick.
    if (mState < State::ENABLED) {
        return;
    }

    bool wasConnected = (mState == State::CONNECTED);

    mConnection.clear();
    mState = State::INITIALIZED;
    LOG_WARNING("Coprocessor reset: BLE state invalidated\n");

    // A running discovery session is stuck unless reset; clearing it lets the
    // coordinator restart cleanly on the next connection.
    blePeripheralClients().reset();

    if (wasConnected) {
        // HCI "Connection Terminated By Local Host" — surface the drop so the
        // GUI does not keep showing a stale connection.
        mObserver.notify(&Interface::IBle::StatusCallback::onDisconnect, 0x16);
    }
}

PeripheralBlueNRG::ServiceHolder* PeripheralBlueNRG::findService(
        ServiceHandle handle)
{
    if (handle == nullptr) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skServicesMax; ++i) {
        if (&mServiceHolder[i] == handle
                && mServiceHolder[i].uuid.isValid())
        {
            return &mServiceHolder[i];
        }
    }
    return nullptr;
}

PeripheralBlueNRG::ServiceHolder* PeripheralBlueNRG::findService(
        const UUID &uuid)
{
    if (!uuid.isValid()) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skServicesMax; ++i) {
        if (mServiceHolder[i].uuid.isValid()
                && mServiceHolder[i].uuid == uuid)
        {
            return &mServiceHolder[i];
        }
    }
    return nullptr;
}

PeripheralBlueNRG::ServiceHolder* PeripheralBlueNRG::findService(
        uint16_t handle)
{
    if (handle == 0) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skServicesMax; ++i) {
        if (mServiceHolder[i].uuid.isValid()
                && mServiceHolder[i].handle == handle)
        {
            return &mServiceHolder[i];
        }
    }

    return nullptr;
}

PeripheralBlueNRG::CharacteristicHolder* PeripheralBlueNRG::findCharacteristic(
        CharacteristicHandle handle)
{
    if (handle == nullptr) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skCharacteristicsMax; ++i) {
        if (&mCharacteristicHolder[i] == handle
                && mCharacteristicHolder[i].uuid.isValid())
        {
            return &mCharacteristicHolder[i];
        }
    }
    return nullptr;
}

PeripheralBlueNRG::CharacteristicHolder* PeripheralBlueNRG::findCharacteristic(
        ServiceHolder *hsrv, const UUID &uuid)
{
    if (hsrv == nullptr || !uuid.isValid()) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skCharacteristicsMax; ++i) {
        if (mCharacteristicHolder[i].service == hsrv
                && mCharacteristicHolder[i].uuid.isValid()
                && mCharacteristicHolder[i].uuid == uuid )
        {
            return &mCharacteristicHolder[i];
        }
    }
    return nullptr;
}

PeripheralBlueNRG::CharacteristicHolder* PeripheralBlueNRG::findCharacteristic(
        const UUID &uuid)
{
    if (!uuid.isValid()) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skCharacteristicsMax; ++i) {
        if (mCharacteristicHolder[i].uuid.isValid()
                && mCharacteristicHolder[i].uuid == uuid)
        {
            return &mCharacteristicHolder[i];
        }
    }
    return nullptr;
}

PeripheralBlueNRG::CharacteristicHolder* PeripheralBlueNRG::findSCharacteristic(
        uint16_t handle)
{
    if (handle == 0) {
        return nullptr;
    }

    for (uint32_t i = 0; i < skCharacteristicsMax; ++i) {
        if (mCharacteristicHolder[i].uuid.isValid()
                && mCharacteristicHolder[i].handle == handle)
        {
            return &mCharacteristicHolder[i];
        }
    }
    return nullptr;
}

PeripheralBlueNRG::CharacteristicHolder* PeripheralBlueNRG::findSCharacteristicByAttr(
        uint16_t attrHandle)
{
    if (attrHandle < 1) {
        return nullptr;
    }

    return findSCharacteristic((uint16_t)(attrHandle - 1));
}

PeripheralBlueNRG::CharacteristicHolder* PeripheralBlueNRG::findSCharacteristicByDesc(
        uint16_t descHandle)
{
    if (descHandle < 2) {
        return nullptr;
    }

    return findSCharacteristic((uint16_t)(descHandle - 2));
}

} /* namespace Ble */


extern "C" {

// una_ble_hci_le_connection_complete_event / una_ble_hci_le_enhanced_connection_complete_event
// are routed by role in BleEventDispatch.cpp (Ble::EventRouter).

void una_ble_hci_le_connection_update_complete_event(uint8_t Status,
        uint16_t Connection_Handle, uint16_t Conn_Interval,
        uint16_t Conn_Latency, uint16_t Supervision_Timeout)
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);
    Ble::PeripheralBlueNRG::ConnectionInfo &conn = instance->mConnection;

    // Log every status, for every handle. A connection update that COMPLETES
    // BUT FAILS is otherwise invisible -- and the failures that matter here
    // (0x84 NEW_INTERVAL_FAILED / 0x85 INTERVAL_TOO_LARGE, the anchor-period
    // slot rules) only ever surface this way. Absence of this line must not be
    // read as "the procedure never completed".
    LOG_INFO("Conn update h=0x%04X status=0x%02X interval %u (%u ms) "
             "latency %u timeout %u (%u ms)\n",
            Connection_Handle, Status,
            (unsigned)Conn_Interval, (unsigned)((Conn_Interval * 5u) / 4u),
            (unsigned)Conn_Latency,
            (unsigned)Supervision_Timeout,
            (unsigned)(Supervision_Timeout * 10u));

    if (Status == UNA_BLE_STATUS_SUCCESS && Connection_Handle == conn.handle) {
        // mConnection describes the PHONE, so the handle guard matters: this
        // event fires for ANY link, the accessory central link included.
        conn.interval = Conn_Interval;
        conn.latency = Conn_Latency;
        conn.supervisionTimeout = Supervision_Timeout;
    }

    // Deliver to whichever link owns the handle, so an accessory link can check
    // that a relax it requested actually took effect. Routed rather than handled
    // here because only the owner knows what it asked for.
    Ble::bleEventRouter().onConnUpdateComplete(Connection_Handle, Status,
            Conn_Interval, Conn_Latency, Supervision_Timeout);
}

// una_ble_hci_disconnection_complete_event is routed by handle in BleEventDispatch.cpp.

void una_ble_hci_le_data_length_change_event(uint16_t Connection_Handle,
        uint16_t MaxTxOctets, uint16_t MaxTxTime, uint16_t MaxRxOctets,
        uint16_t MaxRxTime)
{
    LOG_DEBUG("DLE: TxOctets %u, TxTime %u us, RxOctets %u, RxTime %u us\n",
            MaxTxOctets, MaxTxTime,
            MaxRxOctets, MaxRxTime);
}

void una_ble_att_exchange_mtu_resp_event(uint16_t Connection_Handle,
        uint16_t Server_RX_MTU)
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);
    Ble::PeripheralBlueNRG::ConnectionInfo &conn = instance->mConnection;

    conn.mtu = Server_RX_MTU;
    LOG_DEBUG("New MTU: %d\n", conn.mtu);

    instance->mObserver.notify(&Interface::IBle::StatusCallback::onMtuChanged, conn.mtu);
}


void una_ble_gap_addr_not_resolved_event(uint16_t Connection_Handle)
{
}

void una_ble_gap_slave_security_initiated_event(void)
{
    // Do nothing
}

void una_ble_gap_pass_key_req_event(uint16_t Connection_Handle)
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);
    Ble::PeripheralBlueNRG::ConnectionInfo &conn = instance->mConnection;

    // The passkey is the only secret behind MITM-protected pairing, so it comes
    // from the controller's random number generator (HCI LE Rand), never from
    // anything an attacker could predict such as uptime.
    uint8_t random[8];
    una_ble_status status = una_ble_hci_le_rand(random);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        // No passkey reply: the pairing fails on the SMP timeout instead of
        // falling back to a guessable passkey.
        LOG_ERROR("una_ble_hci_le_rand() failed [0x%02X]\n", status);
        return;
    }
    uint64_t value;
    std::memcpy(&value, random, sizeof(value));
    // Passkeys run 000000-999999; the modulo bias over 64 bits is negligible
    uint32_t pin = static_cast<uint32_t>(value % 1000000);

    // Send pin code
    status = una_ble_gap_pass_key_resp(conn.handle, pin);
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_ERROR("una_ble_gap_pass_key_resp() failed [0x%02X]\n", status);
    } else {
        // The system log is written to the watch's storage, so the passkey
        // itself stays out of it
        LOG_INFO("Pin code sent\n");
        instance->mObserver.notify(&Interface::IBle::StatusCallback::onPinRequest, pin);
        // Next events:
        // una_ble_gap_pairing_complete_event
    }
}

void una_ble_gap_pairing_complete_event(uint16_t Connection_Handle, uint8_t Status,
        uint8_t Reason)
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);
    Ble::PeripheralBlueNRG::ConnectionInfo &conn = instance->mConnection;

    if(Status == UNA_BLE_STATUS_SUCCESS) {
        LOG_DEBUG("Paired\n");
        // Next events:
        // una_ble_hci_encryption_change_event
    } else {
        LOG_ERROR("Pairing failed [0x%02X]. Reason 0x%02X\n", Status, Reason);


        una_ble_status status = una_ble_gap_terminate(conn.handle, 0x05);
        if (status != UNA_BLE_STATUS_SUCCESS) {
            LOG_ERROR("una_ble_gap_terminate() failed [0x%02X]\n", status);
        }
        // Next events:
        // una_ble_hci_disconnection_complete_event
    }

    instance->mObserver.notify(&Interface::IBle::StatusCallback::onPair, Status == UNA_BLE_STATUS_SUCCESS);
}

void una_ble_hci_encryption_change_event(uint8_t Status, uint16_t Connection_Handle,
        uint8_t Encryption_Enabled)
{
    if(Status == UNA_BLE_STATUS_SUCCESS && Encryption_Enabled == 1) {
        LOG_INFO("Encrypted. Connection_Handle 0x%04X\n", Connection_Handle);

        // Request Data Length Extension now the link is fully up (after the LL
        // feature exchange). Requesting it at connect time (pre-encryption) was
        // accepted but never negotiated. Result appears in
        // una_ble_hci_le_data_length_change_event ("DLE: TxOctets ...").
        una_ble_status dleStatus = una_ble_hci_le_set_data_length(Connection_Handle, 251, 2120);
        if (dleStatus != UNA_BLE_STATUS_SUCCESS) {
            LOG_WARNING("una_ble_hci_le_set_data_length() failed [0x%02X]\n", dleStatus);
        }

        // Initiate MTU exchange.
        // Next events:
        // una_ble_att_exchange_mtu_resp_event
        // una_ble_gatt_proc_complete_event
        //
        // The controller can refuse this with 0x46 BLE_STATUS_NOT_ALLOWED.
        // conn.mtu is only set by una_ble_att_exchange_mtu_resp_event, so it
        // then stays 0 and setCharacteristicValue() uses a 220-byte payload cap.
        una_ble_status bleStatus = una_ble_gatt_exchange_config(Connection_Handle);
        if (bleStatus != UNA_BLE_STATUS_SUCCESS) {
            // Warning, not error: the link and its transfers work regardless.
            LOG_WARNING("una_ble_gatt_exchange_config() failed [0x%02X]\n", bleStatus);
        }

        // Begin serialised GATT-client discovery on the encrypted link.
        Ble::blePeripheralClients().start(Connection_Handle);
    } else {
        LOG_ERROR("Encryption failed [0x%02X]. Enabled %d\n", Status, Encryption_Enabled);
    }

    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);
    instance->mObserver.notify(&Interface::IBle::StatusCallback::onEncrypt, Status == UNA_BLE_STATUS_SUCCESS);
}

void una_ble_hci_encryption_key_refresh_complete_event(uint8_t Status,
        uint16_t Connection_Handle)
{
    LOG_DEBUG("Status 0x%02X, Connection_Handle 0x%04X\n", Status, Connection_Handle);
}

void una_ble_gap_bond_lost_event(void)
{
    LOG_DEBUG("bond lost\n");
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);
    Ble::PeripheralBlueNRG::ConnectionInfo &conn = instance->mConnection;
    una_ble_gap_allow_rebond(conn.handle);
}


void una_ble_gatt_attribute_modified_event(uint16_t Connection_Handle,
                                       uint16_t Attr_Handle,
                                       uint16_t Offset,
                                       uint16_t Attr_Data_Length,
                                       uint8_t Attr_Data[])
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);

    // Incoming GATT activity on the phone link: feed the transfer-quiet gate
    // used by the accessory link-vacation logic.
    instance->noteTraffic();

    Ble::PeripheralBlueNRG::CharacteristicHolder *hchr = nullptr;
    // Notification/Indication
    hchr = instance->findSCharacteristicByDesc(Attr_Handle);
    if (hchr) {
        LOG_DEBUG("N/I: UUID %s: 0x%02X\n",
                hchr->uuid.toString(instance->tbuff), Attr_Data[0]);

        if (hchr->callback) {
             hchr->callback->onNotifyIndicate(hchr, (Attr_Data[0] & 0x01), (Attr_Data[0] & 0x02));
        }
    }

    hchr = instance->findSCharacteristicByAttr(Attr_Handle);
    if (hchr) {
        LOG_DEBUG("W: UUID %s: Len %u\n",
                hchr->uuid.toString(instance->tbuff), Attr_Data_Length);
        LOG_DEBUG_DUMP(Attr_Data, Attr_Data_Length);

        if (hchr->callback) {
            hchr->callback->onWrite(hchr, Attr_Data, Attr_Data_Length);
        }
    }
}

void una_ble_gatt_read_permit_req_event(uint16_t Connection_Handle,
        uint16_t Attribute_Handle, uint16_t Offset)
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);

    Ble::PeripheralBlueNRG::CharacteristicHolder *hchr =
            instance->findSCharacteristicByAttr(Attribute_Handle);

    if (hchr) {
        LOG_DEBUG("R: UUID %s\n", hchr->uuid.toString(instance->tbuff));
        if (hchr->callback) {
            hchr->callback->onRead(hchr);
        }
    } else {
        LOG_WARNING("R: unknown Attribute_Handle 0x%04X\n", Attribute_Handle);
    }

    LOG_DEBUG("R: allow_read\n");
    una_ble_status allowStatus = Ble::aciRetry(5, 20, [Connection_Handle]{
        return una_ble_gatt_allow_read(Connection_Handle);
    });
    if (allowStatus != UNA_BLE_STATUS_SUCCESS) {
        LOG_WARNING("una_ble_gatt_allow_read() failed [0x%02X]\n", allowStatus);
    }
}

void una_ble_gatt_read_multi_permit_req_event(
        uint16_t Connection_Handle, uint8_t Number_of_Handles,
        una_ble_handle_item Handle_Item[])
{
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    assert(instance != nullptr);

    for (uint8_t i = 0; i < Number_of_Handles; i++) {
        Ble::PeripheralBlueNRG::CharacteristicHolder *hchr =
                instance->findSCharacteristicByAttr(Handle_Item[i].handle);
        if (hchr) {
            LOG_DEBUG("R multi[%u]: UUID %s\n", i, hchr->uuid.toString(instance->tbuff));
            if (hchr->callback) {
                hchr->callback->onRead(hchr);
            }
        } else {
            LOG_WARNING("R multi[%u]: unknown Attribute_Handle 0x%04X\n",
                    i, Handle_Item[i].handle);
        }
    }

    LOG_DEBUG("R multi: allow_read\n");
    una_ble_status allowStatus = Ble::aciRetry(5, 20, [Connection_Handle]{
        return una_ble_gatt_allow_read(Connection_Handle);
    });
    if (allowStatus != UNA_BLE_STATUS_SUCCESS) {
        LOG_WARNING("una_ble_gatt_allow_read() failed [0x%02X]\n", allowStatus);
    }
}


void una_ble_gatt_indication_event(uint16_t Connection_Handle,
        uint16_t Attribute_Handle, uint8_t Attribute_Value_Length,
        uint8_t Attribute_Value[])
{
    LOG_DEBUG("Handle 0x%04X Attr 0x%04X Len %u\n",
            Connection_Handle, Attribute_Handle, Attribute_Value_Length);

    // Confirm the indication to prevent una_ble_gatt_proc_timeout_event (30 s).
    // The BLE spec requires the receiver of an indication to send a
    // confirmation (ATT_HANDLE_VALUE_CONFIRMATION PDU); without it the
    // remote sender considers the GATT procedure still in progress and
    // the stack fires a timeout after 30 seconds and disconnects.
    // Retry on 0xFF (BLE_STATUS_BUSY): the chip may be processing another
    // operation (e.g. security handshake) immediately after connection.
    una_ble_status status = Ble::aciRetry(5, 20, [Connection_Handle]{
        return una_ble_gatt_confirm_indication(Connection_Handle);
    });
    if (status != UNA_BLE_STATUS_SUCCESS) {
        LOG_WARNING("una_ble_gatt_confirm_indication() failed [0x%02X]\n", status);
    }
}

void una_ble_gatt_server_confirmation_event(uint16_t Connection_Handle)
{
    LOG_DEBUG("Handle 0x%04X\n", Connection_Handle);
}

void una_ble_gatt_tx_pool_available_event(uint16_t Connection_Handle,
        uint16_t Available_Buffers)
{
    // Controller TX buffers freed. Wake any sender parked on a full pool (the
    // FTS windowed read pump) so it resumes immediately -- the single-threaded,
    // event-driven refill that keeps the pool fed without blocking this thread.
    Ble::PeripheralBlueNRG *instance = Ble::PeripheralBlueNRG::pInstance;
    if (instance != nullptr) {
        instance->mObserver.notify(&Interface::IBle::StatusCallback::onTxReady);
    }
}



// una_ble_gatt_proc_complete_event is routed by handle in BleEventDispatch.cpp.

void una_ble_blue_events_lost_event(uint8_t Lost_Events[8])
{
    char buf[20] {};
    int pos = snprintf(buf, sizeof(buf), "0x");
    for (int i = 7; i >= 0 && pos < (int)(sizeof(buf)) - 2; --i) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", Lost_Events[i]);
    }
    LOG_WARNING("Lost_Events %s\n", buf);
}

/**
 * @brief This event is generated by the client/server to the application
 *  on a GATT timeout (30 seconds).
 *
 *  This is a critical event that should  not happen during normal operating
 *  conditions. It is an indication of either a major disruption in the
 *  communication link or a mistake in the application which does not provide
 *  a reply to GATT procedures.
 *  After this event, the GATT channel is closed and no more GATT
 *  communication can be performed. The applications is exptected to issue
 *  an @ref una_ble_gap_terminate to disconnect from the peer device. It is
 *  important to leave an 100 ms blank window before sending
 *  the @ref una_ble_gap_terminate, since immediately after this event,
 *  system could save important information in non volatile memory.
 *
 * @param Connection_Handle Connection handle on which the GATT procedure has timed out
 * @retval None
 */
void una_ble_gatt_proc_timeout_event(uint16_t Connection_Handle)
{
    LOG_WARNING("GATT timeout (30 seconds)\n");
    OS::Delay(100);
    una_ble_gap_terminate(Connection_Handle, 0x13);
}

} /* extern "C" */
