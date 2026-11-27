/**
 ******************************************************************************
 * @file    Directory.cpp
 * @brief   Directory object.
 ******************************************************************************
 *
 ******************************************************************************
 */

#include "Directory.hpp"

#include <cstdio>
#include <cstring>
#include <cassert>

#define LOG_MODULE_PRX      "Directory"
#define LOG_MODULE_LEVEL    LOG_LEVEL_INFO
#include "Logger.h"

#include "FileSystem.hpp"

namespace FS
{

namespace {

// Whether @p path names a volume's root: "2:", "2:/".
bool isVolumeRoot(const TCHAR* path)
{
    const TCHAR* p = strchr(path, ':');
    p = p ? p + 1 : path;
    while (*p == _T('/')) {
        ++p;
    }
    return *p == 0;
}

// Whether the open directory @p dir is the volume's root, whatever its path
// says. FatFs reads a start cluster of 0 as the root (dir_sdi()), so a damaged
// entry that holds 0 (or the root's own cluster) lists the root, and a
// recursive delete would go round through the whole volume.
bool opensAsRoot(const DIR& dir)
{
    const FATFS* fs = dir.obj.fs;
    if (dir.obj.sclust == 0) {
        return true;
    }
    return fs->fs_type >= FS_FAT32 && dir.obj.sclust == static_cast<DWORD>(fs->dirbase);
}

} // namespace

Directory::Directory(const char *path)
{
    assert(path != nullptr);
    setPath(path);
}

Directory::~Directory()
{
    // f_opendir on a non-root path takes one of the FF_FS_LOCK sharing slots
    // (there are only 10, shared with open files), so a Directory destroyed
    // while still open used to leak one. close() is a no-op when the directory
    // was never opened. Qualified because a base destructor cannot dispatch to
    // a derived override anyway.
    Directory::close();
}

void Directory::setPath(const char *path)
{
    assert(path != nullptr);

    if (isOpen()) {
        close();
    }

    strncpy(mPath, path, sizeof(mPath));
}

const char* Directory::getPath() const
{
    return mPath;
}

bool Directory::exist() const
{
    return Exist(mPath);
}

bool Directory::rename(const char *newPath)
{
    if (isOpen()) {
        close();
    }

    if (Rename(mPath, newPath)) {
        setPath(newPath);
        return true;
    }

    return false;
}

bool Directory::remove()
{
    if (mIsOpened) {
        close();
    }

    return Remove(mPath);
}

bool Directory::create()
{
    return Create(mPath);
}

bool Directory::open()
{
    if (FR_OK == f_opendir(&mDir, mPath)) {
        mIsOpened = true;
        return true;
    }

    return false;
}

bool Directory::isOpen() const
{
    return mIsOpened;
}

bool Directory::readNext(Interface::IFileSystem::ObjectInfo &item, bool reset)
{
    if (!mIsOpened) {
        return false;
    }

    if (reset) {
        if (FR_OK == f_readdir(&mDir, NULL)) {
            return true;
        }
        return false;
    }

    FILINFO fno { };

    if (FR_OK == f_readdir(&mDir, &fno)) {
        if (strlen(fno.fname) == 0) {
            return false;
        }
        FileSystem::fillObjectInfo(item, fno);
#if (FF_FS_NORTC == 0)
#if FF_FS_EXFAT
    if (mDir.obj.fs->fs_type == FS_EXFAT) {  // exFAT volume
        // XDIR_ModTZ - exFAT: Modified timezone (BYTE). fillObjectInfo set
        // item.utc from the FAT timestamp; adjust it for the entry's TZ here,
        // where the directory object (and so dirbuf) is available.
        uint8_t tzByte = mDir.obj.fs->dirbuf[23];
        item.utc -= FileSystem::TimezoneOffsetSeconds(tzByte);
    }
#endif // FF_FS_EXFAT
#endif // (FF_FS_NORTC == 0)
        return true;
    }

    return false;
}

bool Directory::close()
{
    if (!mIsOpened) {
        return true;
    }

    // f_closedir returns an FRESULT, where FR_OK is 0. Assigning that straight
    // into a bool inverted both the return value and mIsOpened.
    FRESULT fr = f_closedir(&mDir);

    if (fr != FR_OK) {
        LOG_ERROR("f_closedir failed. Err [%s] Path: %s\n", ERRSTR(fr), mPath);
        return false;
    }

    mIsOpened = false;

    return true;
}

bool Directory::Exist(const char *path)
{
    assert(path != nullptr);
    return FileSystem::GetInstance().exist(path);
}

bool Directory::Rename(const char *oldPath, const char *newPath)
{
    assert(oldPath != nullptr);
    assert(newPath != nullptr);

    return FileSystem::GetInstance().rename(oldPath, newPath);
}

bool Directory::Remove(const char *path)
{
    assert(path != nullptr);
    return FileSystem::GetInstance().remove(path);
}

/**
 * @brief Recursively removes directory contents (and optionally the directory itself).
 *
 * @details
 * This function traverses the directory tree starting at @p path and deletes entries
 * according to the @p filter callback.
 *
 * - If @p filter is not set (empty), all entries are considered deletable.
 * - If @p filter returns @c true for a given node, that node may be deleted.
 * - If @p filter returns @c false for a given node, that node is skipped (not deleted).
 *
 * For subdirectories, the function first processes (traverses) the subdirectory and then,
 * if permitted by @p filter, deletes the now-empty directory.
 *
 * Trailing '/' characters in @p path are ignored (except for root paths like "X:/").
 *
 * @param[in] path
 *     Absolute or relative directory path. Must not be @c nullptr and must be shorter than
 *     @c FF_MAX_LFN.
 * @param[in] filter
 *     Optional callback used to decide whether a node should be deleted.
 *     Signature: <tt>bool(const TCHAR* path, const FILINFO* fno)</tt>.
 *     Return value:
 *     - @c true  -> delete this node (file or directory, if possible)
 *     - @c false -> keep this node (skip deletion)
 *
 * @param[in] deleteThisFolder
 *     Controls whether the directory specified by @p path should be deleted after its contents
 *     are processed:
 *     - @c true  -> delete @p path itself (directory must become empty)
 *     - @c false -> keep @p path, delete only its contents
 *
 * @return @c true on success, @c false otherwise.
 *
 * @note
 * FatFs will return @c FR_DENIED when attempting to delete read-only objects (AM_RDO) or
 * objects currently in use (open handles). In such cases, the operation fails and this
 * function returns @c false.
 */
bool Directory::RemoveRecursively(const char*  path,
                                  RemoveFilter filter,
                                  bool         deleteThisFolder)
{
    if (!path) {
        return false;
    }

    const size_t len = strlen(path);
    if (len == 0 || len >= FF_MAX_LFN) {
        return false;
    }

    auto normalize = [](char* s) {
        size_t n = strlen(s);
        while (n > 0 && s[n - 1] == '/' && !(n == 3 && s[1] == ':' && s[2] == '/')) {
            s[n - 1] = '\0';
            --n;
        }
    };

    char buff[FF_MAX_LFN];
    strcpy(buff, path);
    normalize(buff);

    FILINFO fno = {};
    uint32_t skipped = 0;

    const FRESULT fr = deleteNode(buff, FF_MAX_LFN, &fno, filter, skipped);

    if (fr != FR_OK) {
        LOG_ERROR("Err [%s] Path: %s\n", ERRSTR(fr), path);
        return false;
    }

    if (skipped != 0) {
        LOG_ERROR("%lu entries not deleted. Path: %s\n",
                  static_cast<unsigned long>(skipped), path);
        return false;
    }

    if (!deleteThisFolder) {
        return true;
    }

    strcpy(buff, path);

    normalize(buff);

    return (f_unlink(buff) == FR_OK);
}

bool Directory::Create(const char *path)
{
    assert(path != nullptr);
    return FileSystem::GetInstance().mkdir(path);
}

/**
 * @brief Internal recursive routine to traverse and delete a directory tree.
 *
 * @details
 * This function is used by Directory::RemoveRecursively() as the traversal engine.
 * It opens the directory specified by @p path, iterates over its entries, and for each entry:
 * - builds a full child path in the @p path buffer (in-place)
 * - calls @p filter (if provided) to decide whether the entry should be deleted
 * - recurses into subdirectories
 * - deletes files and (optionally) subdirectories after recursion
 *
 * @param[in,out] path
 *     Mutable path buffer containing the base directory path on entry.
 *     The buffer is modified in-place to append child names while traversing.
 *     On return, the buffer is restored back to the base directory path.
 * @param[in] sz_buff
 *     Size of @p path buffer in TCHAR items (not bytes). Must be large enough to hold
 *     the longest constructed path including a terminating null character.
 * @param[in,out] fno
 *     Pointer to a FILINFO structure used as a scratch buffer for directory entry information.
 * @param[in] filter
 *     Optional callback used to decide whether an entry should be deleted.
 *     Signature: <tt>bool(const TCHAR* path, const FILINFO* fno)</tt>.
 * @param[in,out] skipped
 *     Incremented when @p path, or a subdirectory under it, is left alone because
 *     its entry opens as the volume's root (a damaged entry). The folders that
 *     hold one are kept too.
 *
 * @return FatFs result code:
 *     - @c FR_OK on success, including when entries were skipped (see @p skipped)
 *     - otherwise an error code returned by FatFs (e.g., @c FR_DENIED, @c FR_INVALID_PARAMETER, ...)
 *
 * @warning
 * The @p path buffer must remain valid for the entire recursion depth. Do not pass a pointer
 * to a temporary or a string literal.
 *
 * @note
 * Special directory entries "." and ".." are skipped.
 */
FRESULT Directory::deleteNode(TCHAR*              path,
                              UINT                sz_buff,
                              FILINFO*            fno,
                              const RemoveFilter& filter,
                              uint32_t&           skipped)
{
    FRESULT fr;
    DIR dir;

    LOG_DEBUG("path = %s\n", path);

    fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        LOG_ERROR("f_opendir failed. Err [%s] Path: %s\n", ERRSTR(fr), path);
        return fr;
    }

    if (opensAsRoot(dir) && !isVolumeRoot(path)) {
        // Going in would delete from the root, and unlinking it would check the
        // root for emptiness. Leave it; the caller keeps the folders holding it.
        LOG_ERROR("Skipped: the entry points at the root. Path: %s\n", path);
        f_closedir(&dir);
        ++skipped;
        return FR_OK;
    }

    const size_t baseLen = strlen(path);
    if (baseLen == 0) {
        LOG_ERROR("Invalid parameter: empty path\n");
        f_closedir(&dir);
        return FR_INVALID_PARAMETER;
    }

    // Ensure trailing slash and remember base length WITH slash
    UINT i = (UINT)baseLen;
    size_t baseSlashLen = baseLen;

    if (path[i - 1] != _T('/')) {
        if (i + 1 >= sz_buff) {
            LOG_ERROR("Path buffer overflow while appending '/'. Base: %s\n", path);
            f_closedir(&dir);
            return FR_INVALID_PARAMETER;
        }

        path[i++] = _T('/');
        path[i] = 0;
        baseSlashLen = baseLen + 1;     // <-- key
    }

    for (;;) {
        fr = f_readdir(&dir, fno);
        if (fr != FR_OK) {
            LOG_ERROR("f_readdir failed. Err [%s] Base: %s\n", ERRSTR(fr), path);
            break;
        }

        if (!fno->fname[0]) {
            break; // end of directory
        }

        if ((strcmp(fno->fname, ".") == 0) || (strcmp(fno->fname, "..") == 0)) {
            continue;
        }

        // Build <base_with_slash><child>
        UINT j = 0;
        do {
            if (i + j >= sz_buff) {
                fr = FR_INVALID_PARAMETER;
                LOG_ERROR("Path buffer overflow while appending entry '%s' to base '%s'\n",
                          fno->fname, path);
                break;
            }
            path[i + j] = fno->fname[j];
        } while (fno->fname[j++]);

        if (fr != FR_OK) {
            break;
        }

        const bool wantDelete = filter ? filter(path, fno) : true;

        if (fno->fattrib & AM_DIR) {
            const uint32_t skippedBefore = skipped;
            fr = deleteNode(path, sz_buff, fno, filter, skipped);
            if (fr != FR_OK) {
                LOG_ERROR("Failed while processing subdir. Err [%s] Path: %s\n", ERRSTR(fr), path);
                break;
            }

            // A folder that kept a skipped entry is not empty.
            if (wantDelete && skipped == skippedBefore) {
                fr = f_unlink(path);
                if (fr != FR_OK) {
                    LOG_ERROR("unlink folder failed. Err [%s] Path: %s attr=0x%02X\n", ERRSTR(fr), path, fno->fattrib);

                    FILINFO fi{};
                    FRESULT fr2 = f_stat(path, &fi);
                    LOG_ERROR("stat after unlink: %d attr=0x%02X\n", (int)fr2, fi.fattrib);
                    break;
                }
            }
        } else {
            if (wantDelete) {
                fr = f_unlink(path);
                if (fr != FR_OK) {
                    LOG_ERROR("unlink file failed. Err [%s] Path: %s attr=0x%02X\n", ERRSTR(fr), path, fno->fattrib);

                    FILINFO fi{};
                    FRESULT fr2 = f_stat(path, &fi);
                    LOG_ERROR("stat after unlink: %d attr=0x%02X\n", (int)fr2, fi.fattrib);
                    break;
                }
            }
        }

        // Restore to base path WITH slash, ready for next append
        path[baseSlashLen] = 0;
        i = (UINT)baseSlashLen;
    }

    // Restore to original base path (without added slash, if we added it)
    path[baseLen] = 0;

    const FRESULT frClose = f_closedir(&dir);
    if (frClose != FR_OK) {
        LOG_ERROR("f_closedir failed. Err [%s] Base: %s\n", ERRSTR(frClose), path);
    }

    return (fr != FR_OK) ? fr : frClose;
}

} /* namespace FS */
