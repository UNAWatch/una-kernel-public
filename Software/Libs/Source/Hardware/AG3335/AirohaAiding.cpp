/**
 ******************************************************************************
 * @file    Aiding.cpp
 * @brief   Helper class for retrieving time, location andEPO files from the
 *          file system.
 ******************************************************************************
 *
 ******************************************************************************
 */


#include "AG3335/AirohaAiding.hpp"

#define LOG_MODULE_PRX      "Airoha.Aiding"
#define LOG_MODULE_LEVEL    LOG_LEVEL_WARNING
#include "Logger.h"



#include <cstring>
#include <cassert>

namespace Airoha
{

namespace {

/// EpoReader over an open file. Every read seeks first: the parser asks for
/// the header, then walks the segments, and a caller may read the same range
/// twice (EpoParser::selectSegments counts, then copies).
class FileEpoReader final : public EpoReader {
public:
    explicit FileEpoReader(Interface::IFile &file) : mFile(file) {}

    size_t size() const override { return mFile.size(); }

    bool read(size_t offset, uint8_t *dst, size_t n) override
    {
        size_t br = 0;
        return mFile.seek(offset)
                && mFile.read(reinterpret_cast<char*>(dst), n, br)
                && br == n;
    }

private:
    Interface::IFile &mFile;
};

/// An EPO capsule: a file, not a directory, named *.DAT.
bool IsEpoFile(const Interface::IFileSystem::ObjectInfo &item)
{
    std::string_view fileExt = PS_EPO_FILE_EXTENSION;
    if (item.isDir) {
        return false;
    }
    std::string_view fileName = item.name;

    return fileName.size() >= fileExt.size()
            && fileName.substr(fileName.size() - fileExt.size()) == fileExt;
}

} // namespace

Aiding::Aiding(Interface::IRtc &rtc, Interface::IFileSystem &fs, const char *epoDir)
{
    assert(epoDir != nullptr);
    config(rtc, fs, epoDir);
}

Aiding::~Aiding()
{

}

bool Aiding::config(Interface::IRtc &rtc, Interface::IFileSystem &fs, const char* epoDir)
{
    assert(epoDir != nullptr);
    mFs = &fs;
    mRtc = &rtc;
    mDir = mFs->dir(epoDir);
    mFile = mFs->file("");

    if (!mEpoScanLock.init("AidingEpo")) {
        LOG_ERROR("Failed to create the EPO scan mutex\n");
        return false;
    }

    return true;
}

Aiding::EpoPeriod Aiding::getEpoTimeRange()
{
    if (!mEpoScanLock.lock(kEpoScanTimeoutMs)) {
        LOG_WARNING("EPO scan busy, reporting no time range\n");
        return {};
    }

    EpoPeriod timeRange = scanEpoTimeRangeLocked();

    mEpoScanLock.release();

    return timeRange;
}

Aiding::EpoPeriod Aiding::scanEpoTimeRangeLocked()
{
    if (mDir == nullptr || mFile == nullptr || !mDir->exist() || !mDir->open()) {
        return {};
    }

    EpoPeriod timeRange {};

    while (mDir->readNext(mItem)) {
        if (!IsEpoFile(mItem)) {
            continue;
        }

        char path[Interface::IFileSystem::skMaxPathLen] {};
        snprintf(path, sizeof(path), "%s%s", mDir->getPath(), mItem.name);
        mFile->setPath(path);

        LOG_INFO("File: [%s]\n", mFile->getPath());

        if (!mFile->open()) {
            LOG_ERROR("Failed to open file [%s]\n", mFile->getPath());
            continue;
        }

        // Only the start and the end are wanted, and the parser reads no more
        // of the file than it needs to work them out.
        FileEpoReader reader(*mFile);
        EpoParser p;

        if (p.init(reader, mScratch, sizeof(mScratch))) {
            auto seveRange = [](EpoPeriod::Range &range, uint32_t s, uint32_t e) {
                if (range.start == 0) {
                    range.start = s;
                } else {
                    range.start = std::min(range.start, s);
                }
                range.end = std::max(range.end, e);
            };

            uint32_t s = 0;
            uint32_t e = 0;
            p.getStartTime(s);
            p.getEndTime(e);
            LOG_DEBUG("File: [%s]. Sys %u, Range: %u - %u\n", mFile->getPath(), p.getSystemType(), s, e);

            if (p.isSystemType(SystemType::GPS)) {
                seveRange(timeRange.gps, s, e);
            } else if (p.isSystemType(SystemType::GLONASS)) {
                seveRange(timeRange.glonass, s, e);
            } else if (p.isSystemType(SystemType::GALILEO)) {
                seveRange(timeRange.galileo, s, e);
            } else if (p.isSystemType(SystemType::BEIDOU)) {
                seveRange(timeRange.beidou, s, e);
            } else if (p.isSystemType(SystemType::QZSS)) {
                seveRange(timeRange.qzss, s, e);
            }
        }

        mFile->close();
    }

    // Cleanup
    mDir->close();

    return timeRange;
}

void Aiding::setReferenceLocation(float lat, float lon, float alt)
{
    onAidingRefLocationSave(lat, lon, alt);
}

bool Aiding::getReferenceLocation(float& lat, float& lon, float& alt)
{
    ReferenceLocation location;

    if (!onAidingRefLocationRequest(location)) {
        return false;
    }

    lat = location.latitude;
    lon = location.longitude;
    alt = location.height;

    return true;
}

bool Aiding::onAidingRefTimeRequest(std::tm &utc)
{
    if (mRtc == nullptr) {
        return false;
    }
    Interface::IRtc::DateTime rtctm = mRtc->get();
    if (rtctm.time_reliable) {
        utc = rtctm;
        return true;
    }

    return false;
}

bool Aiding::onAidingRefLocationRequest(ReferenceLocation &loc)
{
    if (mRtc == nullptr) {
        return false;
    }

    uint32_t tmp = 0;
    uint32_t utc = mRtc->readBackupReg(RTC_BRK_REG_GPS_REF_UTC);

    tmp = mRtc->readBackupReg(RTC_BRK_REG_GPS_REF_LAT);
    float lat = 0;
    std::memcpy(&lat, &tmp, sizeof(lat));

    tmp = mRtc->readBackupReg(RTC_BRK_REG_GPS_REF_LON);
    float lon = 0;
    std::memcpy(&lon, &tmp, sizeof(lon));

    tmp = mRtc->readBackupReg(RTC_BRK_REG_GPS_REF_ALT);
    float alt = 0;
    std::memcpy(&alt, &tmp, sizeof(alt));

    uint32_t crc = mRtc->readBackupReg(RTC_BRK_REG_GPS_REF_CRC);

    if (utc == crc) {
        // TODO: Process time

        loc.latitude = lat;
        loc.longitude = lon;
        loc.height = alt;
        LOG_DEBUG("lat %f, lon %f, alt %f\n", lat, lon, alt);
        return true;
    }

    return false;
}

void Aiding::onAidingRefLocationSave(float lat, float lon, float alt)
{
    if (mRtc == nullptr) {
        return;
    }

    if (isUnsetReferenceLocation(lat, lon, alt)) {
        // Mark location as invalid
        mRtc->writeBackupReg(RTC_BRK_REG_GPS_REF_CRC, 0);
        return;
    }

    uint32_t tmp = 0;
    uint32_t time = static_cast<uint32_t>(std::time(nullptr));

    mRtc->writeBackupReg(RTC_BRK_REG_GPS_REF_UTC, time);

    std::memcpy(&tmp, &lat, sizeof(tmp));
    mRtc->writeBackupReg(RTC_BRK_REG_GPS_REF_LAT, tmp);

    std::memcpy(&tmp, &lon, sizeof(tmp));
    mRtc->writeBackupReg(RTC_BRK_REG_GPS_REF_LON, tmp);

    std::memcpy(&tmp, &alt, sizeof(tmp));
    mRtc->writeBackupReg(RTC_BRK_REG_GPS_REF_ALT, tmp);

    mRtc->writeBackupReg(RTC_BRK_REG_GPS_REF_CRC, time);

    LOG_DEBUG("lat %f, lon %f, alt %f\n", lat, lon, alt);
}

EpoData Aiding::onAidingEpoRequest(SystemType sys, uint32_t week, uint32_t tow)
{
    if (!mEpoScanLock.lock(kEpoScanTimeoutMs)) {
        LOG_WARNING("EPO scan busy, no aiding data for sys %u\n",
                static_cast<unsigned>(sys));
        return {};
    }

    EpoData epo = readEpoFileLocked(sys, week, tow);

    mEpoScanLock.release();

    return epo;
}

EpoData Aiding::readEpoFileLocked(SystemType sys, uint32_t week, uint32_t tow)
{
    if (mDir == nullptr || mFile == nullptr || !mDir->exist() || !mDir->open()) {
        return {};
    }

    EpoData epo;

    // Looking for correct file
    while (mDir->readNext(mItem)) {
        if (!IsEpoFile(mItem)) {
            continue;
        }

        char path[Interface::IFileSystem::skMaxPathLen] {};
        snprintf(path, sizeof(path), "%s%s", mDir->getPath(), mItem.name);
        mFile->setPath(path);

        LOG_INFO("File: [%s]\n", mFile->getPath());

        if (!mFile->open()) {
            LOG_ERROR("Failed to open file [%s]\n", mFile->getPath());
            continue;
        }

        FileEpoReader reader(*mFile);
        EpoParser p;

        if (!p.init(reader, mScratch, sizeof(mScratch))
                || !p.isSystemType(sys) || !p.coversTime(week, tow)) {
            mFile->close();
            continue;
        }

        uint32_t SWeek = 0;
        uint32_t STow = 0;
        uint32_t EWeek = 0;
        uint32_t ETow = 0;
        p.getStartTime(SWeek, STow);
        p.getEndTime(EWeek, ETow);

        LOG_INFO("Request %u,%u. File: [%s] Start: %u,%u, End: %u,%u\n",
                week, tow, mFile->getPath(), SWeek, STow, EWeek, ETow);

        // Only the current window is copied out - one 6-hour block, about
        // 2.3 KB for GPS - and the rest of the file is read past, not kept.
        // EPO aiding is optional, so a heap that cannot serve even that means
        // no aiding this time round.
        const bool selected = p.selectSegments(sys, week, tow, epo.segments);
        if (!selected) {
            LOG_ERROR("Failed to read the EPO segments from [%s], skipping EPO injection\n",
                    mFile->getPath());
        }

        if (selected && p.getHeader() != nullptr) {
            std::memcpy(epo.header, p.getHeader(), sizeof(epo.header));
            epo.hasHeader = true;
        }

        mFile->close();
        mDir->close();

        if (!selected) {
            return {};
        }
        return epo;
    }

    // Cleanup
    mDir->close();

    return {};
}

}  /* namespace Airoha */

