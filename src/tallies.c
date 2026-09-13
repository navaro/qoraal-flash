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

#include "qoraal-flash/config.h"

#if defined CONFIG_QORAAL_FLASH_TALLIES && CONFIG_QORAAL_FLASH_TALLIES

#include <stdint.h>
#include <string.h>
#include "qoraal/qoraal.h"
#include "qoraal/svc/svc_tasks.h"
#include "qoraal/common/rtclib.h"
#include "qoraal-flash/tallies.h"

#define DBG_MESSAGE_TALLIES(severity, fmt_str, ...) \
        DBG_MESSAGE_T_LOG (SVC_LOGGER_TYPE(severity,0), 0, fmt_str, ##__VA_ARGS__)

extern void keep_talliescmds (void) ;

/*===========================================================================*/
/* Stored record.                                                            */
/*===========================================================================*/

/*
 * The name is stored so that it can be checked on load. See block_load():
 * if it does not match, the record is dropped and the counter starts at zero
 * rather than inheriting the history of whatever used to own that id.
 */
#pragma pack(1)
typedef struct TALLIES_DATA_S {
    char                    name[TALLIES_MAX_NAME_LEN] ;
    TALLIES_ENTRY_T         entry ;

} TALLIES_DATA_T ;

typedef struct TALLIES_RECORD_S {
    NVOL3_RECORD_HEAD_T     head ;
    uint32_t                key ;
    TALLIES_DATA_T          data ;

} TALLIES_RECORD_T ;
#pragma pack()

#define TALLIES_RECORD_LEN      (sizeof(uint32_t) + sizeof(TALLIES_DATA_T))

/*===========================================================================*/
/* Module local variables.                                                   */
/*===========================================================================*/

static NVOL3_INSTANCE_T *   _tallies_inst = 0 ;
static TALLIES_BLOCK_T *    _tallies_blocks = 0 ;
static uint32_t             _tallies_started = 0 ;
static uint32_t             _tallies_paused = 0 ;
static uint32_t             _tallies_mismatched = 0 ;
static uint32_t             _tallies_timer_drops = 0 ;
static p_mutex_t            _tallies_mutex = 0 ;

/*
 * One scratch record for the whole store, allocated at start and reused.
 * nvol3 reads up to record_size into it, which is larger than the payload we
 * write, so a corrupt length cannot overrun a caller's buffer. It also keeps
 * the persist pass off the allocator - see the design note on record_set()
 * doing an NVOL3_MALLOC per call.
 */
static NVOL3_RECORD_T *     _tallies_scratch = 0 ;

static SVC_TASKS_DECL(_tallies_task) ;

static int32_t  block_persist_locked (TALLIES_BLOCK_T * blk, uint16_t local) ;
static int32_t  stop_timer_locked (TALLIES_BLOCK_T * blk, uint16_t local) ;

/*===========================================================================*/
/* Local functions.                                                          */
/*===========================================================================*/

static inline int32_t
tallies_ready (TALLIES_BLOCK_T * blk, uint16_t local)
{
    /*
     * Deliberately does not require tallies_start(): a registered block counts
     * into RAM whether or not the volume is up, and gets written out when and
     * if it comes up.
     */
    if (!_tallies_inst || !blk || (local >= blk->count) || (blk->base < 0)) {
        return E_PARM ;
    }

    return EOK ;
}

/**
 * @brief   EOK if this entry may be counted, E_BUSY if it is still inside its
 *          rate limit window.
 * @note    Only inc and add consult it. Rate limiting a set, a clear or a
 *          timer stop would silently drop a deliberate write.
 * @note    Called with the mutex held: it reads the timestamp that every
 *          other mutation writes.
 */
static int32_t
rate_limited_locked (TALLIES_BLOCK_T * blk, uint16_t local)
{
    if (blk->defs[local].seconds) {
        RTCLIB_DATE_T date ;
        RTCLIB_TIME_T time ;

        rtc_localtime (rtc_time(), &date, &time) ;

        if (blk->entry[local].date.date != date.date) {
            return EOK ;
        }

        if (rtc_seconds_diff (blk->entry[local].time, time) <
                    blk->defs[local].seconds) {
            return E_BUSY ;
        }
    }

    return EOK ;
}

/**
 * @brief   The name exactly as it is stored, so load and persist compare the
 *          same bytes.
 * @note    The old implementation strncpy'd into an uninitialised stack
 *          record, so an 11 character name left byte 11 as whatever was on the
 *          stack and identical logical records produced different FLASH bytes.
 */
static void
name_canonical (const char * name, char * out)
{
    size_t len = name ? strlen (name) : 0 ;

    if (len > TALLIES_MAX_NAME_LEN - 1) {
        len = TALLIES_MAX_NAME_LEN - 1 ;
    }

    memset (out, 0, TALLIES_MAX_NAME_LEN) ;
    if (len) {
        memcpy (out, name, len) ;
    }
}

static TALLIES_BLOCK_T *
block_find (TALLIES_BLOCK_T * blk)
{
    TALLIES_BLOCK_T * entry ;

    for (entry = _tallies_blocks; entry; entry = entry->next) {
        if (entry == blk) {
            return entry ;
        }
    }

    return 0 ;
}

static TALLIES_BLOCK_T *
block_find_overlap (const TALLIES_BLOCK_T * blk, int32_t base)
{
    TALLIES_BLOCK_T * entry ;

    for (entry = _tallies_blocks; entry; entry = entry->next) {
        if (entry == blk) {
            continue ;
        }
        if ((base < entry->base + (int32_t)entry->count) &&
            (entry->base < base + (int32_t)blk->count)) {
            return entry ;
        }
    }

    return 0 ;
}

/**
 * @brief   Zero a block and refill it from the volume.
 */
static void
block_load (TALLIES_BLOCK_T * blk)
{
    TALLIES_RECORD_T * record = (TALLIES_RECORD_T *)_tallies_scratch ;
    uint16_t local ;

    for (local = 0; local < blk->count; local++) {
        char name[TALLIES_MAX_NAME_LEN] ;

        blk->dirty[local] = TALLIES_CLEAN ;
        blk->entry[local].date.date = 0 ;
        blk->entry[local].time.time = 0 ;
        blk->entry[local].value = 0 ;

        if (!_tallies_inst->dict || !record) {
            continue ;
        }

        record->key = (uint32_t)(blk->base + local) ;
        if (nvol3_record_get (_tallies_inst, (NVOL3_RECORD_T *)record) <
                    (int32_t)TALLIES_RECORD_LEN) {
            continue ;
        }

        name_canonical (blk->defs[local].name, name) ;
        if (memcmp (record->data.name, name, TALLIES_MAX_NAME_LEN) != 0) {
            /*
             * Someone else's counter is sitting on this id - the module
             * numbering changed, or an id was reused. Drop it and start at
             * zero rather than inherit a history that is not ours.
             */
            char stored[TALLIES_MAX_NAME_LEN + 1] ;
            memcpy (stored, record->data.name, TALLIES_MAX_NAME_LEN) ;
            stored[TALLIES_MAX_NAME_LEN] = '\0' ;

            DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_WARNING,
                    "TALY  :W: '%s.%s' id %d holds '%s' - reset",
                    blk->name, blk->defs[local].name,
                    (int32_t)(blk->base + local), stored) ;

            record->key = (uint32_t)(blk->base + local) ;
            nvol3_record_delete (_tallies_inst, (NVOL3_RECORD_T *)record) ;
            _tallies_mismatched++ ;
            continue ;
        }

        blk->entry[local] = record->data.entry ;
    }
}

static int32_t
block_persist_locked (TALLIES_BLOCK_T * blk, uint16_t local)
{
    TALLIES_RECORD_T * record = (TALLIES_RECORD_T *)_tallies_scratch ;

    if (!record || !_tallies_inst->dict) {
        return E_UNEXP ;
    }

    memset (record, 0, sizeof(TALLIES_RECORD_T)) ;
    record->key = (uint32_t)(blk->base + local) ;
    name_canonical (blk->defs[local].name, record->data.name) ;
    record->data.entry = blk->entry[local] ;

    return nvol3_record_set (_tallies_inst, (NVOL3_RECORD_T *)record,
                    TALLIES_RECORD_LEN) ;
}

/**
 * @brief   Write out dirty entries, at most @p max of them.
 * @param[in] all   also sample running timers.
 * @param[in] max   0 for no bound.
 * @return  the number of entries still needing a write.
 */
static uint32_t
tallies_persist_bounded (uint32_t all, uint32_t max)
{
    TALLIES_BLOCK_T * blk ;
    uint32_t written = 0 ;
    uint32_t remaining = 0 ;

    /*
     * Not gated on _tallies_started: tallies_stop() flushes after clearing it.
     */
    if (!_tallies_scratch) {
        return 0 ;
    }

    os_mutex_lock (&_tallies_mutex) ;

    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;

        if (blk->base < 0) {
            continue ;
        }

        for (local = 0; local < blk->count; local++) {

            if ((blk->dirty[local] == TALLIES_CLEAN) ||
                (blk->dirty[local] == TALLIES_DIRTY_TIMER_PAUSED)) {
                continue ;
            }

            if (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) {
                uint32_t seconds ;

                if (!all) {
                    continue ;
                }

                seconds = rtc_seconds_elapsed (blk->entry[local].date,
                                blk->entry[local].time) ;
                if (!seconds) {
                    continue ;
                }

                /*
                 * An interval longer than the persist period cannot have come
                 * from a timer this pass would have sampled: the clock was
                 * stepped, or the store was down. Counting it would corrupt
                 * the total, so it is dropped - and counted, so that the drop
                 * is visible in "tallies" rather than silent.
                 */
                if (seconds < TALLIES_PERSIST_INTERVAL*2/3) {
                    blk->entry[local].value += seconds ;
                } else {
                    _tallies_timer_drops++ ;
                }
                rtc_localtime (rtc_time(), &blk->entry[local].date,
                                &blk->entry[local].time) ;
            }

            /*
             * Bound the work per tick. The flag is only cleared once the write
             * has actually happened, so what is left over is picked up by the
             * next pass rather than lost.
             */
            if (max && (written >= max)) {
                remaining++ ;
                continue ;
            }

            if (blk->dirty[local] == TALLIES_DIRTY_VALUE) {
                blk->dirty[local] = TALLIES_CLEAN ;
            }

            block_persist_locked (blk, local) ;
            written++ ;
        }
    }

    os_mutex_unlock (&_tallies_mutex) ;

    return remaining ;
}

static void
tallies_cb (SVC_TASKS_T * task, uintptr_t parm, uint32_t reason)
{
    if ((reason == SERVICE_CALLBACK_REASON_RUN) && _tallies_started) {
        uint32_t remaining ;
        uint32_t nexttime ;

        remaining = tallies_persist_bounded (1, TALLIES_PERSIST_MAX_PER_PASS) ;
        nexttime = remaining ?
                SVC_TASK_S2TICKS(TALLIES_PERSIST_RESUME) :
                SVC_TASK_S2TICKS(TALLIES_PERSIST_INTERVAL) ;

        if (_tallies_started) {
            svc_tasks_schedule (&_tallies_task, tallies_cb, parm,
                    SERVICE_PRIO_QUEUE3, nexttime) ;
        }
    }

    svc_tasks_complete (task) ;
}

/*===========================================================================*/
/* Lifecycle.                                                                */
/*===========================================================================*/

int32_t
tallies_init (NVOL3_INSTANCE_T * inst)
{
    if (!inst) {
        return E_PARM ;
    }

    keep_talliescmds () ;

    /*
     * The mutex is created here, not in tallies_start(): a registered block
     * can be counted into before the volume is up, and every mutation takes
     * the lock.
     */
    if (!_tallies_mutex && (os_mutex_create (&_tallies_mutex) != EOK)) {
        return EFAIL ;
    }

    _tallies_inst = inst ;

    return EOK ;
}

int32_t
tallies_start (void)
{
    TALLIES_BLOCK_T * blk ;
    int32_t status ;

    if (!_tallies_inst) {
        return E_UNEXP ;
    }
    if (_tallies_started) {
        return EOK ;
    }

    _tallies_scratch = NVOL3_MALLOC (_tallies_inst->config->record_size) ;
    if (!_tallies_scratch) {
        return E_NOMEM ;
    }

    status = nvol3_validate (_tallies_inst) ;
    if (status == EOK) {
        status = nvol3_load (_tallies_inst) ;
    }
    if (status != EOK) {
        status = nvol3_reset (_tallies_inst) ;
    }

    /*
     * Carry on even if the volume could not be brought up. The counters still
     * work in RAM, which is the whole point of keeping them there - and one of
     * them may well be the record of why the volume failed.
     */
    if (status != EOK) {
        DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                "TALY  :E: volume unavailable (%d), counting in RAM only",
                status) ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    _tallies_paused = 0 ;
    for (blk = _tallies_blocks; blk; blk = blk->next) {
        if (blk->base >= 0) {
            block_load (blk) ;
        }
    }
    _tallies_started = 1 ;
    os_mutex_unlock (&_tallies_mutex) ;

    svc_tasks_cancel (&_tallies_task) ;
    svc_tasks_schedule (&_tallies_task, tallies_cb, 0, SERVICE_PRIO_QUEUE3,
            SVC_TASK_S2TICKS(TALLIES_PERSIST_INTERVAL)) ;

    return status ;
}

void
tallies_stop (void)
{
    TALLIES_BLOCK_T * blk ;

    if (!_tallies_started) {
        return ;
    }

    /* Clear first so the persist task does not reschedule itself under us. */
    _tallies_started = 0 ;
    svc_tasks_cancel_wait (&_tallies_task, 1000) ;

    /* Stop the running timers so their elapsed seconds are banked, then flush. */
    os_mutex_lock (&_tallies_mutex) ;
    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;
        for (local = 0; local < blk->count; local++) {
            if (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) {
                stop_timer_locked (blk, local) ;
            }
        }
    }
    os_mutex_unlock (&_tallies_mutex) ;

    tallies_persist_bounded (1, 0) ;

    nvol3_unload (_tallies_inst) ;
    NVOL3_FREE (_tallies_scratch) ;
    _tallies_scratch = 0 ;
}

void
tallies_reset (void)
{
    TALLIES_BLOCK_T * blk ;

    if (!_tallies_started) {
        return ;
    }

    os_mutex_lock (&_tallies_mutex) ;

    nvol3_reset (_tallies_inst) ;

    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;
        for (local = 0; local < blk->count; local++) {
            int32_t running = (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) ;

            blk->entry[local].value = 0 ;
            blk->entry[local].date.date = 0 ;
            blk->entry[local].time.time = 0 ;
            blk->dirty[local] = TALLIES_DIRTY_VALUE ;

            if (running) {
                rtc_localtime (rtc_time(), &blk->entry[local].date,
                                &blk->entry[local].time) ;
                blk->dirty[local] = _tallies_paused ?
                        TALLIES_DIRTY_TIMER_PAUSED : TALLIES_DIRTY_TIMER_RUNNING ;
            }
        }
    }

    _tallies_mismatched = 0 ;
    _tallies_timer_drops = 0 ;

    os_mutex_unlock (&_tallies_mutex) ;

    tallies_persist_bounded (1, 0) ;
}

/*===========================================================================*/
/* Registration.                                                             */
/*===========================================================================*/

int32_t
tallies_register (TALLIES_BLOCK_T * blk, int32_t base)
{
    TALLIES_BLOCK_T * tail ;
    TALLIES_BLOCK_T * clash ;
    uint16_t local ;

    if (!blk || !blk->name || !blk->defs || !blk->entry || !blk->dirty ||
            !blk->count || (base < 0)) {
        DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                "TALY  :E: register '%s' base %d invalid res=%d",
                (blk && blk->name) ? blk->name : "(null)", base, E_PARM) ;
        return E_PARM ;
    }

    for (local = 0; local < blk->count; local++) {
        if (!blk->defs[local].name ||
                (strlen (blk->defs[local].name) > TALLIES_MAX_NAME_LEN - 1)) {
            DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                    "TALY  :E: register '%s' name %d too long (max %d) res=%d",
                    blk->name, (int32_t)local, TALLIES_MAX_NAME_LEN - 1, E_PARM) ;
            return E_PARM ;
        }
    }

    if (block_find (blk)) {
        return (blk->base == base) ? EOK : E_BUSY ;
    }

    clash = block_find_overlap (blk, base) ;
    if (clash) {
        DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                "TALY  :E: register '%s' base %d overlaps '%s' base %d res=%d",
                blk->name, base, clash->name, clash->base, E_BUSY) ;
        return E_BUSY ;
    }

    blk->base = base ;
    blk->next = 0 ;

    if (_tallies_blocks) {
        for (tail = _tallies_blocks; tail->next; tail = tail->next) {
            /* find tail */
        }
        tail->next = blk ;
    } else {
        _tallies_blocks = blk ;
    }

    /*
     * Registration order is not fixed: modules register at service CTRL_INIT,
     * which may be before or after tallies_start(). A block that arrives late
     * is populated here instead.
     */
    if (_tallies_started) {
        os_mutex_lock (&_tallies_mutex) ;
        block_load (blk) ;
        os_mutex_unlock (&_tallies_mutex) ;
    }

    return EOK ;
}

int32_t
tallies_unregister (TALLIES_BLOCK_T * blk)
{
    TALLIES_BLOCK_T * entry = _tallies_blocks ;
    TALLIES_BLOCK_T * prev = 0 ;

    if (!blk) {
        return E_PARM ;
    }

    while (entry) {
        if (entry == blk) {
            if (prev) {
                prev->next = entry->next ;
            } else {
                _tallies_blocks = entry->next ;
            }

            entry->next = 0 ;
            entry->base = -1 ;
            return EOK ;
        }

        prev = entry ;
        entry = entry->next ;
    }

    return E_NOTFOUND ;
}

/*===========================================================================*/
/* Counters.                                                                 */
/*===========================================================================*/

int32_t
tallies_inc (TALLIES_BLOCK_T * blk, uint16_t local)
{
    return tallies_add (blk, local, 1) ;
}

int32_t
tallies_add (TALLIES_BLOCK_T * blk, uint16_t local, uint32_t value)
{
    int32_t status ;

    if (tallies_ready (blk, local) != EOK) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    status = rate_limited_locked (blk, local) ;
    if (status == EOK) {
        rtc_localtime (rtc_time(), &blk->entry[local].date,
                        &blk->entry[local].time) ;
        blk->entry[local].value += value ;
        blk->dirty[local] = TALLIES_DIRTY_VALUE ;
    }
    os_mutex_unlock (&_tallies_mutex) ;

    return status ;
}

int32_t
tallies_set (TALLIES_BLOCK_T * blk, uint16_t local, uint32_t value)
{
    if (tallies_ready (blk, local) != EOK) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    rtc_localtime (rtc_time(), &blk->entry[local].date, &blk->entry[local].time) ;
    blk->entry[local].value = value ;
    blk->dirty[local] = TALLIES_DIRTY_VALUE ;
    os_mutex_unlock (&_tallies_mutex) ;

    return EOK ;
}

int32_t
tallies_clear (TALLIES_BLOCK_T * blk, uint16_t local)
{
    if (tallies_ready (blk, local) != EOK) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    blk->entry[local].value = 0 ;
    blk->entry[local].date.date = 0 ;
    blk->entry[local].time.time = 0 ;
    blk->dirty[local] = TALLIES_DIRTY_VALUE ;
    os_mutex_unlock (&_tallies_mutex) ;

    return EOK ;
}

/*===========================================================================*/
/* Timers.                                                                   */
/*===========================================================================*/

int32_t
tallies_start_timer (TALLIES_BLOCK_T * blk, uint16_t local)
{
    int32_t status = E_UNEXP ;

    if (tallies_ready (blk, local) != EOK) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    if (blk->dirty[local] != TALLIES_DIRTY_TIMER_RUNNING) {
        rtc_localtime (rtc_time(), &blk->entry[local].date,
                        &blk->entry[local].time) ;
        blk->dirty[local] = _tallies_paused ?
                TALLIES_DIRTY_TIMER_PAUSED : TALLIES_DIRTY_TIMER_RUNNING ;
        status = EOK ;
    }
    os_mutex_unlock (&_tallies_mutex) ;

    return status ;
}

static int32_t
stop_timer_locked (TALLIES_BLOCK_T * blk, uint16_t local)
{
    if (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) {
        uint32_t seconds = rtc_seconds_elapsed (blk->entry[local].date,
                        blk->entry[local].time) ;

        /* See tallies_persist_bounded() for why a long interval is dropped. */
        if (seconds < TALLIES_PERSIST_INTERVAL*2/3) {
            blk->entry[local].value += seconds ;
        } else {
            _tallies_timer_drops++ ;
        }

        rtc_localtime (rtc_time(), &blk->entry[local].date,
                        &blk->entry[local].time) ;
        blk->dirty[local] = TALLIES_DIRTY_VALUE ;
        return EOK ;
    }

    if (blk->dirty[local] == TALLIES_DIRTY_TIMER_PAUSED) {
        blk->dirty[local] = TALLIES_DIRTY_VALUE ;
        return EOK ;
    }

    return E_UNEXP ;
}

int32_t
tallies_stop_timer (TALLIES_BLOCK_T * blk, uint16_t local)
{
    int32_t status ;

    if (tallies_ready (blk, local) != EOK) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    status = stop_timer_locked (blk, local) ;
    os_mutex_unlock (&_tallies_mutex) ;

    return status ;
}

void
tallies_timers_pause (void)
{
    TALLIES_BLOCK_T * blk ;

    os_mutex_lock (&_tallies_mutex) ;
    _tallies_paused = 1 ;

    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;
        for (local = 0; local < blk->count; local++) {
            if (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) {
                stop_timer_locked (blk, local) ;
                blk->dirty[local] = TALLIES_DIRTY_TIMER_PAUSED ;
            }
        }
    }
    os_mutex_unlock (&_tallies_mutex) ;
}

void
tallies_timers_resume (void)
{
    TALLIES_BLOCK_T * blk ;

    os_mutex_lock (&_tallies_mutex) ;
    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;
        for (local = 0; local < blk->count; local++) {
            if (blk->dirty[local] == TALLIES_DIRTY_TIMER_PAUSED) {
                rtc_localtime (rtc_time(), &blk->entry[local].date,
                                &blk->entry[local].time) ;
                blk->dirty[local] = TALLIES_DIRTY_TIMER_RUNNING ;
            }
        }
    }
    _tallies_paused = 0 ;
    os_mutex_unlock (&_tallies_mutex) ;
}

/*===========================================================================*/
/* Read back.                                                                */
/*===========================================================================*/

uint32_t
tallies_get_value (TALLIES_BLOCK_T * blk, uint16_t local)
{
    uint32_t value ;

    if (tallies_ready (blk, local) != EOK) {
        return 0 ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    value = blk->entry[local].value ;
    if (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) {
        value += rtc_seconds_elapsed (blk->entry[local].date,
                        blk->entry[local].time) ;
    }
    os_mutex_unlock (&_tallies_mutex) ;

    return value ;
}

int32_t
tallies_get (TALLIES_BLOCK_T * blk, uint16_t local, TALLIES_ENTRY_T * out)
{
    if (!out || (tallies_ready (blk, local) != EOK)) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    *out = blk->entry[local] ;
    if (blk->dirty[local] == TALLIES_DIRTY_TIMER_RUNNING) {
        out->value += rtc_seconds_elapsed (blk->entry[local].date,
                        blk->entry[local].time) ;
    }
    os_mutex_unlock (&_tallies_mutex) ;

    return EOK ;
}

const char *
tallies_name (TALLIES_BLOCK_T * blk, uint16_t local)
{
    if (!blk || (local >= blk->count)) {
        return 0 ;
    }

    return blk->defs[local].name ;
}

void
tallies_persist (uint32_t all)
{
    tallies_persist_bounded (all, 0) ;
}

TALLIES_BLOCK_T *
tallies_block_first (void)
{
    return _tallies_blocks ;
}

TALLIES_BLOCK_T *
tallies_block_next (TALLIES_BLOCK_T * cur)
{
    return cur ? cur->next : 0 ;
}

uint32_t
tallies_mismatched (void)
{
    return _tallies_mismatched ;
}

uint32_t
tallies_timer_drops (void)
{
    return _tallies_timer_drops ;
}

void
tallies_log_status (void)
{
    if (_tallies_inst && _tallies_inst->dict) {
        nvol3_entry_log_status (_tallies_inst, 1) ;
    }
}

#endif /* CONFIG_QORAAL_FLASH_TALLIES */
