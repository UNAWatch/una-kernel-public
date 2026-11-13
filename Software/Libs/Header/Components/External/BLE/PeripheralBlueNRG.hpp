/**
 ******************************************************************************
 * @file    PeripheralBlueNRG.hpp
 * @brief   BlueNRG Peripheral component.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __COMPONENT_BLE_PERIPHERAL_BLUENRG_HPP
#define __COMPONENT_BLE_PERIPHERAL_BLUENRG_HPP

#include <cstdint>
#include <cstdbool>

#include "main.h"
#include "OS.hpp"

#include "IBle.hpp"
#include "CoreBlueNRG.hpp"
#include "EventRouter.hpp"
#include "ILinkClient.hpp"
#include "Observer.hpp"
#include "OS.hpp"

#include "PeripheralLinkClients.hpp"
#include "GattClientOpsBlueNRG.hpp"
#include "PeripheralGattSchedulerOs.hpp"

namespace Ble
{

/**
 * @brief   BlueNRG Peripheral component.
 */
class PeripheralBlueNRG : virtual public Interface::IBle,
        public CoreBlueNRG::ResetHandler,
        public Ble::ILinkClient {

public:

    /**
     * @brief   Constructor.
     */
    PeripheralBlueNRG(CoreBlueNRG &core);

    /**
     * @brief   Destructor.
     */
    virtual ~PeripheralBlueNRG();

    /**
     * @brief   Initialize BLE object.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool init() override;

    /**
     * @brief   Deinitialize BLE object and free all resources.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool deinit() override;

    /**
     * @brief   Turn on BLE.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool enable() override;

    /**
     * @brief   Turn off BLE or put it to low power mode.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool disable() override;

    /**
     * @brief   Check if the BLE is on.
     * @retval  'true' - BLE is on, 'false' - otherwise.
     */
    virtual bool isEnabled() override;

    /**
     * @brief   Check if the BLE chip is alive and responding to commands.
     * @retval  'true' - chip is responding, 'false' - chip is not responding.
     */
    virtual bool isAlive() override;

    /**
     * @brief   Set the device name for BLE advertising.
     * @param   name: The name to be set.
     */
    virtual void setName(const char *name) override;

    /**
     * @brief   Get the device name for BLE advertising.
     * @retval   name: The device name.
     */
    const char* getName() const;

    /**
     * @brief   Add a service to the BLE stack.
     * @param   uuid: UUID of the service.
     * @retval  Handle to the added service.
     */
    virtual ServiceHandle addService(const UUID &uuid) override;

    /**
     * @brief   Remove a service from the BLE stack.
     * @param   handle: Handle to the service to be removed.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool delService(ServiceHandle handle) override;

    /**
     * @brief   Add a characteristic to a service.
     * @param   srvHandle: Handle of the service to which the characteristic
     *          is added.
     * @param   uuid: UUID of the characteristic.
     * @param   maxlen: Maximum length of the characteristic's value.
     * @param   properties: Properties of the characteristic (read, write,
     *          notify, etc.).
     * @retval  Handle to the added characteristic.
     */
    virtual CharacteristicHandle addCharacteristic(ServiceHandle srvHandle,
            const UUID &uuid, uint16_t maxlen, uint8_t properties) override;

    /**
     * @brief   Remove a characteristic from the BLE stack.
     * @param   handle: Handle to the characteristic to be removed.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool delCharacteristic(CharacteristicHandle handle) override;

    /**
     * @brief   Retrieve a service handle by its UUID.
     * @param   uuid: UUID of the desired service.
     * @retval  Handle to the service, or invalid handle if not found.
     */
    virtual ServiceHandle getServiceByUUID(const UUID &uuid) override;

    /**
     * @brief   Retrieve the UUID of a service by its handle.
     * @param   handle: Handle to the service.
     * @retval  UUID of the service.
     */
    virtual UUID getServiceUUID(ServiceHandle handle) override;

    /**
     * @brief   Retrieve a characteristic handle by its UUID.
     * @param   srvHandle: Handle of the service containing the characteristic.
     * @param   uuid: UUID of the desired characteristic.
     * @retval  Handle to the characteristic, or invalid handle if not found.
     */
    virtual CharacteristicHandle getCharacteristicByUUID(
            ServiceHandle srvHandle, const UUID &uuid) override;

    /**
     * @brief   Retrieve the UUID of a characteristic by its handle.
     * @param   handle: Handle to the characteristic.
     * @retval  UUID of the characteristic.
     */
    virtual UUID getCharacteristicUUID(CharacteristicHandle handle) override;

    /**
    * @brief   Get the maximum length of a characteristic.
    * @retval  Maximum length of the characteristic in bytes.
    */
    virtual uint32_t getMaxCharacteristicLen() override;

    /**
     * @brief   Set the value of a characteristic.
     * @param   handle: Handle of the characteristic to be updated.
     * @param   pData: Pointer to the data to be written to the characteristic.
     * @param   len: Length of the data in bytes.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool setCharacteristicValue(Ble::CharacteristicHandle handle,
            const uint8_t *pData, uint8_t len) override;

    virtual bool trySetCharacteristicValue(Ble::CharacteristicHandle handle,
            const uint8_t *pData, uint8_t len, bool *pPoolFull = nullptr) override;

    /**
     * @brief   Attach a status callback for BLE events.
     * @note    It is allowed attach multiple callbacks.
     * @param   pCallback: Pointer to the callback interface.
     */
    virtual void attachStatusCallback(
            Interface::IBle::StatusCallback *pCallback) override;

    /**
     * @brief   Detach the currently attached callback.
     * @param   pCallback: Pointer to the callback interface to be detached.
     */
    virtual void detachStatusCallback(
            Interface::IBle::StatusCallback *pCallback) override;

    /**
     * @brief   Attach a characteristic callback for BLE events.
     * @note    It is allowed attach only one callback for characteristic.
     * @param   handle: Handle of the characteristic.
     * @param   pCallback: Pointer to the callback interface.
     *          'nullptr' to detach.
     */
    virtual void attachCharacteristicCallback(Ble::CharacteristicHandle handle,
            Interface::IBle::CharacteristicCallback *pCallback) override;

    /**
     * @brief   Pair with a remote BLE device.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool pair() override;

    /**
     * @brief   Unpair from a remote BLE device.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool unpair() override;

    /**
     * @brief   Check if the device is bonded.
     * @retval  'true' - device is bonded, 'false' - otherwise.
     */
    virtual bool isBonded() override;

    /**
     * @brief   Get the number of bonded devices.
     * @retval  Number of bonded devices.
     */
    virtual uint8_t getBondedCount() override;

    /**
     * @brief   Start BLE advertising.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool startAdvertising() override;

    /**
     * @brief   Stop BLE advertising.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool stopAdvertising() override;

    /**
     * @brief   Check if the device is advertise.
     * @retval  'true' - device is advertise, 'false' - otherwise.
     */
    virtual bool isAdvertise() override;

    /**
     * @brief   Disconnect from the currently connected BLE device.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool disconnect() override;

    /**
     * @brief   Check if the device is currently connected to a remote
     *          BLE device.
     * @retval  'true' - device is connected, 'false' - otherwise.
     */
    virtual bool isConnected() override;


    /**
     * @brief   Start FCC TX test (LE transmitter test).
     * @note    Closes any active connection before starting.
     * @param   channel: RF channel index, N = (F - 2402) / 2, range 0x00..0x27.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool startTxFCC(uint8_t channel) override;

    /**
     * @brief   Start FCC RX test (LE receiver test).
     * @note    Closes any active connection before starting.
     * @param   channel: RF channel index, N = (F - 2402) / 2, range 0x00..0x27.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool startRxFCC(uint8_t channel) override;

    /**
     * @brief   Stop FCC TX test and retrieve the number of transmitted packets.
     * @param   numPackets: reference to store the number of transmitted packets.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool stopTxFCC(uint32_t &numPackets) override;

    /**
     * @brief   Stop FCC RX test and retrieve the number of received packets.
     * @param   numPackets: reference to store the number of received packets.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool stopRxFCC(uint32_t &numPackets) override;

    /**
     * @brief   Run a multi-channel RX FCC scan without chip reset between channels.
     * @note    Resets the chip once before the first channel. Closes any active
     *          connection before starting.
     * @param   channels:  pointer to array of RF channel indices (0x00..0x27).
     * @param   count:     number of channels in the array.
     * @param   dwellMs:   receive window per channel in milliseconds.
     * @param   results:   caller-allocated array of at least 'count' entries.
     * @retval  'true' if all slots completed without HCI error, 'false' otherwise.
     */
    virtual bool runRxFCCScan(const uint8_t *channels, uint8_t count,
            uint32_t dwellMs, RxFCCScanResult *results) override;

    /**
     * @brief   BLE peripheral operating state.
     */
    enum class State {
        NO_INIT,
        INITIALIZED,
        ENABLED,
        ADVERTISE,
        CONNECTED,
    };

    /**
     * @brief   Timestamp (ms) of the last observed phone-link GATT traffic in
     *          either direction (outgoing notify/indicate or incoming write).
     * @retval  OS::GetTimeMs() value at the last traffic, 0 if none yet.
     */
    uint32_t lastTrafficMs() const { return mLastTrafficMs; }

    /**
     * @brief   Whether the phone link has been quiet for at least quietMs.
     * @note    The IBle::isTransferQuiet implementation. Consumed by the GATT-client
     *          coordinator to defer discovery / setup writes while a transfer is
     *          in flight, and by the accessory link-vacation gate to avoid
     *          tearing down a live sync. A handle-less "no traffic yet" reads as
     *          quiet.
     * @param   quietMs: required quiet window in milliseconds.
     * @retval  'true' if no traffic seen within the last quietMs.
     */
    bool isTransferQuiet(uint32_t quietMs) const override;

    /**
     * @brief   Record that phone-link GATT traffic just occurred.
     * @note    Called from the data path (setCharacteristicValue success and
     *          incoming attribute-modified events).
     */
    void noteTraffic();

protected:

private:
    // BlueNRG Core component
    CoreBlueNRG &mCore;

    // Phone-link GATT-client discovery ports. The coordinator itself is the
    // process-wide blePeripheralClients() singleton; this owner only binds the ports
    // to it, so it stays free of any concrete service client.
    GattClientOpsBlueNRG mGattOps { mCore };
    PeripheralGattSchedulerOs mGattScheduler;

    // The BlueNRG library is not designed to create multiple instances
    // and uses static callbacks. This static pointer is created to access
    // the CoreBlueNRG object.
    static PeripheralBlueNRG *pInstance;

    State mState { };

    static const uint32_t skNameLenMax = 20;
    char mName[skNameLenMax];


    const uint16_t kDeviceAppearance = BLE_DEVICE_APPEARANCE;
    // Company identifier code
    const uint16_t kManufId = BLE_MANUFACTURER_ID;
    // Manufacturer "Magic string" for device recognition
    const char kManufMagic[sizeof(BLE_MANUFACTURER_MAGIC_STR)] {BLE_MANUFACTURER_MAGIC_STR};


    // The maximum number of bonded devices
    static const uint32_t skBondedListSize = 12;

    // Connection information
    struct ConnectionInfo {
        uint16_t handle;
        Address  addr;
        uint16_t interval;
        uint16_t latency;
        uint16_t supervisionTimeout;
        uint32_t mtu;

        void clear()
        {
            handle = 0;
            addr.setAddress(nullptr);
            interval = 0;
            latency = 0;
            supervisionTimeout = 0;
            mtu = 0;
        }
    } ;
    ConnectionInfo mConnection { };

    // Services
    static const uint32_t skServicesMax = 10;
    struct ServiceHolder {
        UUID uuid;
        uint8_t handlesNum;
        uint16_t handle;

        void clear()
        {
            uuid.clear();
            handlesNum = 0;
            handle = 0;
        }
    };
    ServiceHolder mServiceHolder[skServicesMax] {};

    // Characteristics
    static const uint32_t skCharacteristicsMax = 20;
    struct CharacteristicHolder {
        UUID uuid;
        uint16_t len;
        uint8_t properties;
        ServiceHolder *service;
        uint16_t handle;
        Interface::IBle::CharacteristicCallback *callback;

        void clear()
        {
            uuid.clear();
            len = 0;
            properties = 0;
            service = nullptr;
            handle = 0;
            callback = nullptr;
        }
    };
    CharacteristicHolder mCharacteristicHolder[skCharacteristicsMax] {};


    // Array of an observers
    Observer::Holder<Interface::IBle::StatusCallback, 15> mObserver {};

    // Temporary buffer to print UUDs and MAC
    char tbuff[37];



    bool initServices();
    bool startAdv(bool pair = false);
    bool stopAdv();
    bool terminate();
    bool updateWhitelist();
    bool isBondedAtLeastOne();
    uint8_t countBonded();
    // Ble::ILinkClient (the watch's peripheral/phone link owner). The router
    // delivers the slave-role connection and its GATT-client events here; the
    // GATT-client events are forwarded to the blePeripheralClients() coordinator.
    void onConnected(uint16_t connHandle, uint8_t addrType,
            const uint8_t addr[6], uint16_t connInterval, uint16_t connLatency,
            uint16_t supTimeout) override;
    void onDisconnected(uint16_t connHandle, uint8_t reason) override;
    void onServiceGroupFound(uint16_t connHandle, uint16_t foundHandle,
            uint16_t endHandle) override;
    void onCharDiscovered(uint16_t connHandle, const uint8_t *attrData,
            uint8_t len) override;
    void onCharDescFound(uint16_t connHandle, uint16_t attrHandle,
            uint16_t uuid16) override;
    void onNotification(uint16_t connHandle, uint16_t attrHandle,
            const uint8_t *data, uint8_t len) override;
    void onGattError(uint16_t connHandle, uint8_t reqOpcode,
            uint16_t attrHandle, uint8_t errorCode) override;
    void onGattProcComplete(uint16_t connHandle, uint8_t errorCode) override;

    // CoreBlueNRG::ResetHandler: invalidate cached state after a spontaneous
    // coprocessor reset so the BLE watchdog re-enables and re-advertises.
    void onReset() override;

    // Timestamp (ms) of the last phone-link GATT traffic; see noteTraffic().
    volatile uint32_t mLastTrafficMs { 0 };

    ServiceHolder* findService(ServiceHandle handle);
    ServiceHolder* findService(const UUID &uuid);
    ServiceHolder* findService(uint16_t handle);
    CharacteristicHolder* findCharacteristic(CharacteristicHandle handle);
    CharacteristicHolder* findCharacteristic(ServiceHolder* hsrv, const UUID &uuid);
    CharacteristicHolder* findCharacteristic(const UUID &uuid);
    CharacteristicHolder* findSCharacteristic(uint16_t handle);
    CharacteristicHolder* findSCharacteristicByAttr(uint16_t attrHandle);
    CharacteristicHolder* findSCharacteristicByDesc(uint16_t descHandle);


    // Connection. Connection-complete and disconnection are routed via
    // Ble::EventRouter (BleEventDispatch.cpp); only the peripheral-specific
    // connection-update event still reaches this class directly.
    friend void ::una_ble_hci_le_connection_update_complete_event(uint8_t Status,
            uint16_t Connection_Handle, uint16_t Conn_Interval,
            uint16_t Conn_Latency, uint16_t Supervision_Timeout);

    friend void ::una_ble_hci_le_data_length_change_event(uint16_t Connection_Handle,
            uint16_t MaxTxOctets, uint16_t MaxTxTime, uint16_t MaxRxOctets,
            uint16_t MaxRxTime);

    friend void ::una_ble_att_exchange_mtu_resp_event(uint16_t Connection_Handle,
            uint16_t Server_RX_MTU);


    // Pairing / Bonding
    friend void ::una_ble_gap_addr_not_resolved_event(uint16_t Connection_Handle);

    friend void ::una_ble_gap_slave_security_initiated_event(void);

    friend void ::una_ble_gap_pass_key_req_event(uint16_t Connection_Handle);

    friend void ::una_ble_gap_pairing_complete_event(uint16_t Connection_Handle,
            uint8_t Status, uint8_t Reason);

    friend void ::una_ble_hci_encryption_change_event(uint8_t Status,
            uint16_t Connection_Handle, uint8_t Encryption_Enabled);

    friend void ::una_ble_hci_encryption_key_refresh_complete_event(uint8_t Status,
            uint16_t Connection_Handle);

    friend void ::una_ble_gap_bond_lost_event(void);


    // Data handling
    friend void ::una_ble_gatt_attribute_modified_event(uint16_t Connection_Handle,
            uint16_t Attr_Handle, uint16_t Offset, uint16_t Attr_Data_Length,
            uint8_t Attr_Data[]);

    friend void ::una_ble_gatt_read_permit_req_event(uint16_t Connection_Handle,
            uint16_t Attribute_Handle, uint16_t Offset);

    friend void ::una_ble_gatt_read_multi_permit_req_event(
            uint16_t Connection_Handle, uint8_t Number_of_Handles,
            una_ble_handle_item Handle_Item[]);

    // Sometimes occurred
    friend void ::una_ble_gatt_indication_event(uint16_t Connection_Handle,
            uint16_t Attribute_Handle, uint8_t Attribute_Value_Length,
            uint8_t Attribute_Value[]);

    friend void ::una_ble_gatt_server_confirmation_event(
            uint16_t Connection_Handle);

    friend void ::una_ble_gatt_tx_pool_available_event(uint16_t Connection_Handle,
            uint16_t Available_Buffers);

    friend void ::una_ble_blue_events_lost_event(uint8_t Lost_Events[8]);

    friend void ::una_ble_gatt_proc_timeout_event(uint16_t Connection_Handle);


};

}  /* namespace Ble */

#endif /* __COMPONENT_BLE_PERIPHERAL_BLUENRG_HPP */
