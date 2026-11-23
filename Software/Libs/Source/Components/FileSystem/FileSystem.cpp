/**
 ******************************************************************************
 * @file    FileSystem.cpp
 * @brief   FatFs wrapper.
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "FileSystem.hpp"

#include <cstring>
#include <ctime>
#include <cassert>

#if (FF_FS_NORTC == 0)
#include <ctime>
#endif // (FF_FS_NORTC == 0)

#define LOG_MODULE_PRX      "FileSystem"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include "File.hpp"
#include "Directory.hpp"

#include "System.hpp"
#include "flash_diskio.h"

#define FS_USE_EXFAT_DISK_LABELS     1

#if (FS_USE_EXFAT_DISK_LABELS == 1)
#include "exfat_helper.h"
// Defined in diskio.c
extern const char* skDiskLabel[];
#endif

namespace FS
{

char FileSystem::sVolumePath[skVolumes][3];

FileSystem::FileSystem()
{

    for (uint8_t i = 0; i < skVolumes; i++) {
        sprintf(sVolumePath[i], "%u:", i);
    }

    for (uint32_t i = 0; i < sizeof(mMutex) / sizeof(mMutex[0]); i++) {
        const osMutexAttr_t kMutAttributes = {
            .name = "FsMutex",
            .attr_bits = 0,
            .cb_mem = &mMutex[i].mMutControlBlock,
            .cb_size = sizeof(mMutex[i].mMutControlBlock)
        };

        mMutex[i].mMutHandle = osMutexNew(&kMutAttributes);
    }
}

FileSystem::~FileSystem()
{
    for (uint32_t i = 0; i < sizeof(mMutex) / sizeof(mMutex[0]); i++) {
        osMutexDelete(mMutex[i].mMutHandle);
    }
}

bool FileSystem::mount()
{
    bool isError = false;

    // Mount volumes..
    for (uint8_t i = 0; i < skVolumes; i++) {
        if(!mount(i)){
            isError = true;
        }
    }

    return !isError;
}

bool FileSystem::mount(uint8_t vol)
{
    return mountStatus(vol) == MountStatus::Ok;
}

FileSystem::MountStatus FileSystem::mountStatus(uint8_t vol)
{
    if (vol >= skVolumes) {
        return MountStatus::DiskError;
    }

    FRESULT fr;

    fr = f_mount(&mVolume[vol], sVolumePath[vol], 1);
    if (fr != FR_OK) {
        // Log the raw FatFs reason here (kept inside FileSystem so callers need
        // no FatFs types). Only a warning: the boot policy retries a disk error
        // and formats a blank volume, and emits the single loud error itself if
        // it ultimately gives up - so an eventually-successful mount stays quiet.
        LOG_WARNING("Mount '%s' failed. Err[%s]\n", sVolumePath[vol],
                ERRSTR(fr));
        return (fr == FR_NO_FILESYSTEM) ? MountStatus::Blank
                                        : MountStatus::DiskError;
    }

    // Print capacity and free space
    FATFS *fs = &mVolume[vol];

    DWORD fre_clust, fre_sect, tot_sect;
    DWORD ss = FF_MAX_SS;

    f_getfree(sVolumePath[vol], &fre_clust, &fs);

    // Get total sectors and free sectors
    tot_sect = (fs->n_fatent - 2) * fs->csize;
    fre_sect = fre_clust * fs->csize;

    LOG_DEBUG("'%s' mounted. Free space %u / %u kB\n", sVolumePath[vol],
            (fre_sect * ss) / 1024, (tot_sect * ss) / 1024);

    (void)fre_sect;
    (void)tot_sect;
    (void)ss;

    return MountStatus::Ok;
}

void FileSystem::unmount()
{
    FRESULT fr;
    for (uint32_t i = 0; i < skVolumes; i++) {
        fr = f_unmount(sVolumePath[i]);
        if (fr != FR_OK) {
            LOG_ERROR("Unmount '%s' failed. Err[%s]\n", sVolumePath[i],
                    ERRSTR(fr));
        } else {
            LOG_DEBUG("'%s' unmounted\n", sVolumePath[i]);
        }
    }
}

bool FileSystem::format(uint8_t vol)
{
    FRESULT fr;

    if (vol >= skVolumes) {
        return false;
    }

    MKFS_PARM param {};

    param.fmt = FM_EXFAT;
    param.n_fat = 2;
    param.align = 0;    // Auto
    param.n_root = 0;   // Not use for FAT32
    param.au_size = 4096;  // 4k for exFAT

#if (FF_MULTI_PARTITION == 1)
    DWORD plist[] = {100, 0, 0}; // 1 partition at 100%
    f_fdisk(vol, plist, mWorkBuffer); // creates MBR with 1 partition
#endif  // (FF_MULTI_PARTITION == 1)

    fr = f_mkfs(sVolumePath[vol], &param, mWorkBuffer, sizeof(mWorkBuffer));

    if (fr != FR_OK) {
        LOG_ERROR("Failed to format volume %s. Err[%s]\n",
                sVolumePath[vol], ERRSTR(fr));
        return false;
    }

#if (FS_USE_EXFAT_DISK_LABELS == 1)
    fr = f_mount(&mVolume[vol], sVolumePath[vol], 1);

    if (fr != FR_OK) {
        LOG_ERROR("Failed to mount volume %s. Err[%s]\n", sVolumePath[vol], ERRSTR(fr));
        return false;
    }

    if (!exfat_create_volume_label(vol, &mVolume[vol], skDiskLabel[vol])) {
        LOG_ERROR("Failed to create volume label\n");
        return false;
    }
#endif

    LOG_INFO("Volume %s formated\n", sVolumePath[vol]);

    return true;
}

bool FileSystem::factoryReset()
{
    LOG_WARNING("Factory reset...\n");

    // Formating
    bool isError = false;
    for (uint32_t i = 0; i < skVolumes; i++) {
        if (format(i) == false) {
            isError = true;
        }
    }
    LOG_INFO("Factory reset %s\n", isError ? "failed" : "finished");
    return !isError;
}

void FileSystem::printContent(const char *path)
{
    Directory dir(path);
    printContent(dir);
}

void FileSystem::printContent(Interface::IDirectory &dir)
{
    Interface::IFileSystem::ObjectInfo item {};

    LOG_INFO("Path %s\n", dir.getPath());

    if (dir.open()) {
        while (dir.readNext(item)) {

#if (FF_FS_NORTC == 0)
            // Get local time in string
            static char timeStr[64] {};
            std::tm localTime {};
            localtime_r(&item.utc, &localTime); // Takes into account timezone and DST
            std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &localTime);

            LOG_INFO("%s %7ld %s%s\n", timeStr, item.size, item.name,
                    item.isDir ? "/" : "");
#else
            LOG_INFO("%7ld %s%s\n", item.size, item.name,
                    item.isDir ? "/" : "");
#endif  // (FF_FS_NORTC == 0)
        }
        dir.close();
    }
}

#if (FF_FS_NORTC == 0)
time_t FileSystem::FatToUtc(uint32_t fatTime)
{
    uint32_t year = (fatTime >> 25) + 1980;   // year shifted to 1980
    uint32_t month = (fatTime >> 21) & 0xF;    // month from 1 to 12
    uint32_t day = (fatTime >> 16) & 0x1F;   // day from 1 to 31
    uint32_t hour = (fatTime >> 11) & 0x1F;   // hour from 0 to 23
    uint32_t minute = (fatTime >> 5) & 0x3F;   // minute from 0 to 59
    uint32_t second = (fatTime & 0x1F) * 2; // a second from 0 to 58, a multiple of two

    struct tm dateTime {};
    dateTime.tm_year = year - 1900;     // year shifted to 1900
    dateTime.tm_mon = month - 1;        // month from 0 to 11
    dateTime.tm_mday = day;
    dateTime.tm_hour = hour;
    dateTime.tm_min = minute;
    dateTime.tm_sec = second;

    return mktime(&dateTime);
}

uint32_t FileSystem::UtcToFat(time_t utc)
{
    struct tm dateTime {};

    gmtime_r(&utc, &dateTime);

    uint32_t year = dateTime.tm_year + 1900; // the year is shifted to -1900
    uint32_t month = dateTime.tm_mon + 1; // month from 1 to 12
    uint32_t day = dateTime.tm_mday;
    uint32_t hour = dateTime.tm_hour;
    uint32_t minute = dateTime.tm_min;
    uint32_t second = dateTime.tm_sec;

    uint32_t fatTime = ((year - 1980) << 25) | (month << 21) | (day << 16)
                       | (hour << 11) | (minute << 5) | (second / 2);

    return fatTime;
}

int FileSystem::TimezoneOffsetSeconds(uint8_t tzByte)
{
    if (tzByte == 0x80) {
        // Special case for UTC
        return 0;
    }

    int8_t offset = (int8_t)tzByte;

    if ((tzByte & 0xC0) == 0x80) { // Positive
        offset = (int8_t)(tzByte & 0x3F);
    }

    // Calculate the displacement in seconds
    return offset * 15 * 60;
}
#endif // (FF_FS_NORTC == 0)

bool FileSystem::mkdir(const char *path)
{
    if (path == nullptr) {
        return false;
    }

    const char *p = path;

    char pathTmp[FF_MAX_LFN + 1] {};

    do {
        p = strchr(p, '/');
        int32_t len = 0;
        if (p == nullptr) {
            len = strlen(path);
        } else {
            len = (int32_t)(p - path);
        }

        // Copy path
        memset(pathTmp, 0, sizeof(pathTmp));
        strncpy(&pathTmp[0], path, len);

        FRESULT fr = f_mkdir(pathTmp);

        if (fr != FR_OK && fr != FR_EXIST) {
            LOG_ERROR("Failed to create dir '%s' Err[%s]\n", pathTmp,
                    ERRSTR(fr));
            return false;
        }

        while (p && *p == '/') {
            p++;
        }
    } while (p && *p != '\0');

    return true;
}

std::unique_ptr<Interface::IFile> FileSystem::file(const char *path)
{
    if (!path) {
        return nullptr;
    }

    std::unique_ptr<FS::File> ptr {new (std::nothrow) FS::File(path)};
    return ptr;
}

std::unique_ptr<Interface::IDirectory> FileSystem::dir(const char *path)
{
    if (!path) {
        return nullptr;
    }

    std::unique_ptr<Directory> ptr {new (std::nothrow) Directory(path)};
    return ptr;
}

bool FileSystem::exist(const char *path) const
{
    assert(path != nullptr);

    FRESULT fr = FR_DISK_ERR;
    FILINFO fno {};

    fr = f_stat(path, &fno);

    if (fr != FR_OK) {
        if (fr != FR_NO_FILE && fr != FR_NO_PATH) {
            LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), path);
        }
        return false;
    }

    return true;
}

bool FileSystem::remove(const char *path)
{
    assert(path != nullptr);

    FRESULT fr = FR_DISK_ERR;

    fr = f_unlink(path);

    if (fr != FR_OK) {
        LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), path);
        return false;
    }

    return true;
}

bool FileSystem::rename(const char *oldPath, const char *newPath)
{
    assert(oldPath != nullptr);
    assert(newPath != nullptr);

    FRESULT fr = FR_DISK_ERR;

    fr = f_rename(oldPath, newPath);

    LOG_DEBUG("Rename '%s' >> '%s'\n", oldPath, newPath);
    if (fr != FR_OK) {
        LOG_ERROR("Error renaming '%s' to '%s' Err [%s]\n", oldPath, newPath,
                ERRSTR(fr));
        return false;
    }

    return true;
}

bool FileSystem::copy(const char *oldPath, const char *newPath)
{
    assert(oldPath != nullptr);
    assert(newPath != nullptr);

    FRESULT fr = FR_DISK_ERR;

    FIL src_file, dest_file;
    UINT br, bw;
    BYTE buffer[512];

    fr = f_open(&src_file, oldPath, FA_READ);
    if (fr != FR_OK) {
        LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), oldPath);
        return false;
    }

    fr = f_open(&dest_file, newPath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), newPath);
        f_close(&src_file);
        return false;
    }

    do {
        fr = f_read(&src_file, buffer, sizeof(buffer), &br);
        if (fr != FR_OK) {
            LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), oldPath);
            break;
        }

        if (br == 0) {
            break;
        }

        fr = f_write(&dest_file, buffer, br, &bw);
        if (fr == FR_OK && bw < br) {
            // A short write with no error is FatFs saying the volume is full:
            // a failure, or the copy would end truncated and report success.
            fr = FR_DENIED;
        }
        if (fr != FR_OK) {
            LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), newPath);
            break;
        }
    } while (br > 0);

    f_close(&src_file);
    f_close(&dest_file);

    if (fr == FR_OK) {
        LOG_DEBUG("Copy '%s' >> '%s'\n", oldPath, newPath);
    }

    return fr == FR_OK;
}

void FileSystem::fillObjectInfo(ObjectInfo &item, const FILINFO &fno)
{
    strcpy(item.name, fno.fname);
    // Attributes are a bitfield: test each bit independently so combinations
    // (hidden/system directories, the near-ubiquitous archive bit on files)
    // are classified correctly.
    item.isDir    = (fno.fattrib & AM_DIR) != 0;
    item.isHidden = (fno.fattrib & AM_HID) != 0;
    item.isSystem = (fno.fattrib & AM_SYS) != 0;
    item.size     = fno.fsize;

#if (FF_FS_NORTC == 0)
    item.utc = FatToUtc(fno.fdate << 16 | fno.ftime);
#endif // (FF_FS_NORTC == 0)
}

bool FileSystem::objectInfo(const char *path, ObjectInfo &item) const
{
    assert(path != nullptr);

    FRESULT fr = FR_NOT_READY;
    FILINFO fno {};

    fr = f_stat(path, &fno);

    if (fr != FR_OK) {
        if (fr != FR_NO_FILE) {
            LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), path);
        }
        return false;
    }

    if (strlen(fno.fname) == 0) {
        return false;
    }

    fillObjectInfo(item, fno);
    // TODO: fix TZ - f_stat gives no directory object, so the exFAT timezone
    // adjustment done in Directory::readNext cannot be applied here. We expect
    // the time to be in UTC, but this is not always the case.

    return true;
}

osMutexId_t FileSystem::getMutex(int vol)
{
    assert(vol < (int)(sizeof(mMutex) / sizeof(mMutex[0])));

    // Collapse all per-volume mutexes onto a single shared volume mutex
    // (index 0); keep the system mutex (index skVolumes) separate. The volumes
    // live on one eMMC, so this serializes nothing the hardware did not already
    // serialize. It closes a cross-volume race on FatFs's global SysLock flag:
    // with distinct per-volume mutexes a syslock=0 op (f_read/f_write/f_sync) on
    // one volume can run concurrently with a syslock=1 op (f_open/f_unlink/...)
    // on another, observe the SysLock=2 the latter set, and release the system
    // mutex it never took, tripping xTaskPriorityDisinherit. A single volume
    // mutex makes those ops mutually exclusive, so SysLock cannot change under a
    // syslock=0 op while it holds the lock. lock_volume still takes the volume
    // mutex before the (distinct) system mutex, so there is no self-deadlock.
    if (vol < (int)skVolumes) {
        return mMutex[0].mMutHandle;
    }
    return mMutex[skVolumes].mMutHandle;
}

} /* namespace FS */


extern "C" DWORD get_fattime()
{
#if (FF_FS_NORTC == 0)
    std::time_t utc = time(nullptr);
    return FS::FileSystem::UtcToFat(utc);
#else
    return 0;
#endif // (FF_FS_NORTC == 0)
}

#if (FF_FS_REENTRANT == 1)
/**
 * @brief   ff_mutex_create
 * @param   vol: Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system
 *          mutex (FF_VOLUMES).
 * @retval  1:Function succeeded or 0:Could not create the mutex.
 */
extern "C" int ff_mutex_create(int vol)
{
    return 1;
}

/**
 * @brief   ff_mutex_delete
 * @param   vol: Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system
 *          mutex (FF_VOLUMES).
 * @retval  1:Function succeeded or 0:Could not delete due to an error.
 */
extern "C" void ff_mutex_delete(int vol)
{
}

/**
 * @brief   ff_mutex_take
 * @param   vol: Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system
 *          mutex (FF_VOLUMES).
 * @retval  1:Succeeded or 0:Timeout.
 */
extern "C" int ff_mutex_take(int vol)
{
    osStatus_t status = osError;
    FS::FileSystem &fs = FS::FileSystem::GetInstance();
    status = osMutexAcquire(fs.getMutex(vol), pdMS_TO_TICKS(FF_FS_TIMEOUT));

    return (int) (status == osOK);
}

/**
 * @brief   ff_mutex_give
 * @param   vol: Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system
 *          mutex (FF_VOLUMES).
 */
extern "C" void ff_mutex_give(int vol)
{
    osMutexRelease(FS::FileSystem::GetInstance().getMutex(vol));
}
#endif // (FF_FS_REENTRANT == 1)

#if (FF_USE_LFN == 3)
#include <stdlib.h>

/**
 * @brief   Allocate a new memory block.
 * @param   msize: Number of bytes to allocate.
 * @retval  Pointer to the allocated memory block (null if not enough core).
 */
extern "C" void* ff_memalloc(UINT msize)
{
    return malloc((size_t) msize);
}

/**
 * @brief   Free the memory block .
 * @param   mblock: Pointer to the memory block to free (no effect if null).
 */
extern "C" void ff_memfree(void *mblock)
{
    free(mblock);
}
#endif // (FF_USE_LFN == 3)
