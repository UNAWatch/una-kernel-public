/**
 ******************************************************************************
 * @file    Directory.hpp
 * @brief   Directory object.
 ******************************************************************************
 *
 ******************************************************************************
 */

#ifndef __DIRECTORY_HPP
#define __DIRECTORY_HPP

#include <stdint.h>
#include <stdbool.h>

#include <functional>

#include "IFileSystem.hpp"
#include "ff.h"

namespace FS
{

/**
 * @brief   Directory object.
 */
class Directory : public Interface::IDirectory
{
public:
    using RemoveFilter = std::function<bool(const TCHAR* path, const FILINFO* fno)>;

    /**
     * @brief   Creates a new Directory instance.
     */
    Directory() = default;

    /**
     * @brief   Creates a new Directory instance.
     * @param   path: Absolute path to directory (0:/dir1/dir2).
     */
    Directory(const char *path);

    /**
     * @brief   Destructor. Releases the FatFs handle if the directory is
     *          still open.
     */
    virtual ~Directory();

    /// @name Non-copyable / non-movable
    /// An open DIR holds an index (obj.lockid) into FatFs' table of open
    /// objects. Copying the object duplicates it, and since the destructor
    /// closes, two destructors would decrement one table entry twice — an
    /// entry that may by then have been reused for a different open object.
    /// Matches FS::File.
    /// @{
    Directory(const Directory&)            = delete;
    Directory& operator=(const Directory&) = delete;
    Directory(Directory&&)                 = delete;
    Directory& operator=(Directory&&)      = delete;
    /// @}

    /**
     * @brief   Redirect object to new path.
     * @note    This method will close dir if it was open before.
     * @param   path: New path to directory (0:/dir1/dir2).
     */
    virtual void setPath(const char *path) override;

    /**
     * @brief   Full path to the directory.
     * @retval  path: Current directory path.
     */
    virtual const char* getPath() const override;

    /**
     * @brief   Checks if the directory exists.
     * @retval  'true' if the directory exists, 'false' otherwise.
     */
    virtual bool exist() const override;

    /**
     * @brief   Rename directory.
     * @param   newPath: New directory path.
     * @retval  'true' if the directory was successfully renamed,
     *          'false' otherwise.
     */
    virtual bool rename(const char *newPath) override;

    /**
     * @brief   Remove directory.
     * @retval  'true' if the object was successfully removed,
     *          'false' otherwise.
     */
    virtual bool remove() override;

    /**
     * @brief   Creates the directory if it does not exist.
     * @retval  'true' if the directory was successfully created,
     *          'false' otherwise.
     */
    virtual bool create() override;

    /**
     * @brief   Opens the directory for reading.
     * @retval  'true' if the directory was successfully opened,
     *          'false' otherwise.
     */
    virtual bool open() override;

    /**
     * @brief   Checks if the directory is open.
     * @retval  'true' if the directory is open, 'false' otherwise.
     */
    virtual bool isOpen() const override;

    /**
     * @brief   Reads the next item in the directory.
     * @param   item: Reference to store the directory item information.
     * @param   reset: Reset the read position if true.
     * @retval  'true' if the item was successfully read, 'false' otherwise.
     */
    virtual bool readNext(Interface::IFileSystem::ObjectInfo &item,
            bool reset = false) override;

    /**
     * @brief   Closes the directory.
     * @retval  'true' if the directory was successfully closed,
     *          'false' otherwise.
     */
    virtual bool close() override;

    /**
     * @brief   Checks if the directory exists.
     * @param   path: Path to the directory.
     * @retval  'true' if the directory exists, 'false' otherwise.
     */
    static bool Exist(const char *path);

    /**
     * @brief   Rename directory.
     * @param   oldPath: Current path of the directory.
     * @param   newPath: New directory path.
     * @retval  'true' if the directory was successfully renamed,
     *          'false' otherwise.
     */
    static bool Rename(const char *oldPath, const char *newPath);

    /**
     * @brief   Remove an empty directory.
     * @param   path: Path to the directory.
     * @retval  'true' if the directory was successfully removed,
     *          'false' otherwise.
     */
    static bool Remove(const char *path);

    /**
     * @brief   Remove directory with whole its content.
     * @details A subdirectory whose entry opens as the volume's root (a
     *          damaged entry with start cluster 0) is left alone, with the
     *          folders that hold it, and the rest is removed; the call then
     *          returns 'false'. A @p path that opens as the root without
     *          naming it is left alone the same way.
     * @param   path: Path to the directory.
     * @retval  'true' if the directory was successfully removed,
     *          'false' otherwise.
     */
    static bool RemoveRecursively(const char*  path,
                                  RemoveFilter filter,
                                  bool         deleteThisFolder);

    /**
     * @brief   Creates the directory if it does not exist.
     * @param   path: Path to the directory.
     * @retval  'true' if the directory was successfully created,
     *          'false' otherwise.
     */
    static bool Create(const char *path);

protected:

    // Path to the directory
    char mPath[FF_MAX_LFN + 1] { };

    // Flag indicates that file is opened
    bool mIsOpened { };

private:
    static FRESULT deleteNode(TCHAR*              path,
                              UINT                sz_buff,
                              FILINFO*            fno,
                              const RemoveFilter& filter,
                              uint32_t&           skipped);

    // Directory object
    DIR mDir { };

};

} /* namespace FS */

#endif /* __DIRECTORY_HPP */
