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
 * @brief   Crash-safe flash event log.
 * @details Defines an event descriptor storage shape without depending on
 *          application event headers. Records are committed with a one-way
 *          state transition.
 */

#ifndef __NLOG3_H__
#define __NLOG3_H__

#include <stdint.h>

/*===========================================================================*/
/* Event metadata storage shape.                                              */
/*===========================================================================*/

#pragma pack(1)
typedef struct NLOG3_EVENT_DESC_S {
    uint32_t id;
    uint32_t timestamp_ms;
    uint32_t walltime_sec;

    uint32_t flags;
    int16_t  module;
    uint16_t type;
    uint16_t version;
    uint16_t payload_format;

    uint32_t payload_size;
} NLOG3_EVENT_DESC_T;
#pragma pack()

/*===========================================================================*/
/* Log record layout and filters.                                             */
/*===========================================================================*/

/* STAGED is used by sector headers; record headers commit as PENDING->VALID. */
#define NLOG3_RECORD_STATE_EMPTY        ((uint32_t)0xFFFFFFFFU)
#define NLOG3_RECORD_STATE_PENDING      ((uint32_t)0xFFFFFFFEU)
#define NLOG3_RECORD_STATE_VALID        ((uint32_t)0xFFFFFFFCU)
#define NLOG3_RECORD_STATE_STAGED       ((uint32_t)0xFFFFFFF8U)
#define NLOG3_RECORD_STATE_SECTOR_END   ((uint32_t)0xFFFFFFF0U)

#pragma pack(1)
typedef struct NLOG3_RECORD_HEADER_S {
    uint32_t            state;
    uint32_t            magic;
    uint32_t            total_size;
    uint32_t            header_crc;
    uint32_t            payload_crc;
    NLOG3_EVENT_DESC_T  desc;
} NLOG3_RECORD_HEADER_T;
#pragma pack()

#define NLOG3_FILTER_FLAGS_ANY          ((uint32_t)(1U << 0))
#define NLOG3_FILTER_FLAGS_ALL          ((uint32_t)(1U << 1))
#define NLOG3_FILTER_FLAGS_NONE         ((uint32_t)(1U << 2))
#define NLOG3_FILTER_MODULE             ((uint32_t)(1U << 3))
#define NLOG3_FILTER_TYPE               ((uint32_t)(1U << 4))
#define NLOG3_FILTER_VERSION            ((uint32_t)(1U << 5))
#define NLOG3_FILTER_PAYLOAD_FORMAT     ((uint32_t)(1U << 6))

typedef struct NLOG3_FILTER_S {
    uint32_t mask;
    uint32_t flags_any;
    uint32_t flags_all;
    uint32_t flags_none;
    int16_t  module;
    uint16_t type;
    uint16_t version;
    uint16_t payload_format;
} NLOG3_FILTER_T;

typedef struct NLOG3_ITERATOR_S {
    struct NLOG3_S      *plog;
    NLOG3_FILTER_T      filter;
    uint32_t            sector;
    uint32_t            addr;
    uint32_t            sector_sequence;
    NLOG3_RECORD_HEADER_T header;
} NLOG3_ITERATOR_T;

typedef struct NLOG3_S {
    uint32_t            startaddr;
    uint32_t            sectorcount;
    uint32_t            sectorsize;
    uint32_t            id;
    uint32_t            current_sector;
    uint32_t            current_sequence;
    uint32_t            staged_sector;
    uint32_t            write_addr;
    uint8_t             current_closed;
} NLOG3_T;

#define NLOG3_LOG_DATA(start, count, sectorsize)        {start, count, sectorsize, 0U, 0U, 0U, 0U, 0U, 0U}
#define NLOG3_LOG_DECL(name, start, count, sectorsize)  NLOG3_T name = NLOG3_LOG_DATA(start, count, sectorsize)

/*===========================================================================*/
/* External declarations.                                                     */
/*===========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

int32_t  nlog3_init(NLOG3_T *plog);
int32_t  nlog3_reset(NLOG3_T *plog);
uint32_t nlog3_get_id(NLOG3_T *plog);

int32_t  nlog3_append(NLOG3_T *plog,
                      const NLOG3_EVENT_DESC_T *desc,
                      const void *payload);

int32_t  nlog3_iterator_init(NLOG3_T *plog,
                             const NLOG3_FILTER_T *filter,
                             NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_init_oldest(NLOG3_T *plog,
                                    const NLOG3_FILTER_T *filter,
                                    NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_prev(NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_next(NLOG3_ITERATOR_T *it);
int32_t  nlog3_iterator_desc(const NLOG3_ITERATOR_T *it,
                             NLOG3_EVENT_DESC_T *desc);
int32_t  nlog3_iterator_read(const NLOG3_ITERATOR_T *it,
                             void *payload,
                             int32_t len);
uint32_t nlog3_iterator_id(const NLOG3_ITERATOR_T *it);

#ifdef __cplusplus
}
#endif

#endif /* __NLOG3_H__ */
