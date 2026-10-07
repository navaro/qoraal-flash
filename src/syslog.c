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
#if !defined CFG_SYSLOG_DISABLE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

#include "qoraal/qoraal.h"
#include "qoraal-flash/syslog.h"
#include "qoraal-flash/nvram/nlog3.h"

/*
 * One syslog line is one nlog3 record. The fields a reader filters and prints
 * live in the record descriptor; the payload is the message text without its
 * terminator.
 */
#define SYSLOG_DESC_ID                  0U
#define SYSLOG_DESC_DATE                1U
#define SYSLOG_DESC_TIME                2U
#define SYSLOG_DESC_FACILITY_SEVERITY   3U      /* facility low 16, severity high 16 */
#define SYSLOG_DESC_REQUIRED_WORDS      4U

typedef char syslog_desc_word_count[
    (NLOG3_RECORD_USER_WORDS >= SYSLOG_DESC_REQUIRED_WORDS) ? 1 : -1] ;

static p_mutex_t            _syslog_mutex ;
static char                 _syslog_msg_buffer[SYSLOGLOG_MAX_MSG_SIZE] ;
static SYSLOG_INSTANCE_T *  _syslog_instance = 0 ;
static uint32_t             _syslog_instance_cnt = 0 ;
static uint32_t             _syslog_id[SYSLOG_LOG_MAX] ;


typedef struct _SYSLOG_IT_S {
    QORAAL_LOG_IT_T platform_it ;
    SYSLOG_ITERATOR_T it ;
} _SYSLOG_IT_T ;


/*
 * nlog3 keeps no record counter, so the ids carry on from the newest record
 * found at start.
 */
static uint32_t
_syslog_next_id (NLOG3_T * plog)
{
    NLOG3_ITERATOR_T it ;
    NLOG3_RECORD_DESC_T desc ;

    if ((nlog3_iterator_init (plog, &it) != EOK) ||
            (nlog3_iterator_desc (&it, &desc) != EOK)) {
        return 0 ;
    }

    return desc.user[SYSLOG_DESC_ID] + 1 ;
}

/* Called with the mutex held. */
static void
_syslog_write (uint32_t idx, uint16_t facillity, uint16_t severity, const char* msg, uint32_t len)
{
    NLOG3_RECORD_DESC_T desc ;
    RTCLIB_DATE_T date ;
    RTCLIB_TIME_T time ;

    rtc_localtime (rtc_time(), &date, &time) ;

    memset (&desc, 0, sizeof(desc)) ;
    desc.user[SYSLOG_DESC_ID] = _syslog_id[idx] ;
    desc.user[SYSLOG_DESC_DATE] = date.date ;
    desc.user[SYSLOG_DESC_TIME] = time.time ;
    desc.user[SYSLOG_DESC_FACILITY_SEVERITY] = (uint32_t)facillity | ((uint32_t)severity << 16) ;

    if (nlog3_append (&_syslog_instance->log[idx], &desc, len, msg) == EOK) {
        _syslog_id[idx]++ ;
    }
}


/**
 * @brief   Starts nvol service.
 *
 * @param[in] parm    input parameter
 *
 * @return              Error.
 *
 * @init
 */
int32_t syslog_init (SYSLOG_INSTANCE_T * inst)
{
    _syslog_instance = inst ;
    _syslog_instance_cnt = 0 ;

    return EOK ;
}


/**
 * @brief   Starts nvol service.
 *
 * @param[in] parm    input parameter
 *
 * @return              Error.
 *
 * @init
 */
int32_t syslog_start (void)
{
    int32_t status = 0 ;
    uint32_t cnt ;

    if (!_syslog_instance) {
        return E_UNEXP ;
    }

    if (os_mutex_create (&_syslog_mutex) != EOK) {
        return EFAIL ;
    }

    for (cnt=0; cnt<SYSLOG_LOG_MAX; cnt++) {
        status = nlog3_init (&_syslog_instance->log[cnt]) ;
        if (status != EOK) {
            break ;

        }

        _syslog_id[cnt] = _syslog_next_id (&_syslog_instance->log[cnt]) ;

    }

    _syslog_instance_cnt = cnt ;

    return status ? EFAIL : EOK ;
}

/**
 * @brief   Stops nvol service.
 *
 * @param[in] parm    input parameter
 *
 * @return              Error.
 *
 * @init
 */
int32_t syslog_stop (void)
{
    if (!_syslog_instance) {
        return E_UNEXP ;
    }

    _syslog_instance_cnt = 0 ;
    os_mutex_lock (&_syslog_mutex) ;
    os_mutex_unlock (&_syslog_mutex) ;
    os_mutex_delete (&_syslog_mutex) ;

    return  EOK ;
}

/**
 * @brief   Starts nvol service.
 *
 * @param[in] parm    input parameter
 *
 * @return              Error.
 *
 * @init
 */
int32_t syslog_reset (uint32_t idx)
{
    int32_t res ;
    
    if (!_syslog_instance) {
        return E_UNEXP ;
    }    
    if (idx >= _syslog_instance_cnt) {
        return EFAIL;
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog3_reset (&_syslog_instance->log[idx]) ;
    os_mutex_unlock (&_syslog_mutex) ;

    return res ;
}


/**
 * @brief   nlog_append
 *
 * @param[in] msg    msg
 *
 * @return              Error.
 *
 * @init
 */
void
syslog_append (uint32_t idx, uint16_t facillity, uint16_t severity, const char* msg)
{
    uint32_t len = 0 ;

    if (!_syslog_instance) {
        return ;
    }
    if (idx >= _syslog_instance_cnt) {
        return ;
    }

    while ((len < SYSLOGLOG_MAX_MSG_SIZE - 1) && msg[len]) {
        len++ ;
    }

    os_mutex_lock (&_syslog_mutex) ;
    _syslog_write (idx, facillity, severity, msg, len) ;
    os_mutex_unlock (&_syslog_mutex) ;

}


void
syslog_vappend_fmtstr (int32_t idx, int16_t facillity, int16_t severity, const char* format, va_list    args)
{
    int len ;

    if (!_syslog_instance) {
        return ;
    }    
    if ((uint32_t)idx >= _syslog_instance_cnt) {
        return ;
    }


    os_mutex_lock (&_syslog_mutex) ;
    len = vsnprintf (_syslog_msg_buffer, sizeof(_syslog_msg_buffer), format, args) ;
    if (len < 0) {
        len = 0 ;
    } else if (len >= (int)sizeof(_syslog_msg_buffer)) {
        len = sizeof(_syslog_msg_buffer) - 1 ;
    }

    _syslog_write ((uint32_t)idx, (uint16_t)facillity, (uint16_t)severity,
            _syslog_msg_buffer, (uint32_t)len) ;

    os_mutex_unlock (&_syslog_mutex) ;

}


/**
 * @brief   nlog_append
 *
 * @param[in] msg    msg
 *
 * @return              Error.
 *
 * @init
 */
void
syslog_append_fmtstr (uint32_t idx, uint16_t facillity, uint16_t severity, const char* format, ...)
{
    va_list         args;
    va_start (args, format) ;
    syslog_vappend_fmtstr (idx, facillity, severity, format, args) ;
    va_end (args) ;
}


/* 1 when the record under the iterator is at or above the severity threshold. */
static int32_t
_syslog_iterator_match (SYSLOG_ITERATOR_T *it)
{
    NLOG3_RECORD_DESC_T desc ;
    int32_t res ;

    res = nlog3_iterator_desc (&it->it, &desc) ;
    if (res != EOK) {
        return res ;
    }

    return ((desc.user[SYSLOG_DESC_FACILITY_SEVERITY] >> 16) <= it->severity) ? 1 : 0 ;
}

/* Steps with move until a record matches, starting with the current one. */
static int32_t
_syslog_iterator_find (SYSLOG_ITERATOR_T *it, int32_t (*move)(NLOG3_ITERATOR_T *it))
{
    int32_t res ;

    while ((res = _syslog_iterator_match (it)) == 0) {
        res = move (&it->it) ;
        if (res != EOK) {
            return res ;
        }
    }

    return (res > 0) ? EOK : res ;
}

/* Moves one matching record; the iterator stays put when there is none. */
static int32_t
_syslog_iterator_step (SYSLOG_ITERATOR_T *it, int32_t (*move)(NLOG3_ITERATOR_T *it))
{
    NLOG3_ITERATOR_T saved = it->it ;
    int32_t res ;

    os_mutex_lock (&_syslog_mutex) ;
    res = move (&it->it) ;
    if (res == EOK) {
        res = _syslog_iterator_find (it, move) ;
    }
    os_mutex_unlock (&_syslog_mutex) ;

    if (res != EOK) {
        it->it = saved ;
    }

    return res ;
}


int32_t
syslog_iterator_init (uint32_t idx, uint16_t severity, SYSLOG_ITERATOR_T *it)
{
    int32_t res ;

    if (!_syslog_instance) {
        return E_UNEXP ;
    }
    if (idx >= _syslog_instance_cnt) {
        return E_PARM ;
    }

    it->severity = severity ;

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog3_iterator_init (&_syslog_instance->log[idx], &it->it) ;
    if (res == EOK) {
        res = _syslog_iterator_find (it, nlog3_iterator_prev) ;
    }
    os_mutex_unlock (&_syslog_mutex) ;

    return (res == E_BOF) ? E_EMPTY : res ;
}

int32_t
syslog_iterator_prev (SYSLOG_ITERATOR_T *it)
{
    if (!_syslog_instance) {
        return E_UNEXP ;
    }    
    return _syslog_iterator_step (it, nlog3_iterator_prev) ;
}

int32_t
syslog_iterator_next (SYSLOG_ITERATOR_T *it)
{
    if (!_syslog_instance) {
        return E_UNEXP ;
    }    
    return _syslog_iterator_step (it, nlog3_iterator_next) ;
}

int32_t
syslog_iterator_read (SYSLOG_ITERATOR_T *it, QORAAL_LOG_MSG_T *msg, uint32_t len)
{
    NLOG3_RECORD_DESC_T desc ;
    uint32_t fs ;
    int32_t res ;

    if (!_syslog_instance) {
        return E_UNEXP ;
    }

    /* room for the header, at least one character and the terminator */
    if (len < sizeof(QORAAL_LOG_MSG_T) + 2) {
        return E_PARM ;
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog3_iterator_desc (&it->it, &desc) ;
    if (res == EOK) {
        res = nlog3_iterator_read (&it->it, msg->msg,
                (int32_t)(len - sizeof(QORAAL_LOG_MSG_T) - 1)) ;
    }
    os_mutex_unlock (&_syslog_mutex) ;

    if (res < 0) {
        return res ;
    }

    fs = desc.user[SYSLOG_DESC_FACILITY_SEVERITY] ;
    msg->date.date = desc.user[SYSLOG_DESC_DATE] ;
    msg->time.time = desc.user[SYSLOG_DESC_TIME] ;
    msg->cnt = 0 ;
    msg->id = (uint16_t)desc.user[SYSLOG_DESC_ID] ;
    msg->severity = (uint8_t)(fs >> 16) ;
    msg->facillity = (uint8_t)(fs & 0xFFFF) ;
    msg->len = (uint16_t)res ;
    msg->msg[res] = '\0' ;

    return (int32_t)sizeof(QORAAL_LOG_MSG_T) + res ;

}

static int32_t
_it_prev(struct QORAAL_LOG_IT_S * it)
{
    _SYSLOG_IT_T *syslogit = (_SYSLOG_IT_T*) it ;
    return syslog_iterator_prev (&syslogit->it) ;

}

static int32_t
_it_get(struct QORAAL_LOG_IT_S * it, QORAAL_LOG_MSG_T * msg, uint32_t len)
{
    _SYSLOG_IT_T *syslogit = (_SYSLOG_IT_T*) it ;
    return syslog_iterator_read (&syslogit->it, msg, len) ;
}


QORAAL_LOG_IT_T *
syslog_platform_it_create (uint32_t idx)
{
    if (!_syslog_instance) {
        return 0 ;
    }    
    if (idx >= _syslog_instance_cnt) {
        return 0 ;
    }

    _SYSLOG_IT_T * it = qoraal_malloc(QORAAL_HeapAuxiliary, sizeof(_SYSLOG_IT_T)) ;
    if (it) {
        if (syslog_iterator_init (idx, SYSLOG_SEVERITY_DEBUG, &it->it) != EOK) {
            qoraal_free (QORAAL_HeapAuxiliary, it) ;
            it = 0 ;

        } else {
            it->platform_it.prev = _it_prev ;
            it->platform_it.get = _it_get ;
        }
    }

    return (QORAAL_LOG_IT_T *)it ;
}

void
syslog_platform_it_destroy (QORAAL_LOG_IT_T * it)
{
    qoraal_free (QORAAL_HeapAuxiliary, it) ;
}

#endif

