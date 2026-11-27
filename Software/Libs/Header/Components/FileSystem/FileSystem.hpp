/**
 ******************************************************************************
 * @file    FileSystem.hpp
 * @brief   FatFs wrapper.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __FILE_SYSTEM_HPP
#define __FILE_SYSTEM_HPP

#include "ff.h"

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "main.h"

#include "IFileSystem.hpp"


namespace FS
{

/**
 * @brief   Errors description.
 */
static const char *skFsResultToStr[] __attribute__((used)) = {
    "0, FR_OK, Succeeded",
    "1, FR_DISK_ERR, A hard error occurred in the low level disk I/O layer",
    "2, FR_INT_ERR, Assertion failed",
    "3, FR_NOT_READY, The physical drive cannot work ",
    "4, FR_NO_FILE, Could not find the file",
    "5, FR_NO_PATH, Could not find the path",
    "6, FR_INVALID_NAME, The path name format is invalid",
    "7, FR_DENIED, Access denied due to prohibited access or directory full",
    "8, FR_EXIST, Access denied due to prohibited access",
    "9, FR_INVALID_OBJECT, The file/directory object is invalid",
    "10, FR_WRITE_PROTECTED, The physical drive is write protected",
    "11, FR_INVALID_DRIVE, The logical drive number is invalid",
    "12, FR_NOT_ENABLED, The volume has no work area",
    "13, FR_NO_FILESYSTEM, There is no valid FAT volume",
    "14, FR_MKFS_ABORTED, The f_mkfs() aborted due to any problem",
    "15, FR_TIMEOUT, Could not get a grant to access the volume within defined period",
    "16, FR_LOCKED, The operation is rejected according to the file sharing policy",
    "17, FR_NOT_ENOUGH_CORE, LFN working buffer could not be allocated",
    "18, FR_TOO_MANY_OPEN_FILES, Number of open files > FF_FS_LOCK",
    "19, FR_INVALID_PARAMETER Given parameter is invalid",
};

#define ERRSTR(err) skFsResultToStr[err]

/**
 * @brief   %File system.
 */
class FileSystem : public Interface::IFileSystem
{
public:

    /**
     * @brief   Outcome of a mount attempt, hiding the raw FatFs FRESULT.
     * @details Exposes only the distinction the boot policy needs, so callers
     *          never switch on FatFs codes:
     *          - Ok        : volume mounted.
     *          - Blank     : no valid filesystem (FR_NO_FILESYSTEM) - genuinely
     *                        blank, safe to format.
     *          - DiskError : any other non-OK FatFs result (FR_DISK_ERR /
     *                        FR_NOT_READY / FR_TIMEOUT / ...) - typically a
     *                        transient the caller should retry, NOT reformat.
     */
    enum class MountStatus
    {
        Ok,
        Blank,
        DiskError,
    };

    /**
     * @brief   Get FileSystem instance.
     */
    static FileSystem& GetInstance()
    {
        static FileSystem sInstance;
        return sInstance;
    }

    /**
     * @brief   Initialize and mount file system.
     * @retval  Execution status. 'true' - if success, 'false' - otherwise.
     */
    bool mount();

    /**
     * @brief   Mount file system on specified volume.
     * @param   vol: Volume to format. (0 .. FF_VOLUMES-1)
     * @retval  Execution status. 'true' - if success, 'false' - otherwise.
     */
    bool mount(uint8_t vol);

    /**
     * @brief   Mount a volume and report the outcome as a MountStatus.
     * @details Lets the caller distinguish a genuinely-blank volume
     *          (MountStatus::Blank, safe to format) from a transient disk error
     *          (MountStatus::DiskError) that must NOT trigger a reformat,
     *          without leaking FatFs FRESULT codes. The raw FatFs reason is
     *          logged here (inside FileSystem) so callers need no FatFs types.
     *          The forced mount (opt=1) surfaces disk errors immediately.
     *          bool mount(vol) is a thin wrapper over this.
     * @param   vol: Volume to mount. (0 .. FF_VOLUMES-1)
     * @retval  MountStatus (Ok on success; DiskError if vol is out of range).
     */
    MountStatus mountStatus(uint8_t vol);

    /**
     * @brief   Unmount file system.
     */
    void unmount();

    /**
     * @brief   Hand a volume over to the USB host.
     * @details From here until reclaim(), every FatFs access to the volume
     *          fails with FR_NOT_READY (FR_INVALID_OBJECT for an object opened
     *          before), so the firmware cannot write it while the host does.
     *          Returns once no FatFs call that was already running on the
     *          volume is still running. Close the files that should survive
     *          before calling this: their unsaved data is lost.
     * @param   vol: Volume to hand over. (0 .. FF_VOLUMES-1)
     * @retval  'true' once the volume is the host's. 'false' if a call already
     *          running on it did not finish in time; the volume then stays the
     *          firmware's and must not be handed to the host.
     */
    bool release(uint8_t vol);

    /**
     * @brief   Take a volume back from the USB host.
     * @details FatFs reads the volume again on its first access after this,
     *          so it starts from what the host left rather than from what it
     *          had cached. Call it only once the host can no longer write the
     *          volume. Safe to call for a volume that was not handed over.
     * @param   vol: Volume to take back. (0 .. FF_VOLUMES-1)
     */
    void reclaim(uint8_t vol);

    /**
     * @brief   Get number of the volumes.
     * @retval  Number of the drives.
     */
    static uint8_t GetVolumesNum()
    {
        return skVolumes;
    }

    /**
     * @brief   Format volume.
     * @param   vol: Volume to format. (0 .. FF_VOLUMES-1)
     * @retval  Execution status. 'true' - if success, 'false' - otherwise.
     */
    bool format(uint8_t vol);

    /**
     * @brief   Create Partition Table on the Physical volumes and format
     *          logical drives.
     * @note    USE WITH CAUTION! This will delete all data.
     * @retval  Execution status. 'true' - if success, 'false' - otherwise.
     */
    bool factoryReset();

    /**
     * @brief   Get volume path.
     * @param   vol: Volume. (0 .. FF_VOLUMES-1)
     * @retval  Pointer to volume for the specified file type (like 0:).
     */
    static const char* GetVolumePath(uint8_t vol)
    {
        if (vol >= skVolumes) {
            return "";
        }
        return sVolumePath[vol];
    }

    /**
     * @brief   Helper method for print content of directory.
     * @param   path: Path to the directory.
     */
    void printContent(const char *path);

    /**
     * @brief   Helper method for print content of directory.
     * @param   dir: Directory object.
     */
    void printContent(Interface::IDirectory &dir);

#if (FF_FS_NORTC == 0)
    // TODO: Check this!
    /*
     NOTE:

     The FAT32 and exFAT file systems store file timestamps in local time but
     include a time zone field for accurate time calculations. However, FatFs
     does not account for the time zone when saving files or reading their
     metadata. Therefore, to ensure the correct display of file creation times
     on a PC, you must use UTC time, as the time zone will be set to 0 during
     the saving process.

     A potential issue arises if a file or directory is created on the PC
     (via a Mass Storage Device) and later accessed through FatFs. In this
     scenario, the time zone information will be lost, and the local time will
     be displayed as UTC.

     To avoid this issue, we try to get the timezone byte from FatFs and use
     the TimezoneOffsetSeconds() method to adjust the timestamp.
     */

    /**
     * @brief   Convert FATFS time to UTC.
     * @param   fatTime: time in FAT format.
     * @retval  Time in UTC format (seconds since epoch).
     */
    static time_t FatToUtc(uint32_t fatTime);

    /**
     * @brief   Populate an ObjectInfo from a FatFs FILINFO.
     * @details Single source of truth for decoding a directory entry into an
     *          ObjectInfo (name, attributes, size, UTC). Attribute flags are
     *          tested bitwise, so an entry that carries more than one bit
     *          (e.g. a hidden system directory, or any file with the archive
     *          bit set) is classified correctly. Callers that need the exFAT
     *          timezone adjustment apply it on top of item.utc afterwards.
     * @param   item: destination, fully overwritten.
     * @param   fno:  source FatFs file info.
     */
    static void fillObjectInfo(ObjectInfo& item, const FILINFO& fno);

    /**
     * @brief   Convert Date and Time to FATFS format.
     * @param   uts: Time in UTC format (seconds since epoch).
     * @retval  Time in FAT format.
     */
    static uint32_t UtcToFat(time_t utc);

    /**
     * @brief   Parses the time zone byte from exFAT and returns the offset in
     *          seconds from UTC.
     *
     * This function interprets the given byte, which stores the time zone offset
     * in 15-minute intervals relative to UTC. It accounts for both positive and
     * negative offsets.
     * The special value 0x80 is treated as UTC (0 seconds offset).
     *
     * @param   tzByte: The byte representing the time zone offset in exFAT.
     * @retval int The offset in seconds from UTC.
     */
    static int TimezoneOffsetSeconds(uint8_t tzByte);

#endif // (FF_FS_NORTC == 0)

    /**
     * @copydoc Interface::IFileSystem::mkdir()
     */
    virtual bool mkdir(const char *path) override;

    /**
     * @copydoc Interface::IFileSystem::file()
     */
    virtual std::unique_ptr<Interface::IFile> file(const char *path) override;

    /**
     * @copydoc Interface::IFileSystem::dir()
     */
    virtual std::unique_ptr<Interface::IDirectory> dir(const char *path) override;

    /**
     * @brief   Checks if a file/directory exists.
     * @param   path: Path to the file (0:/dir1/ or 0:/file.txt).
     * @retval  'true' if the file exists, 'false' otherwise.
     */
    virtual bool exist(const char *path) const override;

    /**
     * @brief   Removes a file or directory.
     * @param   path: Path to the file or directory (0:/dir1/ or 0:/file.txt).
     * @retval  'true' if the item was successfully removed, 'false' otherwise.
     */
    virtual bool remove(const char *path) override;

    /**
     * @brief   Renames a file or directory.
     * @param   oldPath: Current path of the item (0:/dir1/ or 0:/file.txt).
     * @param   newPath: New path for the item (0:/dir1/ or 0:/file.txt).
     * @retval  'true' if the item was successfully renamed, 'false' otherwise.
     */
    virtual bool rename(const char *oldPath, const char *newPath) override;

    /**
     * @brief   Copy a file to another location.
     * @param   oldPath: Current path of the item (0:/file.txt).
     * @param   newPath: New path for the item (2:/file_copy.txt).
     * @retval  'true' if the item was successfully moved, 'false' otherwise.
     */
    virtual bool copy(const char *oldPath, const char *newPath) override;

    /**
     * @brief   Get object info.
     * @param   path: Path to the directory (0:/dir1/ or 0:/file.txt).
     * @param   name: Reference to save item info.
     * @retval  Execution status. 'true' - if success, 'false' - otherwise.
     */
    virtual bool objectInfo(const char *path, ObjectInfo &item) const override;

    FATFS* getFatFS(uint8_t volumeIndex)
    {
        if (volumeIndex >= skVolumes) {
            return NULL;
        }

        return &mVolume[volumeIndex];
    }

private:

    static const uint8_t skVolumes = FF_VOLUMES;

    // Volume objects
    FATFS mVolume[skVolumes] {};

    // Volume paths like "0:", "1:"
    static char sVolumePath[skVolumes][3];

    // Working buffer for a sector
    uint8_t mWorkBuffer[FF_MAX_SS] {};

    // Mutexes for each volume and system
    typedef struct {
        osMutexId_t mMutHandle {};
        StaticSemaphore_t mMutControlBlock {};
    } Mutex_t;
    Mutex_t mMutex[skVolumes + 1] {};

    /**
     * @brief   Constructor.
     */
    FileSystem();

    /**
     * @brief   Destructor.
     */
    virtual ~FileSystem();

    /**
     * @brief   Get mutex object for FatFs lock C-functions.
     * @param   vol: Mutex ID: Volume mutex (0 to FF_VOLUMES - 1) or system
     * @retval  Mutex for target volume or system.
     */
    osMutexId_t getMutex(int vol);

    friend int ::ff_mutex_take(int vol);
    friend void ::ff_mutex_give(int vol);
};

} /* namespace FS */

#endif /* __FILE_SYSTEM_HPP */
