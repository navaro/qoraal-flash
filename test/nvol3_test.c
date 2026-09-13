/*
 * Regression tests for nvol3.
 *
 * These drive nvol3 directly against a test-owned NOR simulation so that the
 * failure modes can be injected deliberately:
 *
 *   - writes AND the erased state behave like NOR (bits only go 1 -> 0)
 *   - a write can be made to fail on demand, to exercise the recovery paths
 *   - the backing buffer is writable by the test, so records can be corrupted
 *     exactly the way a bad FLASH read would corrupt them
 *
 * Build:  cmake -DBUILD_FLASH_TESTS=ON ..   ->  ./build/test/nvol3test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "qoraal/qoraal.h"
#include "qoraal-flash/qoraal.h"
#include "qoraal-flash/nvram/nvol3.h"

/*===========================================================================*/
/* Simulated NOR FLASH with fault injection                                  */
/*===========================================================================*/

#define TEST_FLASH_SIZE         (64u * 1024u)
#define TEST_SECTOR_SIZE        (4u * 1024u)
#define TEST_SECTOR1            (0u)
#define TEST_SECTOR2            (TEST_SECTOR_SIZE)

static uint8_t  _flash[TEST_FLASH_SIZE] ;
static int32_t  _fail_write_at = -1 ;   /* address whose next write fails */
static uint32_t _write_count ;

static int32_t
test_flash_read (uint32_t addr, uint32_t len, uint8_t *data)
{
    if (addr + len > TEST_FLASH_SIZE) return E_PARM ;
    memcpy (data, _flash + addr, len) ;
    return EOK ;
}

static int32_t
test_flash_write (uint32_t addr, uint32_t len, const uint8_t *data)
{
    uint32_t i ;
    if (addr + len > TEST_FLASH_SIZE) return E_PARM ;

    if ((_fail_write_at >= 0) && ((uint32_t)_fail_write_at == addr)) {
        _fail_write_at = -1 ;           /* one-shot */
        return EFAIL ;                  /* nothing is written */
    }

    _write_count++ ;
    for (i = 0; i < len; i++) {
        _flash[addr + i] &= data[i] ;   /* NOR: bits only clear */
    }
    return EOK ;
}

static int32_t
test_flash_erase (uint32_t addr_start, uint32_t addr_end)
{
    if (addr_end >= TEST_FLASH_SIZE) addr_end = TEST_FLASH_SIZE - 1 ;
    if (addr_end < addr_start) return E_PARM ;
    memset (_flash + addr_start, 0xFF, addr_end - addr_start + 1) ;
    return EOK ;
}

static const QORAAL_FLASH_CFG_T _test_flash_cfg = {
    .flash_read  = test_flash_read,
    .flash_write = test_flash_write,
    .flash_erase = test_flash_erase,
} ;

/*===========================================================================*/
/* Minimal qoraal platform                                                   */
/*===========================================================================*/

static void *   t_malloc (QORAAL_HEAP heap, size_t size) { (void)heap; return malloc(size) ; }
static void     t_free (QORAAL_HEAP heap, void *mem)     { (void)heap; free(mem) ; }
static void     t_print (const char *s)                  { fputs(s, stdout) ; }
static int32_t  t_getch (uint32_t timeout_ms)            { (void)timeout_ms; return -1 ; }
static void     t_assert (const char *msg)               { printf("ASSERT: %s\n", msg ? msg : "") ; }
static uint32_t t_time (void)                            { return 0 ; }
static uint32_t t_rand (void)                            { return 4 ; }
static uint32_t t_wdt_kick (void)                        { return 0 ; }

static const QORAAL_CFG_T _test_cfg = {
    .malloc        = t_malloc,
    .free          = t_free,
    .print         = t_print,
    .getch         = t_getch,
    .debug_assert  = t_assert,
    .current_time  = t_time,
    .rand          = t_rand,
    .wdt_kick      = t_wdt_kick,
} ;

/*===========================================================================*/
/* Test volume: 4-byte uint keys, 16 bytes of data, fully cached in RAM      */
/*===========================================================================*/

#define TEST_DATA_SIZE      16
#define TEST_HASH_SIZE      17

NVOL3_UINT_INSTANCE_DECL(_tv,
                TEST_SECTOR1,
                TEST_SECTOR2,
                TEST_SECTOR_SIZE,
                TEST_DATA_SIZE,
                TEST_DATA_SIZE,         /* local_size == data_size */
                TEST_HASH_SIZE,
                0,
                NVOL3_SECTOR_VERSION) ;

#pragma pack(1)
typedef struct TEST_RECORD_S {
    NVOL3_RECORD_HEAD_T head ;
    uint32_t            key ;
    uint8_t             data[TEST_DATA_SIZE] ;
} TEST_RECORD_T ;
#pragma pack()

static int _failures ;
static int _checks ;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        _checks++ ;                                                     \
        if (!(cond)) {                                                  \
            _failures++ ;                                               \
            printf ("    FAIL %s:%d  ", __func__, __LINE__) ;           \
            printf (__VA_ARGS__) ;                                      \
            printf ("\n") ;                                             \
        }                                                               \
    } while (0)

static uint32_t
tv_max_records (void)
{
    return (TEST_SECTOR_SIZE - NVOL3_PAGE_SIZE) / _tv.config->record_size ;
}

static int32_t
tv_set (uint32_t key, uint8_t fill)
{
    TEST_RECORD_T rec ;
    memset (&rec, 0, sizeof(rec)) ;
    rec.key = key ;
    memset (rec.data, fill, sizeof(rec.data)) ;
    return nvol3_record_set (&_tv, (NVOL3_RECORD_T*)&rec,
                    sizeof(uint32_t) + TEST_DATA_SIZE) ;
}

static int32_t
tv_get (uint32_t key, uint8_t *fill_out)
{
    TEST_RECORD_T rec ;
    int32_t res ;
    memset (&rec, 0, sizeof(rec)) ;
    rec.key = key ;
    res = nvol3_record_get (&_tv, (NVOL3_RECORD_T*)&rec) ;
    if (res > 0 && fill_out) *fill_out = rec.data[0] ;
    return res ;
}

static void
tv_fresh (void)
{
    test_flash_erase (0, TEST_FLASH_SIZE - 1) ;
    _fail_write_at = -1 ;
    nvol3_unload (&_tv) ;
    _tv.sector = 0 ;
    _tv.next_idx = 0 ;
}

/*===========================================================================*/
/* Tests                                                                     */
/*===========================================================================*/

/* Sanity: set / get / reload round trip. */
static void
test_basic (void)
{
    uint8_t fill = 0 ;
    uint32_t i ;

    printf ("  test_basic\n") ;
    tv_fresh () ;
    CHECK (nvol3_load (&_tv) == EOK, "load failed") ;

    for (i = 0; i < 8; i++) {
        CHECK (tv_set (i, (uint8_t)(0xA0 | i)) == EOK, "set %u failed", i) ;
    }
    for (i = 0; i < 8; i++) {
        CHECK (tv_get (i, &fill) > 0 && fill == (uint8_t)(0xA0 | i),
                "get %u wrong", i) ;
    }

    /* update in place, then reload from FLASH */
    CHECK (tv_set (3, 0x5A) == EOK, "update failed") ;
    CHECK (nvol3_load (&_tv) == EOK, "reload failed") ;
    CHECK (tv_get (3, &fill) > 0 && fill == 0x5A, "update did not persist") ;
    CHECK (tv_get (7, &fill) > 0 && fill == 0xA7, "neighbour corrupted") ;
}

/*
 * B3: swap_sectors() branched on an uninitialised `status` when the
 * dictionary was empty, because the copy loop never ran.
 *
 * Reachable by deleting every record and then continuing to write until the
 * sector fills.
 */
static void
test_swap_with_empty_dictionary (void)
{
    TEST_RECORD_T rec ;
    uint32_t i ;
    uint32_t limit = tv_max_records () * 2 ;

    printf ("  test_swap_with_empty_dictionary\n") ;
    tv_fresh () ;
    CHECK (nvol3_load (&_tv) == EOK, "load failed") ;

    for (i = 0; i < limit; i++) {
        CHECK (tv_set (i, 0x11) == EOK, "set %u failed", i) ;
        memset (&rec, 0, sizeof(rec)) ;
        rec.key = i ;
        CHECK (nvol3_record_delete (&_tv, (NVOL3_RECORD_T*)&rec) == EOK,
                "delete %u failed", i) ;
    }

    /* survived the swap, and the volume is still usable afterwards */
    CHECK (tv_set (0xBEEF, 0x22) == EOK, "set after empty swap failed") ;
    CHECK (nvol3_load (&_tv) == EOK, "reload after empty swap failed") ;
    CHECK (tv_get (0xBEEF, 0) > 0, "record lost across empty swap") ;
}

/*
 * B1 / M2: a hole in the middle of a sector used to truncate the volume,
 * because construct_lookup_table() stopped at the first erased slot.
 *
 * Punch the hole directly: erase one record slot back to 0xFF, the way a
 * write that never landed would leave it.
 */
static void
test_hole_does_not_truncate (void)
{
    uint32_t i ;
    uint32_t offset ;
    uint32_t rsize = _tv.config->record_size ;

    printf ("  test_hole_does_not_truncate\n") ;
    tv_fresh () ;
    CHECK (nvol3_load (&_tv) == EOK, "load failed") ;

    for (i = 0; i < 10; i++) {
        CHECK (tv_set (i, (uint8_t)(0x40 | i)) == EOK, "set %u failed", i) ;
    }

    /* slot 4 never got written */
    offset = _tv.sector + NVOL3_PAGE_SIZE + (rsize * 4) ;
    memset (_flash + offset, 0xFF, rsize) ;

    CHECK (nvol3_load (&_tv) == EOK, "reload failed") ;

    /* the hole costs exactly one record, not every record after it */
    for (i = 0; i < 10; i++) {
        int32_t res = tv_get (i, 0) ;
        if (i == 4) {
            CHECK (res <= 0, "record 4 should be gone") ;
        } else {
            CHECK (res > 0, "record %u lost after the hole", i) ;
        }
    }
    CHECK (_tv.inuse == 9, "inuse %u, expected 9", _tv.inuse) ;

    /* next_idx must still point past the last occupied slot */
    CHECK (_tv.next_idx == 10, "next_idx %u, expected 10",
            (uint32_t)_tv.next_idx) ;

    /* and a following write must not land on top of a live record */
    CHECK (tv_set (99, 0x77) == EOK, "set after hole failed") ;
    CHECK (nvol3_load (&_tv) == EOK, "reload failed") ;
    CHECK (tv_get (9, 0) > 0, "write after hole clobbered record 9") ;
    CHECK (tv_get (99, 0) > 0, "record 99 lost") ;
}

/*
 * B4: a record whose head.length is smaller than key_size underflowed
 * (length - key_size) into the uint16_t entry->length in
 * insert_lookup_table().
 *
 * Forge one with a valid checksum, which is what makes it slip past
 * variable_record_valid() - the checksum is only a byte sum.
 */
static void
test_short_length_record (void)
{
    uint32_t offset ;
    NVOL3_RECORD_HEAD_T head ;
    uint16_t sum = 0 ;
    uint16_t i ;
    uint16_t bogus_len = 2 ;            /* < key_size (4) */

    printf ("  test_short_length_record\n") ;
    tv_fresh () ;
    CHECK (nvol3_load (&_tv) == EOK, "load failed") ;

    CHECK (tv_set (1, 0x31) == EOK, "set 1 failed") ;
    CHECK (tv_set (2, 0x32) == EOK, "set 2 failed") ;

    /* forge slot 0 with a too-short length and a matching checksum */
    offset = _tv.sector + NVOL3_PAGE_SIZE ;
    memcpy (&head, _flash + offset, sizeof(head)) ;
    head.length = bogus_len ;
    for (i = 0; i < bogus_len; i++) {
        sum += _flash[offset + sizeof(head) + i] ;
    }
    head.checksum = (uint16_t)(0x10000 - sum) ;
    memcpy (_flash + offset, &head, sizeof(head)) ;

    /* must not crash, and must reject the record rather than trust it */
    CHECK (nvol3_load (&_tv) == EOK, "reload failed") ;
    CHECK (tv_get (2, 0) > 0, "good record lost") ;
    CHECK (_tv.inuse <= 1, "corrupt record was accepted (inuse %u)",
            _tv.inuse) ;
}

/*
 * A failed write during recovery must cost one record, not the tail of the
 * volume. Injects a write failure inside move_sector().
 */
static void
test_write_failure_during_move (void)
{
    uint32_t i ;
    uint32_t survived = 0 ;

    printf ("  test_write_failure_during_move\n") ;
    tv_fresh () ;
    CHECK (nvol3_load (&_tv) == EOK, "load failed") ;

    for (i = 0; i < 10; i++) {
        CHECK (tv_set (i, (uint8_t)(0x60 | i)) == EOK, "set %u failed", i) ;
    }

    /* fail the 3rd record written into the destination sector */
    _fail_write_at = TEST_SECTOR2 + NVOL3_PAGE_SIZE +
                        (_tv.config->record_size * 2) ;
    CHECK (nvol3_repair (&_tv) == EOK, "repair failed") ;
    CHECK (nvol3_load (&_tv) == EOK, "reload failed") ;

    for (i = 0; i < 10; i++) {
        if (tv_get (i, 0) > 0) survived++ ;
    }
    CHECK (survived >= 9, "%u of 10 records survived a single failed write",
            survived) ;
}

/*===========================================================================*/

int
main (void)
{
    printf ("nvol3 regression tests\n") ;

    qoraal_init_default (&_test_cfg, 0) ;
    qoraal_flash_instance_init (&_test_flash_cfg) ;

    test_basic () ;
    test_swap_with_empty_dictionary () ;
    test_hole_does_not_truncate () ;
    test_short_length_record () ;
    test_write_failure_during_move () ;

    nvol3_unload (&_tv) ;

    printf ("\n%d checks, %d failure(s)\n", _checks, _failures) ;
    return _failures ? 1 : 0 ;
}
