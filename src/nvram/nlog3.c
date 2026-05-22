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

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "qoraal-flash/qoraal.h"
#include "qoraal-flash/nvram/nlog3.h"

#define FLASH_READ(address, len, data)   qoraal_flash_read((address), (len), (data))
#define FLASH_WRITE(address, len, data)  qoraal_flash_write((address), (len), (data))
#define FLASH_ERASE(start, end)          qoraal_flash_erase((start), (end))

#define NLOG3_LOG_RECORD_ALIGN           8U
#define NLOG3_SECTOR_VERSION             1U
#define NLOG3_RECORD_MAGIC               0x33474C4EU
#define NLOG3_SECTOR_MAGIC               0x33534C4EU
#define NLOG3_CRC32_POLY                 0xEDB88320U
#define NLOG3_SECTOR_NONE                UINT32_MAX

#define NLOG3_SCAN_RECORD                0U
#define NLOG3_SCAN_EMPTY                 1U
#define NLOG3_SCAN_TERMINAL              2U

#define NLOG3_MIN(a, b)                  (((a) < (b)) ? (a) : (b))
#define NLOG3_STATIC_ASSERT(name, cond)  typedef char name[(cond) ? 1 : -1]

#pragma pack(1)
typedef struct NLOG3_SECTOR_HEADER_S {
    uint32_t state;
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t sector_size;
    uint32_t header_crc;
} NLOG3_SECTOR_HEADER_T;
#pragma pack()

NLOG3_STATIC_ASSERT(nlog3_record_state_is_first,
                    offsetof(NLOG3_RECORD_HEADER_T, state) == 0U);
NLOG3_STATIC_ASSERT(nlog3_sector_state_is_first,
                    offsetof(NLOG3_SECTOR_HEADER_T, state) == 0U);

typedef struct NLOG3_SECTOR_SCAN_S {
    uint32_t end_addr;
    uint32_t terminal;
} NLOG3_SECTOR_SCAN_T;

static uint32_t
nlog3_align_up(uint32_t value)
{
    return (value + (NLOG3_LOG_RECORD_ALIGN - 1U)) &
           ~(NLOG3_LOG_RECORD_ALIGN - 1U);
}

static uint32_t
nlog3_crc32_begin(void)
{
    return 0xFFFFFFFFU;
}

static uint32_t
nlog3_crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t i;

    while (len > 0U) {
        crc ^= *bytes++;
        for (i = 0U; i < 8U; i++) {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc = (crc >> 1U) ^ (NLOG3_CRC32_POLY & mask);
        }
        len--;
    }

    return crc;
}

static uint32_t
nlog3_crc32_finish(uint32_t crc)
{
    return ~crc;
}

static uint32_t
nlog3_crc32(const void *data, uint32_t len)
{
    return nlog3_crc32_finish(nlog3_crc32_update(nlog3_crc32_begin(),
                                                 data,
                                                 len));
}

static uint32_t
nlog3_record_header_crc(const NLOG3_RECORD_HEADER_T *header)
{
    NLOG3_RECORD_HEADER_T tmp;

    tmp = *header;
    /* State is the commit word, so it is deliberately outside the CRC. */
    tmp.state = 0U;
    tmp.header_crc = 0U;

    return nlog3_crc32(&tmp, (uint32_t)sizeof(tmp));
}

static uint32_t
nlog3_sector_header_crc(const NLOG3_SECTOR_HEADER_T *header)
{
    NLOG3_SECTOR_HEADER_T tmp;

    tmp = *header;
    /* State is the commit word, so it is deliberately outside the CRC. */
    tmp.state = 0U;
    tmp.header_crc = 0U;

    return nlog3_crc32(&tmp, (uint32_t)sizeof(tmp));
}

static int32_t
nlog3_seq_after(uint32_t a, uint32_t b)
{
    return ((int32_t)(a - b)) > 0;
}

static int32_t
nlog3_seq_before(uint32_t a, uint32_t b)
{
    return nlog3_seq_after(b, a);
}

static uint32_t
nlog3_sector_addr(const NLOG3_T *plog, uint32_t sector)
{
    return plog->startaddr + (plog->sectorsize * sector);
}

static uint32_t
nlog3_sector_end(const NLOG3_T *plog, uint32_t sector)
{
    return nlog3_sector_addr(plog, sector) + plog->sectorsize;
}

static uint32_t
nlog3_sector_data_start(const NLOG3_T *plog, uint32_t sector)
{
    return nlog3_sector_addr(plog, sector) +
           nlog3_align_up((uint32_t)sizeof(NLOG3_SECTOR_HEADER_T));
}

static uint32_t
nlog3_sector_next(const NLOG3_T *plog, uint32_t sector)
{
    return (sector < (plog->sectorcount - 1U)) ? (sector + 1U) : 0U;
}

static int32_t
nlog3_addr_has_room(uint32_t addr, uint32_t end, uint32_t size)
{
    return (addr <= end) && (size <= (end - addr));
}

static int32_t
nlog3_log_valid(const NLOG3_T *plog)
{
    if (!plog || (plog->sectorcount < 3U) || (plog->sectorsize == 0U)) {
        return 0;
    }

    return plog->sectorsize >
           (nlog3_align_up((uint32_t)sizeof(NLOG3_SECTOR_HEADER_T)) +
            sizeof(uint32_t));
}

static int32_t
nlog3_sector_erase(NLOG3_T *plog, uint32_t sector)
{
    uint32_t start;

    if (!nlog3_log_valid(plog) || (sector >= plog->sectorcount)) {
        return E_PARM;
    }

    start = nlog3_sector_addr(plog, sector);

    return FLASH_ERASE(start, start + plog->sectorsize - 1U);
}

static int32_t
nlog3_write_u32(uint32_t addr, uint32_t value)
{
    return FLASH_WRITE(addr, (uint32_t)sizeof(value), (const uint8_t *)&value);
}

static int32_t
nlog3_write_pending_header(uint32_t addr,
                           const void *header,
                           uint32_t header_size,
                           uint32_t pending_state)
{
    uint32_t state_size = (uint32_t)sizeof(pending_state);
    int32_t res;

    if (!header || (header_size <= state_size)) {
        return E_PARM;
    }

    res = nlog3_write_u32(addr, pending_state);
    if (res != EOK) {
        return res;
    }

    return FLASH_WRITE(addr + state_size,
                       header_size - state_size,
                       ((const uint8_t *)header) + state_size);
}

static int32_t
nlog3_sector_state_is_readable(uint32_t state)
{
    return ((state == NLOG3_RECORD_STATE_VALID) ||
            (state == NLOG3_RECORD_STATE_STAGED));
}

static int32_t
nlog3_sector_header_read(NLOG3_T *plog,
                         uint32_t sector,
                         NLOG3_SECTOR_HEADER_T *header)
{
    int32_t res;

    if (!nlog3_log_valid(plog) || !header || (sector >= plog->sectorcount)) {
        return E_PARM;
    }

    memset(header, 0xFF, sizeof(*header));
    res = FLASH_READ(nlog3_sector_addr(plog, sector),
                     (uint32_t)sizeof(*header),
                     (uint8_t *)header);
    if (res != EOK) {
        return res;
    }

    if (!nlog3_sector_state_is_readable(header->state)) {
        return E_NOTFOUND;
    }
    if ((header->magic != NLOG3_SECTOR_MAGIC) ||
        (header->version != NLOG3_SECTOR_VERSION) ||
        (header->sector_size != plog->sectorsize)) {
        return E_CORRUPT;
    }
    if (header->header_crc != nlog3_sector_header_crc(header)) {
        return E_CORRUPT;
    }

    return EOK;
}

static int32_t
nlog3_sector_write_header(NLOG3_T *plog,
                          uint32_t sector,
                          uint32_t sequence,
                          uint32_t final_state)
{
    NLOG3_SECTOR_HEADER_T header;
    int32_t res;

    header.state = NLOG3_RECORD_STATE_PENDING;
    header.magic = NLOG3_SECTOR_MAGIC;
    header.version = NLOG3_SECTOR_VERSION;
    header.sequence = sequence;
    header.sector_size = plog->sectorsize;
    header.header_crc = nlog3_sector_header_crc(&header);

    res = nlog3_write_pending_header(nlog3_sector_addr(plog, sector),
                                     &header,
                                     (uint32_t)sizeof(header),
                                     header.state);
    if (res != EOK) {
        return res;
    }

    res = nlog3_write_u32(nlog3_sector_addr(plog, sector),
                          final_state);
    if (res != EOK) {
        return res;
    }

    return EOK;
}

static int32_t
nlog3_sector_init(NLOG3_T *plog, uint32_t sector, uint32_t sequence)
{
    int32_t res;

    res = nlog3_sector_erase(plog, sector);
    if (res != EOK) {
        return res;
    }

    res = nlog3_sector_write_header(plog,
                                    sector,
                                    sequence,
                                    NLOG3_RECORD_STATE_VALID);
    if (res != EOK) {
        return res;
    }

    plog->current_sector = sector;
    plog->current_sequence = sequence;
    plog->staged_sector = NLOG3_SECTOR_NONE;
    plog->write_addr = nlog3_sector_data_start(plog, sector);
    plog->current_closed = 0U;

    return EOK;
}

static uint32_t
nlog3_record_alloc_size(uint32_t payload_size)
{
    if (payload_size > (UINT32_MAX -
                        (uint32_t)sizeof(NLOG3_RECORD_HEADER_T) -
                        (NLOG3_LOG_RECORD_ALIGN - 1U))) {
        return 0U;
    }

    return nlog3_align_up((uint32_t)sizeof(NLOG3_RECORD_HEADER_T) +
                          payload_size);
}

static int32_t
nlog3_record_read(NLOG3_T *plog,
                  uint32_t sector,
                  uint32_t addr,
                  NLOG3_RECORD_HEADER_T *header,
                  uint32_t *scan_type)
{
    uint32_t end;
    uint32_t state;
    int32_t res;

    if (!nlog3_log_valid(plog) || !scan_type ||
        (sector >= plog->sectorcount)) {
        return E_PARM;
    }

    end = nlog3_sector_end(plog, sector);
    *scan_type = NLOG3_SCAN_TERMINAL;

    if (!nlog3_addr_has_room(addr, end, (uint32_t)sizeof(state))) {
        return EOK;
    }

    state = NLOG3_RECORD_STATE_EMPTY;
    res = FLASH_READ(addr, (uint32_t)sizeof(state), (uint8_t *)&state);
    if (res != EOK) {
        return res;
    }

    if (state == NLOG3_RECORD_STATE_EMPTY) {
        *scan_type = NLOG3_SCAN_EMPTY;
        return EOK;
    }
    if (state != NLOG3_RECORD_STATE_VALID) {
        *scan_type = NLOG3_SCAN_TERMINAL;
        return EOK;
    }
    if (!header ||
        !nlog3_addr_has_room(addr, end,
                             (uint32_t)sizeof(NLOG3_RECORD_HEADER_T))) {
        *scan_type = NLOG3_SCAN_TERMINAL;
        return EOK;
    }

    memset(header, 0xFF, sizeof(*header));
    res = FLASH_READ(addr,
                     (uint32_t)sizeof(*header),
                     (uint8_t *)header);
    if (res != EOK) {
        return res;
    }

    if ((header->state != NLOG3_RECORD_STATE_VALID) ||
        (header->magic != NLOG3_RECORD_MAGIC) ||
        (header->total_size < sizeof(*header)) ||
        ((header->total_size & (NLOG3_LOG_RECORD_ALIGN - 1U)) != 0U) ||
        !nlog3_addr_has_room(addr, end, header->total_size) ||
        (header->desc.payload_size >
            (header->total_size - (uint32_t)sizeof(*header))) ||
        (header->header_crc != nlog3_record_header_crc(header))) {
        *scan_type = NLOG3_SCAN_TERMINAL;
        return EOK;
    }

    *scan_type = NLOG3_SCAN_RECORD;

    return EOK;
}

static int32_t
nlog3_sector_scan(NLOG3_T *plog,
                  uint32_t sector,
                  NLOG3_SECTOR_SCAN_T *scan)
{
    uint32_t addr;
    uint32_t end;
    uint32_t scan_type;
    NLOG3_RECORD_HEADER_T header;
    int32_t res;

    if (!nlog3_log_valid(plog) || !scan ||
        (sector >= plog->sectorcount)) {
        return E_PARM;
    }

    addr = nlog3_sector_data_start(plog, sector);
    end = nlog3_sector_end(plog, sector);
    scan->terminal = NLOG3_SCAN_TERMINAL;
    scan->end_addr = addr;

    while (addr < end) {
        res = nlog3_record_read(plog, sector, addr, &header, &scan_type);
        if (res != EOK) {
            return res;
        }

        if (scan_type != NLOG3_SCAN_RECORD) {
            scan->terminal = scan_type;
            scan->end_addr = addr;
            return EOK;
        }

        if (header.desc.id >= plog->id) {
            plog->id = header.desc.id + 1U;
        }

        addr += header.total_size;
    }

    scan->terminal = NLOG3_SCAN_TERMINAL;
    scan->end_addr = end;

    return EOK;
}

static int32_t
nlog3_find_newest_sector(NLOG3_T *plog, uint32_t *sector, uint32_t *sequence)
{
    NLOG3_SECTOR_HEADER_T header;
    uint32_t i;
    uint32_t found = 0U;
    int32_t res;

    for (i = 0U; i < plog->sectorcount; i++) {
        res = nlog3_sector_header_read(plog, i, &header);
        if (res == EOK) {
            if (!found || nlog3_seq_after(header.sequence, *sequence)) {
                *sector = i;
                *sequence = header.sequence;
                found = 1U;
            }
        } else if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
            return res;
        }
    }

    return found ? EOK : E_NOTFOUND;
}

static int32_t
nlog3_find_newest_current_sector(NLOG3_T *plog,
                                 uint32_t *sector,
                                 uint32_t *sequence)
{
    NLOG3_SECTOR_HEADER_T header;
    uint32_t i;
    uint32_t found = 0U;
    int32_t res;

    for (i = 0U; i < plog->sectorcount; i++) {
        res = nlog3_sector_header_read(plog, i, &header);
        if (res == EOK) {
            if ((header.state == NLOG3_RECORD_STATE_VALID) &&
                (!found || nlog3_seq_after(header.sequence, *sequence))) {
                *sector = i;
                *sequence = header.sequence;
                found = 1U;
            }
        } else if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
            return res;
        }
    }

    return found ? EOK : E_NOTFOUND;
}

static int32_t
nlog3_find_oldest_sector(NLOG3_T *plog, uint32_t *sector, uint32_t *sequence)
{
    NLOG3_SECTOR_HEADER_T header;
    uint32_t i;
    uint32_t found = 0U;
    int32_t res;

    for (i = 0U; i < plog->sectorcount; i++) {
        res = nlog3_sector_header_read(plog, i, &header);
        if (res == EOK) {
            if (!found || nlog3_seq_before(header.sequence, *sequence)) {
                *sector = i;
                *sequence = header.sequence;
                found = 1U;
            }
        } else if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
            return res;
        }
    }

    return found ? EOK : E_NOTFOUND;
}

static int32_t
nlog3_find_sector_before(NLOG3_T *plog,
                         uint32_t sequence,
                         uint32_t *sector,
                         uint32_t *previous_sequence)
{
    NLOG3_SECTOR_HEADER_T header;
    uint32_t i;
    uint32_t found = 0U;
    int32_t res;

    for (i = 0U; i < plog->sectorcount; i++) {
        res = nlog3_sector_header_read(plog, i, &header);
        if (res == EOK) {
            if (nlog3_seq_before(header.sequence, sequence) &&
                (!found || nlog3_seq_after(header.sequence,
                                           *previous_sequence))) {
                *sector = i;
                *previous_sequence = header.sequence;
                found = 1U;
            }
        } else if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
            return res;
        }
    }

    return found ? EOK : E_NOTFOUND;
}

static int32_t
nlog3_find_sector_after(NLOG3_T *plog,
                        uint32_t sequence,
                        uint32_t *sector,
                        uint32_t *next_sequence)
{
    NLOG3_SECTOR_HEADER_T header;
    uint32_t i;
    uint32_t found = 0U;
    int32_t res;

    for (i = 0U; i < plog->sectorcount; i++) {
        res = nlog3_sector_header_read(plog, i, &header);
        if (res == EOK) {
            if (nlog3_seq_after(header.sequence, sequence) &&
                (!found || nlog3_seq_before(header.sequence,
                                            *next_sequence))) {
                *sector = i;
                *next_sequence = header.sequence;
                found = 1U;
            }
        } else if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
            return res;
        }
    }

    return found ? EOK : E_NOTFOUND;
}

static int32_t
nlog3_filter_match(const NLOG3_FILTER_T *filter,
                   const NLOG3_EVENT_DESC_T *desc)
{
    if (!filter || (filter->mask == 0U)) {
        return 1;
    }
    if ((filter->mask & NLOG3_FILTER_FLAGS_ANY) &&
        ((desc->flags & filter->flags_any) == 0U)) {
        return 0;
    }
    if ((filter->mask & NLOG3_FILTER_FLAGS_ALL) &&
        ((desc->flags & filter->flags_all) != filter->flags_all)) {
        return 0;
    }
    if ((filter->mask & NLOG3_FILTER_FLAGS_NONE) &&
        ((desc->flags & filter->flags_none) != 0U)) {
        return 0;
    }
    if ((filter->mask & NLOG3_FILTER_MODULE) &&
        (desc->module != filter->module)) {
        return 0;
    }
    if ((filter->mask & NLOG3_FILTER_TYPE) &&
        (desc->type != filter->type)) {
        return 0;
    }
    if ((filter->mask & NLOG3_FILTER_VERSION) &&
        (desc->version != filter->version)) {
        return 0;
    }
    if ((filter->mask & NLOG3_FILTER_PAYLOAD_FORMAT) &&
        (desc->payload_format != filter->payload_format)) {
        return 0;
    }

    return 1;
}

static void
nlog3_iterator_set(NLOG3_ITERATOR_T *it,
                   NLOG3_T *plog,
                   const NLOG3_FILTER_T *filter,
                   uint32_t sector,
                   uint32_t sequence,
                   uint32_t addr,
                   const NLOG3_RECORD_HEADER_T *header)
{
    memset(it, 0, sizeof(*it));
    it->plog = plog;
    if (filter) {
        it->filter = *filter;
    }
    it->sector = sector;
    it->sector_sequence = sequence;
    it->addr = addr;
    it->header = *header;
}

static int32_t
nlog3_sector_find_first(NLOG3_T *plog,
                        uint32_t sector,
                        uint32_t sequence,
                        uint32_t start_addr,
                        const NLOG3_FILTER_T *filter,
                        NLOG3_ITERATOR_T *it)
{
    uint32_t addr;
    uint32_t end;
    uint32_t scan_type;
    NLOG3_RECORD_HEADER_T header;
    int32_t res;

    addr = nlog3_sector_data_start(plog, sector);
    if (start_addr > addr) {
        addr = start_addr;
    }
    end = nlog3_sector_end(plog, sector);

    while (addr < end) {
        res = nlog3_record_read(plog, sector, addr, &header, &scan_type);
        if (res != EOK) {
            return res;
        }
        if (scan_type != NLOG3_SCAN_RECORD) {
            return E_NOTFOUND;
        }
        if (nlog3_filter_match(filter, &header.desc)) {
            nlog3_iterator_set(it, plog, filter, sector, sequence,
                               addr, &header);
            return EOK;
        }
        addr += header.total_size;
    }

    return E_NOTFOUND;
}

static int32_t
nlog3_sector_find_last_before(NLOG3_T *plog,
                              uint32_t sector,
                              uint32_t sequence,
                              uint32_t before_addr,
                              const NLOG3_FILTER_T *filter,
                              NLOG3_ITERATOR_T *it)
{
    uint32_t addr;
    uint32_t end;
    uint32_t scan_type;
    uint32_t found = 0U;
    NLOG3_RECORD_HEADER_T header;
    NLOG3_ITERATOR_T last;
    int32_t res;

    addr = nlog3_sector_data_start(plog, sector);
    end = nlog3_sector_end(plog, sector);
    if ((before_addr == 0U) || (before_addr > end)) {
        before_addr = end;
    }

    while (addr < before_addr) {
        res = nlog3_record_read(plog, sector, addr, &header, &scan_type);
        if (res != EOK) {
            return res;
        }
        if (scan_type != NLOG3_SCAN_RECORD) {
            break;
        }
        if (nlog3_filter_match(filter, &header.desc)) {
            nlog3_iterator_set(&last, plog, filter, sector, sequence,
                               addr, &header);
            found = 1U;
        }
        addr += header.total_size;
    }

    if (!found) {
        return E_NOTFOUND;
    }

    *it = last;

    return EOK;
}

static int32_t
nlog3_write_end_marker(NLOG3_T *plog)
{
    uint32_t end;

    if (plog->current_closed) {
        return EOK;
    }

    end = nlog3_sector_end(plog, plog->current_sector);
    if (nlog3_addr_has_room(plog->write_addr, end,
                            (uint32_t)sizeof(uint32_t))) {
        return nlog3_write_u32(plog->write_addr,
                               NLOG3_RECORD_STATE_SECTOR_END);
    }

    return EOK;
}

static int32_t
nlog3_stage_sector(NLOG3_T *plog, uint32_t sector)
{
    NLOG3_SECTOR_HEADER_T header;
    int32_t res;

    if (sector >= plog->sectorcount) {
        return E_PARM;
    }
    if (sector == plog->current_sector) {
        return E_NOTFOUND;
    }

    res = nlog3_sector_header_read(plog, sector, &header);
    if (res == EOK) {
        if (header.state == NLOG3_RECORD_STATE_STAGED) {
            plog->staged_sector = sector;
            return EOK;
        }

        res = nlog3_write_u32(nlog3_sector_addr(plog, sector),
                              NLOG3_RECORD_STATE_STAGED);
        if (res != EOK) {
            return res;
        }

        plog->staged_sector = sector;
        return EOK;
    }

    if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
        return res;
    }

    res = nlog3_sector_erase(plog, sector);
    if (res != EOK) {
        return res;
    }

    res = nlog3_sector_write_header(plog,
                                    sector,
                                    plog->current_sequence + 1U,
                                    NLOG3_RECORD_STATE_STAGED);
    if (res != EOK) {
        return res;
    }

    plog->staged_sector = sector;

    return EOK;
}

static int32_t
nlog3_ensure_staged_sector(NLOG3_T *plog)
{
    return nlog3_stage_sector(plog,
                              nlog3_sector_next(plog,
                                                plog->current_sector));
}

static int32_t
nlog3_advance_sector(NLOG3_T *plog)
{
    uint32_t next_sector;
    uint32_t next_sequence;
    int32_t res;

    if ((plog->staged_sector >= plog->sectorcount) ||
        (plog->staged_sector == plog->current_sector)) {
        res = nlog3_ensure_staged_sector(plog);
        if (res != EOK) {
            return res;
        }
    }

    next_sector = plog->staged_sector;
    next_sequence = plog->current_sequence + 1U;

    res = nlog3_sector_init(plog, next_sector, next_sequence);
    if (res != EOK) {
        return res;
    }

    return nlog3_ensure_staged_sector(plog);
}

int32_t
nlog3_init(NLOG3_T *plog)
{
    NLOG3_SECTOR_SCAN_T scan;
    uint32_t newest_sector = 0U;
    uint32_t newest_sequence = 0U;
    uint32_t i;
    int32_t res;

    if (!nlog3_log_valid(plog)) {
        return E_PARM;
    }

    plog->id = 0U;
    plog->current_sector = 0U;
    plog->current_sequence = 0U;
    plog->staged_sector = NLOG3_SECTOR_NONE;
    plog->write_addr = 0U;
    plog->current_closed = 1U;

    res = nlog3_find_newest_current_sector(plog,
                                           &newest_sector,
                                           &newest_sequence);
    if (res == E_NOTFOUND) {
        return nlog3_reset(plog);
    }
    if (res != EOK) {
        return res;
    }

    for (i = 0U; i < plog->sectorcount; i++) {
        NLOG3_SECTOR_HEADER_T header;
        res = nlog3_sector_header_read(plog, i, &header);
        if (res == EOK) {
            res = nlog3_sector_scan(plog, i, &scan);
            if (res != EOK) {
                return res;
            }
        } else if ((res != E_NOTFOUND) && (res != E_CORRUPT)) {
            return res;
        }
    }

    res = nlog3_sector_scan(plog, newest_sector, &scan);
    if (res != EOK) {
        return res;
    }

    plog->current_sector = newest_sector;
    plog->current_sequence = newest_sequence;
    plog->write_addr = scan.end_addr;
    plog->current_closed = (scan.terminal == NLOG3_SCAN_EMPTY) ? 0U : 1U;

    return nlog3_ensure_staged_sector(plog);
}

int32_t
nlog3_reset(NLOG3_T *plog)
{
    uint32_t i;
    int32_t res;

    if (!nlog3_log_valid(plog)) {
        return E_PARM;
    }

    for (i = 2U; i < plog->sectorcount; i++) {
        res = nlog3_sector_erase(plog, i);
        if (res != EOK) {
            return res;
        }
    }

    plog->id = 0U;

    res = nlog3_sector_init(plog, 0U, 0U);
    if (res != EOK) {
        return res;
    }

    return nlog3_ensure_staged_sector(plog);
}

uint32_t
nlog3_get_id(NLOG3_T *plog)
{
    return plog ? plog->id : 0U;
}

int32_t
nlog3_append(NLOG3_T *plog,
             const NLOG3_EVENT_DESC_T *desc,
             const void *payload)
{
    NLOG3_RECORD_HEADER_T header;
    uint32_t record_size;
    uint32_t sector_end;
    uint32_t payload_crc;
    int32_t res;

    if (!nlog3_log_valid(plog) || !desc ||
        ((desc->payload_size != 0U) && !payload)) {
        return E_PARM;
    }

    if (plog->write_addr == 0U) {
        return E_NOTRDY;
    }

    record_size = nlog3_record_alloc_size(desc->payload_size);
    if (record_size == 0U) {
        return E_PARM;
    }

    sector_end = nlog3_sector_end(plog, plog->current_sector);
    if (record_size > (sector_end -
                       nlog3_sector_data_start(plog, plog->current_sector))) {
        return E_PARM;
    }

    if (plog->current_closed ||
        !nlog3_addr_has_room(plog->write_addr, sector_end, record_size)) {
        if (!plog->current_closed) {
            res = nlog3_write_end_marker(plog);
            if (res != EOK) {
                plog->current_closed = 1U;
                return res;
            }
            plog->current_closed = 1U;
        }

        res = nlog3_advance_sector(plog);
        if (res != EOK) {
            return res;
        }

        sector_end = nlog3_sector_end(plog, plog->current_sector);
    }

    if (!nlog3_addr_has_room(plog->write_addr, sector_end, record_size)) {
        return E_PARM;
    }

    payload_crc = (desc->payload_size == 0U) ?
        nlog3_crc32(NULL, 0U) :
        nlog3_crc32(payload, desc->payload_size);

    header.state = NLOG3_RECORD_STATE_PENDING;
    header.magic = NLOG3_RECORD_MAGIC;
    header.total_size = record_size;
    header.header_crc = 0U;
    header.payload_crc = payload_crc;
    header.desc = *desc;
    header.header_crc = nlog3_record_header_crc(&header);

    res = nlog3_write_pending_header(plog->write_addr,
                                     &header,
                                     (uint32_t)sizeof(header),
                                     header.state);
    if (res != EOK) {
        plog->current_closed = 1U;
        return res;
    }

    if (desc->payload_size != 0U) {
        res = FLASH_WRITE(plog->write_addr +
                          (uint32_t)sizeof(NLOG3_RECORD_HEADER_T),
                          desc->payload_size,
                          (const uint8_t *)payload);
        if (res != EOK) {
            plog->current_closed = 1U;
            return res;
        }
    }

    res = nlog3_write_u32(plog->write_addr, NLOG3_RECORD_STATE_VALID);
    if (res != EOK) {
        plog->current_closed = 1U;
        return res;
    }

    plog->write_addr += record_size;
    plog->current_closed = 0U;
    if (desc->id >= plog->id) {
        plog->id = desc->id + 1U;
    }

    return EOK;
}

int32_t
nlog3_iterator_init(NLOG3_T *plog,
                    const NLOG3_FILTER_T *filter,
                    NLOG3_ITERATOR_T *it)
{
    uint32_t sector = 0U;
    uint32_t sequence = 0U;
    uint32_t count;
    int32_t res;

    if (!nlog3_log_valid(plog) || !it) {
        return E_PARM;
    }

    memset(it, 0, sizeof(*it));

    res = nlog3_find_newest_sector(plog, &sector, &sequence);
    if (res != EOK) {
        return (res == E_NOTFOUND) ? E_EMPTY : res;
    }

    for (count = 0U; count < plog->sectorcount; count++) {
        res = nlog3_sector_find_last_before(plog,
                                            sector,
                                            sequence,
                                            0U,
                                            filter,
                                            it);
        if (res == EOK) {
            return EOK;
        }
        if (res != E_NOTFOUND) {
            return res;
        }
        res = nlog3_find_sector_before(plog, sequence, &sector, &sequence);
        if (res != EOK) {
            return E_EMPTY;
        }
    }

    return E_EMPTY;
}

int32_t
nlog3_iterator_init_oldest(NLOG3_T *plog,
                           const NLOG3_FILTER_T *filter,
                           NLOG3_ITERATOR_T *it)
{
    uint32_t sector = 0U;
    uint32_t sequence = 0U;
    uint32_t count;
    int32_t res;

    if (!nlog3_log_valid(plog) || !it) {
        return E_PARM;
    }

    memset(it, 0, sizeof(*it));

    res = nlog3_find_oldest_sector(plog, &sector, &sequence);
    if (res != EOK) {
        return (res == E_NOTFOUND) ? E_EMPTY : res;
    }

    for (count = 0U; count < plog->sectorcount; count++) {
        res = nlog3_sector_find_first(plog,
                                      sector,
                                      sequence,
                                      0U,
                                      filter,
                                      it);
        if (res == EOK) {
            return EOK;
        }
        if (res != E_NOTFOUND) {
            return res;
        }
        res = nlog3_find_sector_after(plog, sequence, &sector, &sequence);
        if (res != EOK) {
            return E_EMPTY;
        }
    }

    return E_EMPTY;
}

int32_t
nlog3_iterator_prev(NLOG3_ITERATOR_T *it)
{
    NLOG3_FILTER_T filter;
    NLOG3_ITERATOR_T next_it;
    NLOG3_T *plog;
    uint32_t sector;
    uint32_t sequence;
    uint32_t count;
    int32_t res;

    if (!it || !it->plog ||
        (it->header.state != NLOG3_RECORD_STATE_VALID)) {
        return E_PARM;
    }

    plog = it->plog;
    filter = it->filter;

    res = nlog3_sector_find_last_before(plog,
                                        it->sector,
                                        it->sector_sequence,
                                        it->addr,
                                        &filter,
                                        &next_it);
    if (res == EOK) {
        *it = next_it;
        return EOK;
    }
    if (res != E_NOTFOUND) {
        return res;
    }

    sector = it->sector;
    sequence = it->sector_sequence;
    for (count = 0U; count < plog->sectorcount; count++) {
        res = nlog3_find_sector_before(plog, sequence, &sector, &sequence);
        if (res != EOK) {
            return E_BOF;
        }
        res = nlog3_sector_find_last_before(plog,
                                            sector,
                                            sequence,
                                            0U,
                                            &filter,
                                            &next_it);
        if (res == EOK) {
            *it = next_it;
            return EOK;
        }
        if (res != E_NOTFOUND) {
            return res;
        }
    }

    return E_BOF;
}

int32_t
nlog3_iterator_next(NLOG3_ITERATOR_T *it)
{
    NLOG3_FILTER_T filter;
    NLOG3_ITERATOR_T next_it;
    NLOG3_T *plog;
    uint32_t sector;
    uint32_t sequence;
    uint32_t start_addr;
    uint32_t count;
    int32_t res;

    if (!it || !it->plog ||
        (it->header.state != NLOG3_RECORD_STATE_VALID)) {
        return E_PARM;
    }

    plog = it->plog;
    filter = it->filter;
    start_addr = it->addr + it->header.total_size;

    res = nlog3_sector_find_first(plog,
                                  it->sector,
                                  it->sector_sequence,
                                  start_addr,
                                  &filter,
                                  &next_it);
    if (res == EOK) {
        *it = next_it;
        return EOK;
    }
    if (res != E_NOTFOUND) {
        return res;
    }

    sector = it->sector;
    sequence = it->sector_sequence;
    for (count = 0U; count < plog->sectorcount; count++) {
        res = nlog3_find_sector_after(plog, sequence, &sector, &sequence);
        if (res != EOK) {
            return E_EOF;
        }
        res = nlog3_sector_find_first(plog,
                                      sector,
                                      sequence,
                                      0U,
                                      &filter,
                                      &next_it);
        if (res == EOK) {
            *it = next_it;
            return EOK;
        }
        if (res != E_NOTFOUND) {
            return res;
        }
    }

    return E_EOF;
}

int32_t
nlog3_iterator_desc(const NLOG3_ITERATOR_T *it,
                    NLOG3_EVENT_DESC_T *desc)
{
    if (!it || !desc || (it->header.state != NLOG3_RECORD_STATE_VALID)) {
        return E_PARM;
    }

    *desc = it->header.desc;

    return EOK;
}

int32_t
nlog3_iterator_read(const NLOG3_ITERATOR_T *it,
                    void *payload,
                    int32_t len)
{
    uint8_t buf[32];
    uint32_t payload_size;
    uint32_t payload_addr;
    uint32_t copy_len;
    uint32_t copied;
    uint32_t offset;
    uint32_t crc;
    int32_t res;

    if (!it || !it->plog ||
        (it->header.state != NLOG3_RECORD_STATE_VALID) ||
        (len < 0)) {
        return E_PARM;
    }

    payload_size = it->header.desc.payload_size;
    if (len == 0) {
        return (int32_t)payload_size;
    }
    if (!payload) {
        return E_PARM;
    }

    payload_addr = it->addr + (uint32_t)sizeof(NLOG3_RECORD_HEADER_T);
    copy_len = NLOG3_MIN((uint32_t)len, payload_size);
    copied = 0U;
    offset = 0U;
    crc = nlog3_crc32_begin();

    while (offset < payload_size) {
        uint32_t chunk = NLOG3_MIN((uint32_t)sizeof(buf),
                                   payload_size - offset);
        res = FLASH_READ(payload_addr + offset, chunk, buf);
        if (res != EOK) {
            return res;
        }

        crc = nlog3_crc32_update(crc, buf, chunk);

        if (copied < copy_len) {
            uint32_t to_copy = NLOG3_MIN(chunk, copy_len - copied);
            memcpy((uint8_t *)payload + copied, buf, to_copy);
            copied += to_copy;
        }

        offset += chunk;
    }

    crc = nlog3_crc32_finish(crc);
    if (crc != it->header.payload_crc) {
        return E_CORRUPT;
    }

    return (int32_t)copied;
}

uint32_t
nlog3_iterator_id(const NLOG3_ITERATOR_T *it)
{
    return it ? it->header.desc.id : 0U;
}
