/*
    Copyright (C) 2015-2025, Navaro, All Rights Reserved
    SPDX-License-Identifier: MIT

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
 */

#ifndef __QORAAL_FLASH_H__
#define __QORAAL_FLASH_H__

#include <stdint.h>
#include <string.h>
#include "qoraal/qoraal.h"
#include "qoraal-flash/registry.h"
#include "qoraal-flash/syslog.h"

/**
 * @brief Structure to define the flash memory interface.
 * 
 * The structure holds function pointers to operations
 * for reading, writing, and erasing flash memory.
 */
typedef struct {
    /**
     * @brief Read data from flash memory.
     * 
     * @param addr The starting address in flash memory.
     * @param len The length of the data to read in bytes.
     * @param data Pointer to a buffer where the read data will be stored.
     * @return 0 on success, or a negative error code on failure.
     */
	int32_t (*flash_read)(uint32_t addr, uint32_t len, uint8_t *data);

    /**
     * @brief Write data to flash memory.
     * 
     * @param addr The starting address in flash memory.
     * @param len The length of the data to write in bytes.
     * @param data Pointer to the data to be written.
     * @return 0 on success, or a negative error code on failure.
     */
	int32_t (*flash_write)(uint32_t addr, uint32_t len, const uint8_t *data);

    /**
     * @brief Erase a region of flash memory.
     * 
     * @param addr_start The starting address of the region to erase.
     * @param addr_end The ending address of the region to erase.
     * @return 0 on success, or a negative error code on failure.
     */
	int32_t (*flash_erase)(uint32_t addr_start, uint32_t addr_end);

} QORAAL_FLASH_CFG_T;

/*===========================================================================*/
/* External declarations.                                                    */
/*===========================================================================*/

extern const QORAAL_FLASH_CFG_T *_qoraal_flash_instance;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief   Hand over the volumes this library persists to and bring it up.
 *
 * The application owns the storage: it declares each volume against its own
 * partition and hands it over here. Pass 0 for one it does not use.
 *
 * This is the registry and syslog form, kept for callers that predate the
 * tallies volume; it is qoraal_flash_init_default_ex() with no tallies.
 */
int32_t     qoraal_flash_init_default (const QORAAL_FLASH_CFG_T * instance, NVOL3_INSTANCE_T * registry, SYSLOG_INSTANCE_T * syslog);

/**
 * @brief   As qoraal_flash_init_default(), plus the tallies volume.
 *
 * @p tallies may be 0 whether or not CONFIG_QORAAL_FLASH_TALLIES is set; with
 * the feature off the whole lifecycle compiles to nothing, and with it on a
 * volume that was never handed over simply leaves the counters in RAM.
 *
 * There is no matching start_ex or stop_ex: those take no arguments, and
 * they carry the tallies lifecycle whether or not a volume was handed over -
 * both calls are a no-op when it was not.
 */
int32_t     qoraal_flash_init_default_ex (const QORAAL_FLASH_CFG_T * instance, NVOL3_INSTANCE_T * registry, SYSLOG_INSTANCE_T * syslog, NVOL3_INSTANCE_T * tallies);

int32_t     qoraal_flash_start_default (void);

/**
 * @brief   Flush and unload every volume. Mirrors the start, reversed.
 *
 * Call it before a reset, after the services have been halted, so that what
 * they counted and logged on the way down reaches FLASH. A reset that does
 * not come through here - a watchdog, a brownout - loses whatever the
 * periodic passes had not yet written.
 */
int32_t     qoraal_flash_stop_default (void);

int32_t     qoraal_flash_instance_init (const QORAAL_FLASH_CFG_T * instance);

static inline int qoraal_flash_read(uint32_t addr, uint32_t len, uint8_t *data) {
    if (_qoraal_flash_instance && _qoraal_flash_instance->flash_read) {
        return _qoraal_flash_instance->flash_read(addr, len, data);
    }
    return -1; // Error if no implementation exists
}


static inline int qoraal_flash_write(uint32_t addr, uint32_t len, const uint8_t *data) {
    if (_qoraal_flash_instance && _qoraal_flash_instance->flash_write) {
        return _qoraal_flash_instance->flash_write(addr, len, data);
    }
    return -1; // Error if no implementation exists
}


static inline int qoraal_flash_erase(uint32_t addr_start, uint32_t addr_end) {
    if (_qoraal_flash_instance && _qoraal_flash_instance->flash_erase) {
        return _qoraal_flash_instance->flash_erase(addr_start, addr_end);
    }
    return -1; // Error if no implementation exists
}

#ifdef __cplusplus
}
#endif

#endif /* __QORAAL_FLASH_H__ */
