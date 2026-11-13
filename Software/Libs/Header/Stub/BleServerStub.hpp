/**
 ******************************************************************************
 * @file    BleServerStub.hpp
 * @brief   Stub BLE component. This is used when the system cannot find or
 *          recognize the actual BLE module.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __STUB_BLE_SERVER_HPP
#define __STUB_BLE_SERVER_HPP

#include "IBle.hpp"

namespace Stub
{

class BleServer : public Interface::IBle {

public:


    BleServer() = default;

    virtual ~BleServer() = default;

    virtual bool init() override { return false; }

    virtual bool deinit() override { return false; }

    virtual bool enable() override { return false; }

    virtual bool disable() override { return false; }

    virtual bool isEnabled() override { return false; }

    virtual bool isAlive() override { return false; }

    virtual void setName(const char *name) override {}

    virtual Ble::ServiceHandle addService(const Ble::UUID &uuid)
            override { return nullptr; }

    virtual bool delService(Ble::ServiceHandle handle) { return false; }

    virtual Ble::CharacteristicHandle addCharacteristic(Ble::ServiceHandle srvHandle,
            const Ble::UUID &uuid, uint16_t maxlen, uint8_t properties)
            override { return nullptr; }

    virtual bool delCharacteristic(Ble::CharacteristicHandle handle) override { return false; }

    virtual Ble::ServiceHandle getServiceByUUID(const Ble::UUID &uuid)
            override { return nullptr; }

    virtual Ble::UUID getServiceUUID(Ble::ServiceHandle handle)
            override { Ble::UUID uuid {}; return uuid; }

    virtual Ble::CharacteristicHandle getCharacteristicByUUID(
            Ble::ServiceHandle srvHandle, const Ble::UUID &uuid)
            override { return nullptr; }

    virtual Ble::UUID getCharacteristicUUID(Ble::CharacteristicHandle handle)
            override { Ble::UUID uuid {}; return uuid; }

    virtual uint32_t getMaxCharacteristicLen() override { return 23; }

    virtual bool setCharacteristicValue(Ble::CharacteristicHandle handle,
            const uint8_t *pData, uint8_t len) override { return false; }

    virtual void attachStatusCallback(Interface::IBle::StatusCallback *pCallback) override {}

    virtual void detachStatusCallback(Interface::IBle::StatusCallback *pCallback) override {}

    virtual void attachCharacteristicCallback(Ble::CharacteristicHandle handle,
            Interface::IBle::CharacteristicCallback *pCallback) override {}

    virtual bool pair() override { return false; }

    virtual bool unpair() override { return false; }

    virtual bool isBonded() override { return false; }

    virtual uint8_t getBondedCount() override { return 0; }

    virtual bool startAdvertising() override { return false; }

    virtual bool stopAdvertising() override { return false; }

    virtual bool isAdvertise() override { return false; }

    virtual bool disconnect() override { return false; }

    virtual bool isConnected() override { return false; }

    virtual bool isTransferQuiet(uint32_t /*quietMs*/) const override { return true; }

protected:

private:

};

} /* namespace Stub */

#endif /* __STUB_BLE_SERVER_HPP */
