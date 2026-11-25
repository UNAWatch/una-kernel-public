/**
 ******************************************************************************
 * @file    DeviceInfo.hpp
 * @brief   Device information object.
 ******************************************************************************
 * This file contains the definition of the "DeviceInfo" structure, which
 * represents device information including hardware and software specifications.
 ******************************************************************************
 */

#ifndef __DEVICE_INFO_HPP
#define __DEVICE_INFO_HPP

#include <cstring>

/**
 * @brief Device information structure containing hardware and software specifications.
 * @note All strings are in ASCII format.
 */
struct DeviceInfo {

    /**
     * @brief   Default constructor.
     */
    DeviceInfo() = default;

    /**
     * @brief   Copy constructor. Declared because operator= is user-provided,
     *          which makes the implicit one deprecated.
     */
    DeviceInfo(const DeviceInfo &other) = default;

    /**
     * @brief   Constructor with parameters.
     * @param   name            Device name
     * @param   manufacturer    Device manufacturer
     * @param   id              Device identifier
     * @param   serialNumber    Device serial number
     * @param   swVersion       Software version
     * @param   hwVersion       Hardware version
     * @param   btVersion       Bluetooth version
     * @param   fccId           FCC identifier
     * @param   ce              CE marking information
     */
    DeviceInfo(const char *name, const char *manufacturer, const char *id,
               const char *serialNumber, const char *swVersion, const char *hwVersion,
               const char *btVersion, const char *fccId, const char *ce) :
               name(name), manufacturer(manufacturer), id(id), serialNumber(serialNumber),
               swVersion(swVersion), hwVersion(hwVersion), btVersion(btVersion),
               fccId(fccId), ce(ce)
    {
    }

    /**
     * @brief   Destructor.
     */
    virtual ~DeviceInfo() = default;

    const char* getName() const { return name; }
    const char* getManufacturer() const { return manufacturer; }
    const char* getId() const { return id; }
    const char* getSerialNumber() const { return serialNumber; }
    const char* getSwVersion() const { return swVersion; }
    const char* getHwVersion() const { return hwVersion; }
    const char* getBtVersion() const { return btVersion; }
    const char* getFccId() const { return fccId; }
    const char* getCe() const { return ce; }

    /**
     * @brief Equality operator to compare two device info objects.
     * @param other The other DeviceInfo object to compare.
     * @return 'true' if the device info objects are equal, 'false' otherwise.
     *
     * Equality is determined by comparing the device identifier and serial number.
     */
    bool operator==(const DeviceInfo &other) const
    {
        auto compareStrings = [](const char* a, const char* b) -> bool {
            if (a == nullptr && b == nullptr) return true;
            if (a == nullptr || b == nullptr) return false;
            return strcmp(a, b) == 0;
        };

        return compareStrings(id, other.id) && compareStrings(serialNumber, other.serialNumber);
    }

    /**
     * @brief Inequality operator to compare two device info objects.
     * @param other The other DeviceInfo object to compare.
     * @return 'true' if the device info objects are not equal, 'false' otherwise.
     */
    bool operator!=(const DeviceInfo &other) const
    {
        return !(*this == other);
    }

    /**
     * @brief Assignment operator to copy data from one DeviceInfo to another.
     * @param other The other DeviceInfo object to copy data from.
     * @return Reference to the current object after assignment.
     *
     * This operator performs a shallow copy of all fields, copying pointers to string literals.
     */
    DeviceInfo& operator=(const DeviceInfo &other)
    {
        if (this != &other) {
            name = other.name;
            manufacturer = other.manufacturer;
            id = other.id;
            serialNumber = other.serialNumber;
            swVersion = other.swVersion;
            hwVersion = other.hwVersion;
            btVersion = other.btVersion;
            fccId = other.fccId;
            ce = other.ce;
        }
        return *this;
    }

    const char *name = "";          ///< Device name or model designation.
    const char *manufacturer = "";  ///< Name of the device manufacturer.
    const char *id = "";            ///< Unique device identifier.
    const char *serialNumber = "";  ///< Device serial number for identification.
    const char *swVersion = "";     ///< Software/firmware version information.
    const char *hwVersion = "";     ///< Hardware version or revision information.
    const char *btVersion = "";     ///< Bluetooth protocol version supported by the device.
    const char *fccId = "";         ///< Federal Communications Commission identifier.
    const char *ce = "";            ///< CE (Conformite Europeenne) marking compliance information.
};

#endif // __DEVICE_INFO_HPP
