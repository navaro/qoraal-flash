/*
    Copyright (C) 2015-2026, Navaro, All Rights Reserved
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
 * @file    nlog3.h
 * @brief   Crash-safe flash record log.
 * @details Defines a generic record descriptor storage shape without
 *          depending on application event headers. Records are committed with
 *          a one-way state transition.
 */

#ifndef __NLOG3_H__
#define __NLOG3_H__

#include <stdint.h>

#include "qoraal-flash/config.h"

/*===========================================================================*/
/* Record metadata storage shape.                                             */
/*===========================================================================*/

#define NLOG3_RECORD_USER_WORDS         8U

#pragma pack(1)
typedef struct NLOG3_RECORD_DESC_S {
    uint32_t user[NLOG3_RECORD_USER_WORDS];
} NLOG3_RECORD_DESC_T;
#pragma pack()

/*===========================================================================*/
/* Log record layout.                                                         */
/*===========================================================================*/

/* STAGED is used by sector headers; record headers commit as PENDING->VALID. */
#define NLOG3_RECORD_STATE_EMPTY        ((uint32_t)0xFFFFFFFFU)
#define NLOG3_RECORD_STATE_PENDING      ((uint32_t)0xFFFFFFFEU)
#define NLOG3_RECORD_STATE_VALID        ((uint32_t)0xFFFFFFFCU)
#define NLOG3_RECORD_STATE_STAGED       ((uint32_t)0xFFFFFFF8U)
#define NLOG3_RECORD_STATE_SECTOR_END   ((uint32_t)0xFFFFFFF0U)
#define NLOG3_RECORD_OFFSET_NONE        UINT32_MAX

#pragma pack(1)
typedef struct NLOG3_RECORD_HEADER_S {
    uint32_t            state;
    uint32_t            magic;
    uint32_t            header_crc;
#if defined(CFG_NLOG3_PAYLOAD_CRC)
    uint32_t            payload_crc;
    uint32_t            padding;
#endif
    uint32_t            payload_size;
    uint32_t            previous_offset;
    uint32_t            previous_sequence;
    NLOG3_RECORD_DESC_T desc;
} NLOG3_RECORD_HEADER_T;
#pragma pack()

typedef struct NLOG3_ITERATOR_S {
    struct NLOG3_S      *plog;
    uint32_t            sector;
    uint32_t            addr;
    uint32_t            sector_sequence;
    NLOG3_RECORD_HEADER_T header;
} NLOG3_ITERATOR_T;

typedef struct NLOG3_S {
    uint32_t            startaddr;
    uint32_t            sectorcount;
    uint32_t            sectorsize;
    uint32_t            current_sector;
    uint32_t            current_sequence;
    uint32_t            staged_sector;
    uint32_t            write_addr;
    uint32_t            last_record_offset;
    uint32_t            last_record_sequence;
    uint8_t             current_closed;
} NLOG3_T;

#define NLOG3_LOG_DATA(start, count, sectorsize)        {start, count, sectorsize, 0U, 0U, 0U, 0U, NLOG3_RECORD_OFFSET_NONE, 0U, 0U}
#define NLOG3_LOG_DECL(name, start, count, sectorsize)  NLOG3_T name = NLOG3_LOG_DATA(start, count, sectorsize)

/*===========================================================================*/
/* External declarations.                                                     */
/*===========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

int32_t  nlog3_init(NLOG3_T *plog);
int32_t  nlog3_reset(NLOG3_T *plog);

int32_t  nlog3_append(NLOG3_T *plog,
                      const NLOG3_RECORD_DESC_T *desc,
                      uint32_t payload_size,
                      const void *payload);

int32_t  nlog3_iterator_init(NLOG3_T *plog,
                             NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_init_oldest(NLOG3_T *plog,
                                    NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_prev(NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_next(NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_desc(const NLOG3_ITERATOR_T *it,
                             NLOG3_RECORD_DESC_T *desc);
int32_t  nlog3_iterator_read(const NLOG3_ITERATOR_T *it,
                             void *payload,
                             int32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __NLOG3_H__ */
