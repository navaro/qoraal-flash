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
static uint32_t             _tallies_mismatched = 0 ;

/*
 * Two locks, and they are not interchangeable.
 *
 * _tallies_mutex covers the counters - entry[] and dirty[]. Every hot path
 * takes it, so it is only ever held for a handful of instructions and never
 * across a FLASH access.
 *
 * _tallies_volume covers the nvol3 instance and the scratch record below.
 * This is the one held across FLASH, which on a volume whose sectors are
 * erased in place is milliseconds, not microseconds. A counter raised on an
 * error path must not queue behind that, which is the whole reason the two
 * are separate.
 *
 * Where both are needed - the persist pass, the load - _tallies_volume is
 * taken first and _tallies_mutex taken and released inside it, once per
 * entry. Never the other way round.
 */
static p_mutex_t            _tallies_mutex = 0 ;
static p_mutex_t            _tallies_volume = 0 ;

/*
 * One scratch record for the whole store, allocated at start and reused.
 * nvol3 reads up to record_size into it, which is larger than the payload we
 * write, so a corrupt length cannot overrun a caller's buffer. It also keeps
 * the persist pass off the allocator - see the design note on record_set()
 * doing an NVOL3_MALLOC per call.
 */
static NVOL3_RECORD_T *     _tallies_scratch = 0 ;

static SVC_TASKS_DECL(_tallies_task) ;

static uint32_t entry_write (TALLIES_BLOCK_T * blk, uint16_t local) ;

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
    if (!_tallies_inst || !blk || (local >= blk->count) ||
            (blk->base == TALLIES_BASE_NONE)) {
        return E_PARM ;
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
block_find_overlap (const TALLIES_BLOCK_T * blk, uint32_t base)
{
    TALLIES_BLOCK_T * entry ;

    for (entry = _tallies_blocks; entry; entry = entry->next) {
        if ((entry == blk) || (entry->base == TALLIES_BASE_NONE)) {
            continue ;
        }
        if ((base < (uint32_t)entry->base + entry->count) &&
            ((uint32_t)entry->base < base + blk->count)) {
            return entry ;
        }
    }

    return 0 ;
}

/**
 * @brief   Zero a block and refill it from the volume.
 * @note    Called with the volume mutex held and the value mutex NOT held.
 *          The record is read from FLASH unlocked and only the handover into
 *          entry[] takes the value mutex, one entry at a time.
 */
static void
block_load (TALLIES_BLOCK_T * blk)
{
    TALLIES_RECORD_T * record = (TALLIES_RECORD_T *)_tallies_scratch ;
    uint16_t local ;

    for (local = 0; local < blk->count; local++) {
        char name[TALLIES_MAX_NAME_LEN] ;

        os_mutex_lock (&_tallies_mutex) ;
        blk->dirty[local] = TALLIES_CLEAN ;
        blk->entry[local].date.date = 0 ;
        blk->entry[local].time.time = 0 ;
        blk->entry[local].value = 0 ;
        os_mutex_unlock (&_tallies_mutex) ;

        if (!_tallies_inst->dict || !record) {
            continue ;
        }

        record->key = (uint32_t)(blk->base + local) ;
        if (nvol3_record_get (_tallies_inst, (NVOL3_RECORD_T *)record) <
                    (int32_t)TALLIES_RECORD_LEN) {
            continue ;
        }

        name_canonical (blk->defs[local], name) ;
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
                    blk->name, blk->defs[local],
                    (int32_t)(blk->base + local), stored) ;

            record->key = (uint32_t)(blk->base + local) ;
            nvol3_record_delete (_tallies_inst, (NVOL3_RECORD_T *)record) ;
            _tallies_mismatched++ ;
            continue ;
        }

        os_mutex_lock (&_tallies_mutex) ;
        blk->entry[local] = record->data.entry ;
        blk->dirty[local] = TALLIES_CLEAN ;
        os_mutex_unlock (&_tallies_mutex) ;
    }
}

/**
 * @brief   Snapshot one entry and write it out, if it is dirty.
 * @note    Called with the volume mutex held and the value mutex NOT held.
 *          The value mutex is taken twice and briefly - once to claim the
 *          entry and copy it into the scratch record, and again only if the
 *          write failed - so nothing counting into this block waits on FLASH.
 * @return  1 if a write was attempted, 0 if the entry was already clean.
 */
static uint32_t
entry_write (TALLIES_BLOCK_T * blk, uint16_t local)
{
    TALLIES_RECORD_T * record = (TALLIES_RECORD_T *)_tallies_scratch ;

    os_mutex_lock (&_tallies_mutex) ;

    if (blk->dirty[local] == TALLIES_CLEAN) {
        os_mutex_unlock (&_tallies_mutex) ;
        return 0 ;
    }

    /*
     * Claimed before the write rather than after it. A counter raised while
     * the write is in flight has to leave the entry dirty so that the next
     * pass writes the newer value; clearing the flag afterwards would drop
     * exactly that increment.
     */
    blk->dirty[local] = TALLIES_CLEAN ;

    memset (record, 0, sizeof(TALLIES_RECORD_T)) ;
    record->key = (uint32_t)(blk->base + local) ;
    name_canonical (blk->defs[local], record->data.name) ;
    record->data.entry = blk->entry[local] ;

    os_mutex_unlock (&_tallies_mutex) ;

    if (nvol3_record_set (_tallies_inst, (NVOL3_RECORD_T *)record,
                    TALLIES_RECORD_LEN) < EOK) {
        /*
         * Hand it back so the next pass retries. If it has been raised again
         * in the meantime it is dirty already and this changes nothing.
         */
        os_mutex_lock (&_tallies_mutex) ;
        blk->dirty[local] = TALLIES_DIRTY ;
        os_mutex_unlock (&_tallies_mutex) ;
    }

    return 1 ;
}

/**
 * @brief   Write out dirty entries, at most @p max of them.
 * @param[in] max   0 for no bound.
 * @return  the number of entries still needing a write.
 *
 * @note    The volume mutex is held for the whole pass - one pass at a time,
 *          and the scratch record is not shared with a concurrent load - but
 *          the value mutex is only taken per entry, inside entry_write().
 *          A pass is therefore not atomic against the counters: an entry
 *          raised after it was snapshotted stays dirty and goes out next
 *          time. For a running total whose last write wins that is the point.
 */
static uint32_t
tallies_persist_bounded (uint32_t max)
{
    TALLIES_BLOCK_T * blk ;
    uint32_t written = 0 ;
    uint32_t remaining = 0 ;

    os_mutex_lock (&_tallies_volume) ;

    /*
     * Not gated on _tallies_started: tallies_stop() flushes after clearing it.
     * With no volume there is nowhere to write and every entry stays dirty,
     * which is what "counting in RAM only" means.
     */
    if (!_tallies_scratch || !_tallies_inst || !_tallies_inst->dict) {
        os_mutex_unlock (&_tallies_volume) ;
        return 0 ;
    }

    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;

        if (blk->base == TALLIES_BASE_NONE) {
            continue ;
        }

        for (local = 0; local < blk->count; local++) {

            /*
             * Bound the work per tick. An entry is only claimed when it is
             * about to be written, so what is left over is picked up by the
             * next pass rather than lost.
             */
            if (max && (written >= max)) {
                os_mutex_lock (&_tallies_mutex) ;
                if (blk->dirty[local] != TALLIES_CLEAN) {
                    remaining++ ;
                }
                os_mutex_unlock (&_tallies_mutex) ;
                continue ;
            }

            written += entry_write (blk, local) ;
        }
    }

    os_mutex_unlock (&_tallies_volume) ;

    return remaining ;
}

static void
tallies_cb (SVC_TASKS_T * task, uintptr_t parm, uint32_t reason)
{
    if ((reason == SERVICE_CALLBACK_REASON_RUN) && _tallies_started) {
        uint32_t remaining ;
        uint32_t nexttime ;

        remaining = tallies_persist_bounded (TALLIES_PERSIST_MAX_PER_PASS) ;
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

    /*
     * The mutexes are created here, not in tallies_start(): a registered block
     * can be counted into before the volume is up, and every mutation takes
     * the value lock.
     */
    if (!_tallies_mutex && (os_mutex_create (&_tallies_mutex) != EOK)) {
        return EFAIL ;
    }
    if (!_tallies_volume && (os_mutex_create (&_tallies_volume) != EOK)) {
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

    os_mutex_lock (&_tallies_volume) ;

    _tallies_scratch = NVOL3_MALLOC (_tallies_inst->config->record_size) ;
    if (!_tallies_scratch) {
        os_mutex_unlock (&_tallies_volume) ;
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

    for (blk = _tallies_blocks; blk; blk = blk->next) {
        if (blk->base != TALLIES_BASE_NONE) {
            block_load (blk) ;
        }
    }

    os_mutex_unlock (&_tallies_volume) ;

    /* Last, so a block registering concurrently does not try to load twice. */
    os_mutex_lock (&_tallies_mutex) ;
    _tallies_started = 1 ;
    os_mutex_unlock (&_tallies_mutex) ;

    svc_tasks_cancel (&_tallies_task) ;
    svc_tasks_schedule (&_tallies_task, tallies_cb, 0, SERVICE_PRIO_QUEUE3,
            SVC_TASK_S2TICKS(TALLIES_PERSIST_INTERVAL_START)) ;

    return status ;
}

void
tallies_stop (void)
{
    if (!_tallies_started) {
        return ;
    }

    /* Clear first so the persist task does not reschedule itself under us. */
    _tallies_started = 0 ;
    svc_tasks_cancel_wait (&_tallies_task, 1000) ;

    tallies_persist_bounded (0) ;

    os_mutex_lock (&_tallies_volume) ;
    nvol3_unload (_tallies_inst) ;
    NVOL3_FREE (_tallies_scratch) ;
    _tallies_scratch = 0 ;
    os_mutex_unlock (&_tallies_volume) ;
}

void
tallies_reset (void)
{
    TALLIES_BLOCK_T * blk ;

    if (!_tallies_started) {
        return ;
    }

    /* The erase is the slow half and has no business holding the value lock. */
    os_mutex_lock (&_tallies_volume) ;
    nvol3_reset (_tallies_inst) ;
    os_mutex_unlock (&_tallies_volume) ;

    os_mutex_lock (&_tallies_mutex) ;

    for (blk = _tallies_blocks; blk; blk = blk->next) {
        uint16_t local ;
        for (local = 0; local < blk->count; local++) {
            blk->entry[local].value = 0 ;
            blk->entry[local].date.date = 0 ;
            blk->entry[local].time.time = 0 ;
            blk->dirty[local] = TALLIES_DIRTY ;
        }
    }

    _tallies_mismatched = 0 ;

    os_mutex_unlock (&_tallies_mutex) ;

    /*
     * Write the zeros back. Anything counted between the erase and here is
     * dirty already and goes out with them.
     */
    tallies_persist_bounded (0) ;
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

    /*
     * The id is a uint16_t, so the whole range a block claims has to fit below
     * the unregistered sentinel. A project's module numbering is what has to
     * respect that; this is where it is caught if it does not.
     */
    if (!blk || !blk->name || !blk->defs || !blk->entry || !blk->dirty ||
            !blk->count || (base < 0) ||
            ((uint32_t)base + blk->count > TALLIES_BASE_NONE)) {
        DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                "TALY  :E: register '%s' base %d invalid res=%d",
                (blk && blk->name) ? blk->name : "(null)", base, E_PARM) ;
        return E_PARM ;
    }

    for (local = 0; local < blk->count; local++) {
        if (!blk->defs[local] ||
                (strlen (blk->defs[local]) > TALLIES_MAX_NAME_LEN - 1)) {
            DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                    "TALY  :E: register '%s' name %d too long (max %d) res=%d",
                    blk->name, (int32_t)local, TALLIES_MAX_NAME_LEN - 1, E_PARM) ;
            return E_PARM ;
        }
    }

    if (block_find (blk)) {
        return (blk->base == (uint16_t)base) ? EOK : E_BUSY ;
    }

    clash = block_find_overlap (blk, (uint32_t)base) ;
    if (clash) {
        DBG_MESSAGE_TALLIES (DBG_MESSAGE_SEVERITY_ERROR,
                "TALY  :E: register '%s' base %d overlaps '%s' base %d res=%d",
                blk->name, base, clash->name, (int32_t)clash->base, E_BUSY) ;
        return E_BUSY ;
    }

    blk->base = (uint16_t)base ;
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
        os_mutex_lock (&_tallies_volume) ;
        block_load (blk) ;
        os_mutex_unlock (&_tallies_volume) ;
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
            entry->base = TALLIES_BASE_NONE ;
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
    if (tallies_ready (blk, local) != EOK) {
        return E_PARM ;
    }

    os_mutex_lock (&_tallies_mutex) ;
    rtc_localtime (rtc_time(), &blk->entry[local].date,
                    &blk->entry[local].time) ;
    blk->entry[local].value += value ;
    blk->dirty[local] = TALLIES_DIRTY ;
    os_mutex_unlock (&_tallies_mutex) ;

    return EOK ;
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
    blk->dirty[local] = TALLIES_DIRTY ;
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
    blk->dirty[local] = TALLIES_DIRTY ;
    os_mutex_unlock (&_tallies_mutex) ;

    return EOK ;
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
    os_mutex_unlock (&_tallies_mutex) ;

    return EOK ;
}

const char *
tallies_name (TALLIES_BLOCK_T * blk, uint16_t local)
{
    if (!blk || (local >= blk->count)) {
        return 0 ;
    }

    return blk->defs[local] ;
}

void
tallies_persist (void)
{
    tallies_persist_bounded (0) ;
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

int32_t
tallies_status_get (NVOL3_STATUS_T * status)
{
    int32_t res ;

    if (!status) {
        return E_PARM ;
    }
    if (!_tallies_inst) {
        return E_UNEXP ;
    }

    /*
     * The volume mutex covers the snapshot and nothing beyond it. Formatting
     * belongs to the caller, with the lock released - see the note on the
     * lock split in tallies.h.
     */
    os_mutex_lock (&_tallies_volume) ;
    res = nvol3_status_get (_tallies_inst, status) ;
    os_mutex_unlock (&_tallies_volume) ;

    return res ;
}

void
tallies_log_status (void)
{
    os_mutex_lock (&_tallies_volume) ;
    if (_tallies_inst && _tallies_inst->dict) {
        nvol3_entry_log_status (_tallies_inst, 1) ;
    }
    os_mutex_unlock (&_tallies_volume) ;
}

#endif /* CONFIG_QORAAL_FLASH_TALLIES */
