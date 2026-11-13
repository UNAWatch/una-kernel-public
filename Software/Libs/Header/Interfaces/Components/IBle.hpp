/**
 ******************************************************************************
 * @file    IBle.hpp
 * @brief   BLE Interface.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __INTERFACE_I_BLE_HPP
#define __INTERFACE_I_BLE_HPP

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>


namespace Ble
{

/**
 * @brief   Bluetooth Device Address definition
 */
class Address {
public:

    static constexpr size_t skSize = 6; // Size of Bluetooth address

    /**
     * @brief   Bluetooth address types
     */
    enum Type : uint8_t {
        PUBLIC = 0x00,              ///< Public Device Address
        RANDOM = 0x01,              ///< Random Device Address
        RESOLVABLE_PRV = 0x02,      ///< Public Identity Address (Corresponds to Resolved Private Address)
        NON_RESOLVABLE_PRV = 0x03,  ///< Random (Static) Identity Address (Corresponds to Resolved Private Address)
    };

    Address() : mAddrU64(0) { }
    Address(const uint8_t *address, Type type) : mAddrU64(0)
    {
        if (address != nullptr) {
            memcpy(mAddr, address, skSize);
            mType = type;
        }
    }

    // Getter for address
    const uint8_t* getAddress() const
    {
        return mAddr;
    }

    // Setter for address
    void setAddress(const uint8_t *address)
    {
        if (address != nullptr) {
            memcpy(&mAddr, address, skSize);
        } else {
            memset(&mAddrU64, 0, sizeof(mAddrU64));
        }
    }

    // Getter for address type
    Type getType() const
    {
        return mType;
    }

    // Setter for address type
    void setType(Type type)
    {
        mType = type;
    }

    bool isSet()
    {
        return mAddrU64 != 0;
    }

    char* toString(char *tbuff, size_t sz = 0) const
    {
        if (tbuff) {
            if (sz == 0) {
                sz = 18; // no limit
            }
                snprintf(tbuff, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
                        mAddr[5], mAddr[4], mAddr[3],
                        mAddr[2], mAddr[1], mAddr[0]);
        }
        return tbuff;
    }

    // Overloaded assignment operator
    Address& operator=(const Address &other)
    {
        if (this != &other) {
            memcpy(&mAddrU64, &other.mAddrU64, sizeof(mAddrU64));
        }
        return *this;
    }

    // Overloaded equality operator
    bool operator==(const Address &other) const
    {
        return mAddrU64 == other.mAddrU64;
    }

    // Overloaded inequality operator
    bool operator!=(const Address &other) const
    {
        return !(*this == other);
    }

private:

    union {
        struct {
            Type mType;             ///< Address type.
            uint8_t mAddr[skSize];  ///< Bluetooth Device Address
        };
        struct {
            uint64_t mAddrU64;      ///< Not zero if set
        };
    };

};

/**
 * @brief   Bluetooth UUID
 */
class UUID {
public:
    ///< UUID16 length in bytes
    static constexpr uint8_t skLen16 = 2;

    ///< UUID128 length in bytes
    static constexpr uint8_t skLen128 = 16;

    UUID() = default;
    UUID(uint16_t uuid) { set16(uuid); }
    UUID(uint8_t *uuid) { set128(uuid); }
    UUID(const char *uuid) { fromString(uuid); }

    void set16(uint16_t uuid)
    {
        memcpy(mUuid, &uuid, skLen16);
        mIs128 = false;
    }

    void set128(const uint8_t *uuid)
    {
        memcpy(mUuid, uuid, skLen128);
        mIs128 = true;
    }

    void fromString(const char *str)
    {
        if (strlen(str) != 36 || str[8] != '-' || str[13] != '-'
            || str[18] != '-' || str[23] != '-') {
            return;
        }
        for (uint8_t i = 0; i < skLen128; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                str += 1;
            }
            char b[3] { };
            memcpy(b, str, 2);
            mUuid[16 - 1 - i] = static_cast<uint8_t>(strtol(b, nullptr, 16));
            str += 2;
        }
        mIs128 = true;
    }

    char* toString(char *tbuff, size_t sz = 0) const
    {
        if (tbuff) {
            if (sz == 0) {
                sz = 37; // no limit
            }
            if(mIs128) {
                snprintf(tbuff, sz,
                        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                        mUuid[15],mUuid[14],mUuid[13],mUuid[12],
                        mUuid[11],mUuid[10],
                        mUuid[9],mUuid[8],
                        mUuid[7],mUuid[6],
                        mUuid[5],mUuid[4],mUuid[3],mUuid[2],mUuid[1],mUuid[0]);
            } else {
                snprintf(tbuff, sz, "0x%02X%02X", mUuid[1],mUuid[0]);
            }
        }
        return tbuff;
    }

    bool is128() const
    {
        return mIs128;
    }

    uint16_t get16() const
    {
        return *(uint16_t*) mUuid;
    }

    const uint8_t* get128() const
    {
        return mUuid;
    }

    bool isValid() const
    {
        uint16_t sum = 0;
        for (uint8_t i = 0; i < (mIs128 ? skLen128 : skLen16); i++) {
            sum += mUuid[i];
        }
        return sum > 0;
    }

    void clear()
    {
        memset(mUuid, 0, skLen128);
        mIs128 = false;
    }

    // Overloaded assignment operator
    UUID& operator=(const UUID &other)
    {
        if (this != &other) {
            memcpy(mUuid, other.mUuid, skLen128);
            mIs128 = other.mIs128;
        }
        return *this;
    }

    // Overloaded equality operator
    bool operator==(const UUID &other) const
    {
        if (mIs128 != other.mIs128) {
            return false;
        }
        return memcmp(mUuid, other.mUuid, skLen16 ? skLen128 : skLen16) == 0;
    }

    // Overloaded inequality operator
    bool operator!=(const UUID &other) const
    {
        return !(*this == other);
    }

private:
    uint8_t mUuid[skLen128] { };    ///< UUID
    bool mIs128 { };                ///< UUID type (16/128 bits)
};

/**
 * @brief   Bluetooth Service handle
 */
typedef void* ServiceHandle;

/**
 * @brief   Bluetooth Characteristic handle
 */
typedef void* CharacteristicHandle;

/**
 * @brief   Characteristic properties
 */
struct CharProp {
    static constexpr uint8_t BROADCAST = 0x01;
    static constexpr uint8_t READ = 0x02;
    static constexpr uint8_t WRITE_WO_RESP = 0x04;
    static constexpr uint8_t WRITE = 0x08;
    static constexpr uint8_t NOTIFY = 0x10;
    static constexpr uint8_t INDICATE = 0x20;
    static constexpr uint8_t SIGNED_WRITE = 0x40;
    static constexpr uint8_t EXT = 0x80;
};

} /* namespace Ble */


namespace Interface
{

/**
 * @brief   BLE Interface.
 */
class IBle
{

public:

    /**
     * @brief   BLE Status Callbacks interface
     */
    class StatusCallback {
    public:

        /**
         * @brief   Called when the stack is initialized and it is possible to
         *          perform operations with characteristics (update values, etc.).
         */
        virtual void onEnable() {}

        /**
         * @brief   Called when the pairing process request pin code.
         * @param   pin: 6-digits pin code.
         */
        virtual void onPinRequest(uint32_t pin) {}

        /**
         * @brief   Called when the pairing process completes.
         * @param   state: 'true' if pairing was successful, 'false' otherwise.
         */
        virtual void onPair(bool state) {}

        /**
         * @brief   Called when the encryption process completes.
         * @param   state: 'true' if encryption was successful, 'false' otherwise.
         */
        virtual void onEncrypt(bool state) {}

        /**
         * @brief   Called when the MTU changed.
         * @param   mtu: New MTU value.
         */
        virtual void onMtuChanged(uint16_t mtu) {}

        /**
         * @brief   Called when a connection to a remote device is established.
         * @param   addr: The Bluetooth address of the connected device.
         */
        virtual void onConnect(const Ble::Address &addr) {}

        /**
         * @brief   Called when a disconnection from a remote device occurs.
         * @param   reason: The reason for the disconnection (usually a
         *          BLE-specific error code).
         */
        virtual void onDisconnect(uint8_t reason) {}

        /**
         * @brief   Called when controller TX buffers free after a full pool
         *          (aci_gatt_tx_pool_available_event), so a sender that parked
         *          on a full pool can resume without polling.
         */
        virtual void onTxReady() {}

    protected:

        /**
         * @brief   Destructor.
         */
        virtual ~StatusCallback() = default;
    };

    /**
     * @brief   BLE Characteristic Callbacks interface
     */
    class CharacteristicCallback {
    public:

        /**
         * @brief   Called when a characteristic's notification or indication
         *          is enabled/disabled.
         * @param   chrHandle: Handle of the characteristic.
         * @param   notify: 'true' if notifications are enabled, 'false' disabled.
         * @param   indicate:'true' if indications are enabled, 'false' disabled.
         */
        virtual void onNotifyIndicate(Ble::CharacteristicHandle chrHandle,
                bool notify, bool indicate) {}

        /**
         * @brief   Called when data is written to a characteristic by the
         *          remote device.
         * @param   chrHandle: Handle of the characteristic being written to.
         * @param   pData: Pointer to the data that was written.
         * @param   len: Length of the data in bytes.
         */
        virtual void onWrite(Ble::CharacteristicHandle chrHandle,
                const uint8_t *pData, uint16_t len) {}

        /**
         * @brief   Called when the remote device reads a characteristic's value.
         * @param   chrHandle - Handle of the characteristic being read.
         */
        virtual void onRead(Ble::CharacteristicHandle chrHandle) {}

    protected:

        /**
         * @brief   Destructor..
         */
        virtual ~CharacteristicCallback() = default;
    };

    /**
     * @brief   Initialize BLE object.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool init() = 0;

    /**
     * @brief   Deinitialize BLE object and free all resources.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool deinit() = 0;

    /**
     * @brief   Enable/Turn on BLE.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool enable() = 0;

    /**
     * @brief   Disable/Turn off BLE or put it to low power mode.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool disable() = 0;

    /**
     * @brief   Check if the BLE is on.
     * @retval  'true' - BLE is on, 'false' - otherwise.
     */
    virtual bool isEnabled() = 0;

    /**
     * @brief   Check if the BLE chip is alive and responding to commands.
     *          Use this to detect a hung chip before performing BLE operations.
     * @retval  'true' - chip is responding, 'false' - chip is not responding.
     */
    virtual bool isAlive() = 0;

    /**
     * @brief   Set the device name for BLE advertising.
     * @param   name: The name to be set.
     */
    virtual void setName(const char *name) = 0;

    /**
     * @brief   Add a service to the BLE stack.
     * @param   uuid: UUID of the service.
     * @retval  Handle to the added service.
     */
    virtual Ble::ServiceHandle addService(const Ble::UUID &uuid) = 0;

    /**
     * @brief   Remove a service from the BLE stack.
     * @param   handle: Handle to the service to be removed.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool delService(Ble::ServiceHandle handle) = 0;

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
    virtual Ble::CharacteristicHandle addCharacteristic(
            Ble::ServiceHandle srvHandle, const Ble::UUID &uuid,
            uint16_t maxlen, uint8_t properties) = 0;

    /**
     * @brief   Remove a characteristic from the BLE stack.
     * @param   handle: Handle to the characteristic to be removed.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool delCharacteristic(Ble::CharacteristicHandle handle) = 0;

    /**
     * @brief   Retrieve a service handle by its UUID.
     * @param   uuid: UUID of the desired service.
     * @retval  Handle to the service, or invalid handle if not found.
     */
    virtual Ble::ServiceHandle getServiceByUUID(const Ble::UUID &uuid) = 0;

    /**
     * @brief   Retrieve the UUID of a service by its handle.
     * @param   handle: Handle to the service.
     * @retval  UUID of the service.
     */
    virtual Ble::UUID getServiceUUID(Ble::ServiceHandle handle) = 0;

    /**
     * @brief   Retrieve a characteristic handle by its UUID.
     * @param   srvHandle: Handle of the service containing the characteristic.
     * @param   uuid: UUID of the desired characteristic.
     * @retval  Handle to the characteristic, or invalid handle if not found.
     */
    virtual Ble::CharacteristicHandle getCharacteristicByUUID(
            Ble::ServiceHandle srvHandle, const Ble::UUID &uuid) = 0;

    /**
     * @brief   Retrieve the UUID of a characteristic by its handle.
     * @param   handle: Handle to the characteristic.
     * @retval  UUID of the characteristic.
     */
    virtual Ble::UUID getCharacteristicUUID(Ble::CharacteristicHandle handle) = 0;

    /**
    * @brief   Get the maximum length of a characteristic.
    * @retval  Maximum length of the characteristic in bytes.
    */
    virtual uint32_t getMaxCharacteristicLen() = 0;

    /**
     * @brief   Set the value of a characteristic.
     * @param   handle: Handle of the characteristic to be updated.
     * @param   pData: Pointer to the data to be written to the characteristic.
     * @param   len: Length of the data in bytes.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool setCharacteristicValue(Ble::CharacteristicHandle handle,
            const uint8_t *pData, uint8_t len) = 0;

    /**
     * @brief   Non-blocking variant of setCharacteristicValue(): a single
     *          attempt that returns false immediately when the controller TX
     *          pool is full, instead of retrying. Callers that can resume from
     *          StatusCallback::onTxReady() (e.g. a windowed transfer) use this
     *          to avoid blocking. Default falls back to the blocking form.
     * @param   pPoolFull: optional out-param. On a false return it is set true
     *          iff the failure was a full TX pool (i.e. onTxReady() will fire
     *          later and the send can be retried then); false for any other
     *          send failure, for which NO onTxReady() is coming and the caller
     *          must recover rather than park. Untouched on success.
     */
    virtual bool trySetCharacteristicValue(Ble::CharacteristicHandle handle,
            const uint8_t *pData, uint8_t len, bool *pPoolFull = nullptr) {
        if (pPoolFull) { *pPoolFull = false; }
        return setCharacteristicValue(handle, pData, len);
    }

    /**
     * @brief   Attach a status callback for BLE events.
     * @note    It is allowed attach multiple callbacks.
     * @param   pCallback: Pointer to the callback interface.
     */
    virtual void attachStatusCallback(
            Interface::IBle::StatusCallback *pCallback) = 0;

    /**
     * @brief   Detach the currently attached status callback.
     * @param   pCallback: Pointer to the callback interface to be detached.
     */
    virtual void detachStatusCallback(
            Interface::IBle::StatusCallback *pCallback) = 0;

    /**
     * @brief   Attach a characteristic callback for BLE events.
     * @note    It is allowed attach only one callback for characteristic.
     * @param   handle: Handle of the characteristic.
     * @param   pCallback: Pointer to the callback interface.
     *          'nullptr' to detach.
     */
    virtual void attachCharacteristicCallback(Ble::CharacteristicHandle handle,
            Interface::IBle::CharacteristicCallback *pCallback) = 0;

    /**
     * @brief   Pair with a remote BLE device.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool pair() = 0;

    /**
     * @brief   Unpair from a remote BLE device.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool unpair() = 0;

    /**
     * @brief   Check if the device is bonded.
     * @retval  'true' - device is bonded, 'false' - otherwise.
     */
    virtual bool isBonded() = 0;

    /**
     * @brief   Get the number of bonded devices.
     * @retval  Number of bonded devices.
     */
    virtual uint8_t getBondedCount() = 0;

    /**
     * @brief   Start BLE advertising.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool startAdvertising() = 0;

    /**
     * @brief   Stop BLE advertising.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool stopAdvertising() = 0;

    /**
     * @brief   Check if the device is advertise.
     * @retval  'true' - device is advertise, 'false' - otherwise.
     */
    virtual bool isAdvertise() = 0;

    /**
     * @brief   Disconnect from the currently connected BLE device.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool disconnect() = 0;

    /**
     * @brief   Check if the device is currently connected to a remote
     *          BLE device.
     * @retval  'true' - device is connected, 'false' - otherwise.
     */
    virtual bool isConnected() = 0;

    /**
     * @brief   Whether the link has carried no GATT traffic for at least
     *          quietMs. The central (accessory) role uses this to avoid
     *          scanning/connecting while the phone link is mid-transfer, since
     *          a concurrent GAP procedure starves the peripheral link.
     * @param   quietMs: minimum idle window, in milliseconds.
     * @retval  'true' - link idle for >= quietMs, 'false' - otherwise.
     */
    virtual bool isTransferQuiet(uint32_t quietMs) const = 0;

    // iOS phone-link GATT clients (ANCS notifications, AMS media) live in the
    // Ble::ClientANCS / Ble::ClientAMS components and are consumed directly, so
    // they are intentionally not part of this interface.


    // FCC testing. Optional
    /**
     * @brief   Start FCC TX test (LE transmitter test).
     * @note    Resets the chip and starts continuous packet transmission at
     *          maximum power. Closes any active connection before starting.
     * @param   channel: RF channel index, N = (F - 2402) / 2, range 0x00..0x27.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool startTxFCC(uint8_t channel) { return false; }

    /**
     * @brief   Start FCC RX test (LE receiver test).
     * @note    Resets the chip and starts continuous packet reception.
     *          Closes any active connection before starting.
     * @param   channel: RF channel index, N = (F - 2402) / 2, range 0x00..0x27.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool startRxFCC(uint8_t channel) { return false; }

    /**
     * @brief   Stop FCC TX test and retrieve the number of transmitted packets.
     * @param   numPackets: reference to store the number of transmitted packets.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool stopTxFCC(uint32_t &numPackets) { return false; }

    /**
     * @brief   Stop FCC RX test and retrieve the number of received packets.
     * @param   numPackets: reference to store the number of received packets.
     * @retval  Execution status. 'true' - success, 'false' - otherwise.
     */
    virtual bool stopRxFCC(uint32_t &numPackets) { return false; }

    /**
     * @brief   Result of a single channel slot in an RX FCC scan.
     */
    struct RxFCCScanResult {
        uint8_t  channel;     ///< RF channel index, N = (F - 2402) / 2.
        uint32_t numPackets;  ///< Number of received test packets on this channel.
        bool     ok;          ///< 'false' if the HCI command failed for this slot.
    };

    /**
     * @brief   Run a multi-channel RX FCC scan without chip reset between channels.
     * @note    Resets the chip once before the first channel, then cycles through
     *          all channels using stop->start without additional resets.
     *          Closes any active connection before starting.
     * @param   channels:  pointer to array of RF channel indices (0x00..0x27).
     * @param   count:     number of channels in the array.
     * @param   dwellMs:   receive window per channel in milliseconds.
     * @param   results:   pointer to caller-allocated array of at least 'count'
     *                     RxFCCScanResult entries filled on return.
     * @retval  'true' if all channel slots completed without HCI error,
     *          'false' if at least one slot failed (results still populated).
     */
    virtual bool runRxFCCScan(const uint8_t *channels, uint8_t count,
            uint32_t dwellMs, RxFCCScanResult *results) { return false; }

protected:

    /**
     * @brief   Destructor.
     */
    virtual ~IBle() = default;

};

} /* namespace Interface */

#endif /* __INTERFACE_I_BLE_HPP */
