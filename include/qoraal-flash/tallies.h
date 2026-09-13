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

/**
 * @file    qoraal-flash/tallies.h
 * @brief   Persistent counters and elapsed-time timers.
 *
 * A tallie is a uint32 counter with the date and time of its last update,
 * persisted to an nvol3 volume owned by the application.
 *
 * Counters live in per-module blocks. A module owns a static descriptor - the
 * definitions, the RAM values and the dirty state - and registers it at
 * service CTRL_INIT, the same shape as QORAAL_PROP_RESOURCE_T registers with
 * CMM. This library walks the registration list to load, persist, reset and
 * report. There is no project-wide list of counters.
 *
 * The values are static (.bss), deliberately:
 *
 *  - the hot path stays a pointer plus an array index, cheap enough to put on
 *    an error path
 *  - the RAM cost is visible in the map file, and if it links, it fits
 *  - counters work before the volume is loaded and if the volume is broken.
 *    "We ran out of memory" is exactly the event you want a tallie for, and a
 *    heap-backed counter cannot record it.
 *
 * Ids are namespaced: the global id - which is the nvol3 key - is
 * @p base + local, where base is @p module_id * @ref TALLIES_PER_MODULE. The
 * library is policy free and takes an absolute base; a project layer owns the
 * module numbering (see LXT-LIB-CMM/cmm_tallies.h for the LXT policy).
 *
 * Threading: nvol3 takes no locks. This layer owns a mutex over every mutation
 * and over the whole persist pass. Registration and the lifecycle calls are
 * expected to run from one thread.
 */

#ifndef __QORAAL_FLASH_TALLIES_H__
#define __QORAAL_FLASH_TALLIES_H__

#include <stdint.h>
#include "qoraal/common/rtclib.h"
#include "qoraal-flash/nvram/nvol3.h"

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*/
/* Module constants.                                                         */
/*===========================================================================*/

/** @brief Storage allocated for a tallie name, including the terminator. */
#define TALLIES_MAX_NAME_LEN            12

/** @brief Id space reserved per module. global id = module_id * this + local. */
#define TALLIES_PER_MODULE              32

/**
 * @brief   Sentinel for an unregistered block.
 * @note    Ids are uint16_t, so a project's module numbering has to keep
 *          module_id * TALLIES_PER_MODULE below this. tallies_register()
 *          enforces it.
 */
#define TALLIES_BASE_NONE               ((uint16_t)0xFFFF)

#ifdef NDEBUG
#define TALLIES_PERSIST_INTERVAL        (60*60*5)
#else
#define TALLIES_PERSIST_INTERVAL        (60*2)
#endif

/** @brief Records written per persist pass before the task yields. */
#define TALLIES_PERSIST_MAX_PER_PASS    16

/** @brief Seconds to wait before resuming a persist pass that hit the bound. */
#define TALLIES_PERSIST_RESUME          1

/*
 * Rate limits for TALLIE_DEF_RATE(). A rate limited tallie ignores inc/add
 * until the interval since its last update has passed.
 */
#define SECONDS_ZERO                    (0)
#define SECONDS_FIVE                    (5)
#define SECONDS_TEN                     (10)
#define SECONDS_FIVE_MINUTES            (60*5)
#define SECONDS_TEN_MINUTES             (60*10)
#define SECONDS_SIX_HOURS               (60*60*6)
#define SECONDS_TWELVE_HOURS            (60*60*12)
#define SECONDS_ONE_DAY                 (60*60*24)

/*
 * Per entry state. The state is four valued, so a plain dirty bit will not do:
 * a running timer has no value to write until it is sampled.
 */
#define TALLIES_CLEAN                   0
#define TALLIES_DIRTY_VALUE             1
#define TALLIES_DIRTY_TIMER_RUNNING     2
#define TALLIES_DIRTY_TIMER_PAUSED      3

/*===========================================================================*/
/* Module data structures and types.                                         */
/*===========================================================================*/

/**
 * @brief   A counter value and when it last changed.
 */
typedef struct TALLIES_ENTRY_S {
    RTCLIB_DATE_T       date ;
    RTCLIB_TIME_T       time ;
    uint32_t            value ;

} TALLIES_ENTRY_T ;

/**
 * @brief   Compile time definition of one tallie.
 * @note    @p name is local to the module ("imgfail"); display composes
 *          "module.name". It is also written to FLASH and checked on load,
 *          which is what turns a renumbering mistake into a visible reset
 *          instead of a counter inheriting a stranger's history.
 */
typedef struct TALLIES_DEF_S {
    const char *        name ;
    uint32_t            seconds ;       /**< @brief rate limit, 0 = none */

} TALLIES_DEF_T ;

/**
 * @brief   A module's block of tallies.
 * @note    Generated by qoraal-flash/tallies_module.h; all of it is static.
 */
typedef struct TALLIES_BLOCK_S {
    struct TALLIES_BLOCK_S *    next ;  /**< @brief intrusive registration list */
    const char *                name ;  /**< @brief module name, e.g. "cardreader" */
    const TALLIES_DEF_T *       defs ;
    TALLIES_ENTRY_T *           entry ;
    uint8_t *                   dirty ;
    uint16_t                    count ;
    uint16_t                    base ;  /**< @brief absolute id base, TALLIES_BASE_NONE when unregistered */

} TALLIES_BLOCK_T ;

#define TALLIES_BLOCK_INIT(name_, defs_, entry_, dirty_, count_)               \
        { 0, name_, defs_, entry_, dirty_, (uint16_t)(count_), TALLIES_BASE_NONE }

/** @brief Size of the stored payload: the name followed by the entry. */
#define TALLIES_RECORD_DATA_SIZE        (TALLIES_MAX_NAME_LEN + \
                                         sizeof(TALLIES_ENTRY_T))

/**
 * @brief   Declare the application owned volume.
 * @note    local_size is 0 on purpose: the values live in the module blocks,
 *          so the dictionary only has to map a key to a slot index.
 */
#define TALLIES_INST_DECL(name, start_addr, sector_size, hashsize)             \
        NVOL3_UINT_INSTANCE_DECL(name,                                         \
            start_addr,                                                        \
            start_addr + sector_size,                                          \
            sector_size,                                                       \
            TALLIES_RECORD_DATA_SIZE,                                          \
            0 /*local_size*/,                                                  \
            hashsize,                                                          \
            0 /*tallie*/,                                                      \
            NVOL3_SECTOR_VERSION)

/*===========================================================================*/
/* External declarations.                                                    */
/*===========================================================================*/

#if defined CONFIG_QORAAL_FLASH_TALLIES && CONFIG_QORAAL_FLASH_TALLIES

    /*
     * Lifecycle. The application owns the volume, the same contract as
     * registry_init(): declare it with TALLIES_INST_DECL against your own
     * partition and hand it over here.
     */
    int32_t     tallies_init (NVOL3_INSTANCE_T * inst) ;
    int32_t     tallies_start (void) ;
    void        tallies_stop (void) ;
    void        tallies_reset (void) ;

    /*
     * Registration. Normally called through a project wrapper that owns the
     * module numbering. Works before or after tallies_start(): a block
     * registered first is populated by tallies_start(), a block registered
     * later is populated immediately.
     *
     * It is also safe before tallies_init() - a module registers at service
     * CTRL_INIT, which can run before the application has handed the volume
     * over - because registering only appends to a list. Counting into a
     * block does need tallies_init(), and returns E_PARM until then.
     */
    int32_t     tallies_register (TALLIES_BLOCK_T * blk, int32_t base) ;
    int32_t     tallies_unregister (TALLIES_BLOCK_T * blk) ;

    /*
     * Counters. inc/add honour the rate limit from TALLIE_DEF_RATE and return
     * E_BUSY when they are inside the window; set and clear do not, so a
     * deliberate write is never dropped.
     */
    int32_t     tallies_inc (TALLIES_BLOCK_T * blk, uint16_t local) ;
    int32_t     tallies_add (TALLIES_BLOCK_T * blk, uint16_t local, uint32_t value) ;
    int32_t     tallies_set (TALLIES_BLOCK_T * blk, uint16_t local, uint32_t value) ;
    int32_t     tallies_clear (TALLIES_BLOCK_T * blk, uint16_t local) ;

    /*
     * Timers. The tallie accumulates seconds between start and stop.
     */
    int32_t     tallies_start_timer (TALLIES_BLOCK_T * blk, uint16_t local) ;
    int32_t     tallies_stop_timer (TALLIES_BLOCK_T * blk, uint16_t local) ;

    /*
     * Suspend elapsed time accounting across a clock step. Call pause before
     * the clock is set and resume after, or running timers will absorb the
     * step as elapsed seconds.
     */
    void        tallies_timers_pause (void) ;
    void        tallies_timers_resume (void) ;

    /* Read back. A running timer reports its value plus the seconds so far. */
    uint32_t    tallies_get_value (TALLIES_BLOCK_T * blk, uint16_t local) ;
    int32_t     tallies_get (TALLIES_BLOCK_T * blk, uint16_t local, TALLIES_ENTRY_T * out) ;
    const char * tallies_name (TALLIES_BLOCK_T * blk, uint16_t local) ;

    /* Write every dirty entry out. all != 0 also samples running timers. */
    void        tallies_persist (uint32_t all) ;

    /* Iteration for the shell and, later, the publishers. */
    TALLIES_BLOCK_T * tallies_block_first (void) ;
    TALLIES_BLOCK_T * tallies_block_next (TALLIES_BLOCK_T * cur) ;

    /*
     * Counts records dropped on load because the stored name did not match the
     * registered one, and timer intervals discarded as implausible.
     */
    uint32_t    tallies_mismatched (void) ;
    uint32_t    tallies_timer_drops (void) ;

    void        tallies_log_status (void) ;

/**
 * @brief   Generic accessors. A module normally wraps these, e.g.
 *          @code #define CARDREADER_TALLIE_INC(x) TALLIES_INC(cardreader, x) @endcode
 */
#define TALLIES_INC(mod, x)             tallies_inc (&_##mod##_tallies, __##mod##_tallie_##x)
#define TALLIES_ADD(mod, x, value)      tallies_add (&_##mod##_tallies, __##mod##_tallie_##x, value)
#define TALLIES_SET(mod, x, value)      tallies_set (&_##mod##_tallies, __##mod##_tallie_##x, value)
#define TALLIES_CLEAR(mod, x)           tallies_clear (&_##mod##_tallies, __##mod##_tallie_##x)
#define TALLIES_START_TIMER(mod, x)     tallies_start_timer (&_##mod##_tallies, __##mod##_tallie_##x)
#define TALLIES_STOP_TIMER(mod, x)      tallies_stop_timer (&_##mod##_tallies, __##mod##_tallie_##x)
#define TALLIES_GET(mod, x)             tallies_get_value (&_##mod##_tallies, __##mod##_tallie_##x)

#else /* CONFIG_QORAAL_FLASH_TALLIES */

/*
 * Compiled out. tallies_module.h generates nothing and the accessors become
 * no-ops, so module source needs no #ifdef of its own.
 */
#define TALLIES_INC(mod, x)             ((void)0)
#define TALLIES_ADD(mod, x, value)      ((void)0)
#define TALLIES_SET(mod, x, value)      ((void)0)
#define TALLIES_CLEAR(mod, x)           ((void)0)
#define TALLIES_START_TIMER(mod, x)     ((void)0)
#define TALLIES_STOP_TIMER(mod, x)      ((void)0)
#define TALLIES_GET(mod, x)             ((uint32_t)0)

#endif /* CONFIG_QORAAL_FLASH_TALLIES */

#ifdef __cplusplus
}
#endif

#endif /* __QORAAL_FLASH_TALLIES_H__ */
