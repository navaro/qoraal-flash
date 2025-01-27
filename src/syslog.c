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
#include "qoraal-flash/nvram/nlog2.h"


extern void     keep_syslogcmds (void) ;


static p_mutex_t    _syslog_mutex ;

static char                              _syslog_log_buffer[SYSLOGLOG_MAX_MSG_SIZE + sizeof(QORAAL_LOG_MSG_T)]   ;

#define LOG_NLOG2_MAX   2

NLOG2_T * _log_nlog2[LOG_NLOG2_MAX] = { 0 } ;

//        NLOG2_LOG_DATA(STORAGE_NLOG2_VOL1_START, STORAGE_NLOG2_VOL1_SECTOR_COUNT, STORAGE_NLOG2_VOL1_SECTOR_SIZE),
//        NLOG2_LOG_DATA(STORAGE_NLOG2_VOL2_START, STORAGE_NLOG2_VOL2_SECTOR_COUNT, STORAGE_NLOG2_VOL2_SECTOR_SIZE),


typedef struct _SYSLOG_IT_S {
    QORAAL_LOG_IT_T platform_it ;
    SYSLOG_ITERATOR_T it ;
} _SYSLOG_IT_T ;

static bool _syslog_started = false ;

/**
 * @brief   Starts nvol service.
 *
 * @param[in] parm    input parameter
 *
 * @return              Error.
 *
 * @init
 */
int32_t syslog_init (void)
{
    keep_syslogcmds () ; 

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
int32_t syslog_start (NLOG2_T * info_log, NLOG2_T * assert_log)
{
    int i ;
    int32_t status = 0 ;

    if (info_log) {
        _log_nlog2[0] = info_log ;
        status = nlog2_init (_log_nlog2[0]) ;
        if (status != 0) {
            return EFAIL ;
            
        }

    }
    if (assert_log) {
        _log_nlog2[1] = assert_log ;
        status = nlog2_init (_log_nlog2[1]) ;
        if (status != 0) {
            return EFAIL ;

        }

    }

    if (os_mutex_create (&_syslog_mutex) != EOK) {
        return EFAIL ;
    }
    
    _syslog_started = true ;

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
    os_mutex_lock (&_syslog_mutex) ;
    _syslog_started = false ;
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
    if (idx >= LOG_NLOG2_MAX || !_log_nlog2[idx]) {
        return EFAIL;
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog2_reset (_log_nlog2[idx]) ;
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
    QORAAL_LOG_MSG_T * syslog = (QORAAL_LOG_MSG_T*) _syslog_log_buffer ;

    if (!_syslog_started) {
        return ;       
    }
    if (idx >= LOG_NLOG2_MAX || !_log_nlog2[idx]) {
        return ;
    }


    os_mutex_lock (&_syslog_mutex) ;

    syslog->id = nlog2_get_id (_log_nlog2[idx]) ;
    rtc_localtime (rtc_time(), &syslog->date, &syslog->time) ;
    syslog->facillity = facillity ;
    syslog->severity = severity ;
    strncpy (syslog->msg, msg, SYSLOGLOG_MAX_MSG_SIZE - 1) ;

    nlog2_append (_log_nlog2[idx], 1 << severity, _syslog_log_buffer,
            sizeof(QORAAL_LOG_MSG_T) + strlen(msg) + 1) ;

    os_mutex_unlock (&_syslog_mutex) ;


}


void
syslog_vappend_fmtstr (int32_t idx, int16_t facillity, int16_t severity, const char* format, va_list    args)
{
    QORAAL_LOG_MSG_T * syslog = (QORAAL_LOG_MSG_T*) _syslog_log_buffer ;

    if (!_syslog_started) {
        return ;       
    }
    if (idx >= LOG_NLOG2_MAX || !_log_nlog2[idx]) {
        return ;
    }


    os_mutex_lock (&_syslog_mutex) ;
    syslog->id = nlog2_get_id (_log_nlog2[idx]) ;
    rtc_localtime (rtc_time(), &syslog->date, &syslog->time) ;
    syslog->facillity = facillity ;
    syslog->severity = severity ;
    syslog->len = vsnprintf (syslog->msg, SYSLOGLOG_MAX_MSG_SIZE - 1, format, args) ;

    nlog2_append (_log_nlog2[idx], 1 << severity, _syslog_log_buffer,
            sizeof(QORAAL_LOG_MSG_T) + syslog->len + 1) ;

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


int32_t
syslog_iterator_init (uint32_t idx, uint16_t severity, SYSLOG_ITERATOR_T *it)
{
    int32_t res ;

    if (!_syslog_started) {
        return E_UNEXP ;       
    }
    if (idx >= LOG_NLOG2_MAX || !_log_nlog2[idx]) {
        return E_PARM ;
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog2_iterator_init (_log_nlog2[idx], ~((1 << severity)-1), it) ;
    os_mutex_unlock (&_syslog_mutex) ;

    return res ;
}

int32_t
syslog_iterator_prev (SYSLOG_ITERATOR_T *it)
{
    int32_t res ;

    if (!_syslog_started) {
        return E_UNEXP ;       
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog2_iterator_prev (it) ;
    os_mutex_unlock (&_syslog_mutex) ;

    return res ;
}

int32_t
syslog_iterator_next (SYSLOG_ITERATOR_T *it)
{
    int32_t res ;

    if (!_syslog_started) {
        return E_UNEXP ;       
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog2_iterator_prev (it) ;
    os_mutex_unlock (&_syslog_mutex) ;

    return res ;

}

int32_t
syslog_iterator_read (SYSLOG_ITERATOR_T *it, QORAAL_LOG_MSG_T *msg, uint32_t len)
{
    int32_t res ;

    if (!_syslog_started) {
        return E_UNEXP ;       
    }

    os_mutex_lock (&_syslog_mutex) ;
    res = nlog2_iterator_read (it, (char*)msg, len) ;
    os_mutex_unlock (&_syslog_mutex) ;

    return res ;

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
    if (!_syslog_started) {
        return 0 ;       
    }
    if (idx >= LOG_NLOG2_MAX || !_log_nlog2[idx]) {
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

