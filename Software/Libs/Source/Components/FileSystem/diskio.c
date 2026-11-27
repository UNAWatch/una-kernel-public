/**
 ******************************************************************************
 * @file    diskio.c
 * @brief   Low level disc I/O implementation
 ******************************************************************************
 *
 ******************************************************************************
 */

/*-----------------------------------------------------------------------*/
/* Low level disk I/O module skeleton for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"			/* Obtains integer types */
#include "diskio.h"		/* Declarations of disk functions */

#include "flash_diskio.h"

/* Definitions of physical drive number for each drive */
static const uint8_t skDiskMap[FF_VOLUMES] =
{
     FLASH_DISK_UPD,
#if (FF_VOLUMES >= 2)
     FLASH_DISK_SYS,
#if (FF_VOLUMES >= 3)
     FLASH_DISK_USR,
#endif // FF_VOLUMES >= 3
#endif // FF_VOLUMES >= 2
};

/* Definitions of drive labels for each drive */
const char* skDiskLabel[FF_VOLUMES] =
{
     "UNA BOOT",
#if (FF_VOLUMES >= 2)
     "UNA SYS",
#if (FF_VOLUMES >= 3)
     "UNA WATCH",
#endif // FF_VOLUMES >= 3
#endif // FF_VOLUMES >= 2
};

#if (FF_MULTI_PARTITION == 1)
PARTITION VolToPart[] =
{
 {0, 1}, // Logical disk 0 -> Physical disk 0, partition 1
#if (FF_VOLUMES >= 2)
 {1, 1}, // Logical disk 1 -> Physical disk 1, partition 1
#if (FF_VOLUMES >= 3)
 {2, 1}, // Logical disk 1 -> Physical disk 2, partition 1
#endif // FF_VOLUMES >= 3
#endif // FF_VOLUMES >= 2
};
#endif

/*
 * A volume handed to the USB host is a removed medium as far as FatFs is
 * concerned. sReleased: the host has it, so FatFs may not initialise it.
 * sStale: FatFs's picture of the volume (cached sector, allocation hints) may
 * no longer match it. Both are set by disk_release(); disk_reclaim() clears
 * sReleased, and sStale is cleared by the next disk_initialize(), which FatFs
 * calls when it remounts the volume on its first access after the reclaim.
 * Both are read by FatFs with the volume lock held.
 */
static volatile uint8_t sReleased[FF_VOLUMES];
static volatile uint8_t sStale[FF_VOLUMES];

void disk_release (
	BYTE pdrv		/* Physical drive number to identify the drive */
)
{
    if (pdrv < FF_VOLUMES) {
        sStale[pdrv]    = 1;
        sReleased[pdrv] = 1;
    }
}

void disk_reclaim (
	BYTE pdrv		/* Physical drive number to identify the drive */
)
{
    if (pdrv < FF_VOLUMES) {
        sReleased[pdrv] = 0;
    }
}

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive number to identify the drive */
)
{
    if (pdrv < FF_VOLUMES) {
        if (sStale[pdrv]) {
            return STA_NOINIT;
        }
        return flash_diskio_status(skDiskMap[pdrv]) ? RES_OK : RES_PARERR;
    } else {
        return RES_PARERR;
    }
}



/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive number to identify the drive */
)
{
    if (pdrv < FF_VOLUMES) {
        if (sReleased[pdrv]) {
            return STA_NOINIT;
        }
        if (!flash_diskio_initialize(skDiskMap[pdrv])) {
            return RES_PARERR;
        }
        sStale[pdrv] = 0;
        return RES_OK;
    } else {
        return RES_PARERR;
    }
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive number to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
    if (pdrv < FF_VOLUMES) {
        return flash_diskio_read(skDiskMap[pdrv], buff, sector, count)
                ? RES_OK : RES_PARERR;
    } else {
        return RES_PARERR;
    }
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive number to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
    if (pdrv < FF_VOLUMES) {
        return flash_diskio_write(skDiskMap[pdrv], buff, sector, count)
                ? RES_OK : RES_PARERR;
    } else {
        return RES_PARERR;
    }
}

#endif


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive number (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
    if (pdrv < FF_VOLUMES) {

        switch (cmd) {
            case CTRL_SYNC:
                break;

            // Get erase block size (needed at _USE_MKFS == 1)
            // Retrieves erase block size in unit of sector of the flash
            case GET_BLOCK_SIZE: {
                uint32_t blsz = 0;
                if (flash_diskio_block_size(skDiskMap[pdrv], &blsz)) {
                    *(DWORD*) buff = blsz;
                } else {
                    return RES_PARERR;
                }
            } break;

            // Get sector size (needed at _MAX_SS != _MIN_SS)
            case GET_SECTOR_SIZE: {
                uint32_t secsz = 0;
                if (flash_diskio_sector_size(skDiskMap[pdrv], &secsz)) {
                    *(DWORD*) buff = secsz;
                } else {
                    return RES_PARERR;
                }
            } break;

            // Get media size (needed at _USE_MKFS == 1)
            case GET_SECTOR_COUNT: {
                uint32_t seccnt = 0;
                if (flash_diskio_sector_count(skDiskMap[pdrv], &seccnt)) {
                    *(DWORD*) buff = seccnt;
                } else {
                    return RES_PARERR;
                }
            } break;

            default:
                return RES_PARERR;
                break;
        }

    } else {
        return RES_PARERR;
    }

    return RES_OK;
}

