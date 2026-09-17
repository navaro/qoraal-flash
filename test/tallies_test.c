/*
 * Regression tests for the modular tallies layer.
 *
 * Same approach as nvol3_test.c: a test-owned NOR simulation, so the store can
 * be torn down and brought back up as often as a test needs, and so the stored
 * bytes can be inspected directly.
 *
 * Build:  cmake -DBUILD_FLASH_TESTS=ON ..   ->  ./build/test/talliestest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "qoraal/qoraal.h"
#include "qoraal-flash/qoraal.h"
#include "qoraal-flash/tallies.h"

/*===========================================================================*/
/* Simulated NOR FLASH                                                       */
/*===========================================================================*/

#define TEST_FLASH_SIZE         (64u * 1024u)
#define TEST_SECTOR_SIZE        (8u * 1024u)
#define TEST_SECTOR1            (0u)

static uint8_t  _flash[TEST_FLASH_SIZE] ;

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

static uint32_t _test_now = 1757700000u ;   /* fixed clock the tests advance */

static void *   t_malloc (QORAAL_HEAP heap, size_t size) { (void)heap; return malloc(size) ; }
static void     t_free (QORAAL_HEAP heap, void *mem)     { (void)heap; free(mem) ; }
static void     t_print (const char *s)                  { (void)s ; }
static int32_t  t_getch (uint32_t timeout_ms)            { (void)timeout_ms; return -1 ; }
static void     t_assert (const char *msg)               { printf("ASSERT: %s\n", msg ? msg : "") ; }
static uint32_t t_time (void)                            { return _test_now ; }
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
/* Volume and blocks                                                         */
/*===========================================================================*/

TALLIES_INST_DECL(_tv, TEST_SECTOR1, TEST_SECTOR_SIZE, 31) ;

#define TALLIES_MODULE          sys
#define TALLIES_MODULE_LIST     "tallies_test_sys_lst.h"
#include "qoraal-flash/tallies_module.h"
#define TALLIES_MODULE_DEFINE
#include "qoraal-flash/tallies_module.h"
#undef TALLIES_MODULE
#undef TALLIES_MODULE_LIST

#define TALLIES_MODULE          mod
#define TALLIES_MODULE_LIST     "tallies_test_mod_lst.h"
#include "qoraal-flash/tallies_module.h"
#define TALLIES_MODULE_DEFINE
#include "qoraal-flash/tallies_module.h"
#undef TALLIES_MODULE
#undef TALLIES_MODULE_LIST

/* Same size as sys, different names - used for the id reuse test. */
#define TALLIES_MODULE          alt
#define TALLIES_MODULE_LIST     "tallies_test_alt_lst.h"
#include "qoraal-flash/tallies_module.h"
#define TALLIES_MODULE_DEFINE
#include "qoraal-flash/tallies_module.h"
#undef TALLIES_MODULE
#undef TALLIES_MODULE_LIST

#define SYS_BASE    (0 * TALLIES_PER_MODULE)
#define MOD_BASE    (1 * TALLIES_PER_MODULE)

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

/* Drop everything and start from an erased volume. */
static void
tv_fresh (void)
{
    tallies_unregister (&_sys_tallies) ;
    tallies_unregister (&_mod_tallies) ;
    tallies_unregister (&_alt_tallies) ;
    test_flash_erase (0, TEST_FLASH_SIZE - 1) ;
    _tv.sector = 0 ;
    _tv.next_idx = 0 ;
    _tv.dict = 0 ;
    tallies_init (&_tv) ;
}

/*===========================================================================*/
/* Tests                                                                     */
/*===========================================================================*/

/* Register, count, persist, reload: the values come back. */
static void
test_persist_and_reload (void)
{
    printf ("  test_persist_and_reload\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;

    CHECK (TALLIES_INC(sys, wdt) == EOK, "inc wdt") ;
    CHECK (TALLIES_INC(sys, wdt) == EOK, "inc wdt") ;
    CHECK (TALLIES_ADD(sys, hardfault, 7) == EOK, "add hardfault") ;
    CHECK (TALLIES_GET(sys, wdt) == 2, "wdt %u, expected 2",
            (unsigned)TALLIES_GET(sys, wdt)) ;

    tallies_stop () ;

    /* the values are gone from RAM until the volume is read back */
    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "re-register") ;
    CHECK (tallies_start () == EOK, "restart") ;
    CHECK (TALLIES_GET(sys, wdt) == 2, "wdt %u after reload, expected 2",
            (unsigned)TALLIES_GET(sys, wdt)) ;
    CHECK (TALLIES_GET(sys, hardfault) == 7, "hardfault %u after reload",
            (unsigned)TALLIES_GET(sys, hardfault)) ;
    CHECK (TALLIES_GET(sys, init) == 0, "untouched tallie not zero") ;

    tallies_stop () ;
}

/*
 * Registration has to work either way round, because a module registers at
 * service CTRL_INIT and the store may start before or after that.
 */
static void
test_register_before_and_after_start (void)
{
    printf ("  test_register_before_and_after_start\n") ;
    tv_fresh () ;

    /* before */
    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;
    CHECK (TALLIES_INC(sys, init) == EOK, "inc init") ;

    /* after */
    CHECK (tallies_register (&_mod_tallies, MOD_BASE) == EOK, "register mod") ;
    CHECK (TALLIES_INC(mod, started) == EOK, "inc started") ;
    tallies_persist () ;
    tallies_stop () ;

    /* both survive, and both come back whichever order they register in */
    CHECK (tallies_register (&_mod_tallies, MOD_BASE) == EOK, "re-register mod") ;
    CHECK (tallies_start () == EOK, "restart") ;
    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "re-register sys") ;

    CHECK (TALLIES_GET(sys, init) == 1, "sys.init %u, expected 1",
            (unsigned)TALLIES_GET(sys, init)) ;
    CHECK (TALLIES_GET(mod, started) == 1, "mod.started %u, expected 1",
            (unsigned)TALLIES_GET(mod, started)) ;

    tallies_stop () ;
}

/*
 * The safety net: a block landing on ids that belong to a different set of
 * counters must start at zero, not inherit the stored values.
 */
static void
test_id_reuse_resets (void)
{
    printf ("  test_id_reuse_resets\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;
    CHECK (TALLIES_ADD(sys, init, 11) == EOK, "add init") ;
    CHECK (TALLIES_ADD(sys, wdt, 22) == EOK, "add wdt") ;
    tallies_persist () ;
    tallies_stop () ;

    /* "alt" now occupies the ids that "sys" used to own */
    CHECK (tallies_unregister (&_sys_tallies) == EOK, "unregister sys") ;
    CHECK (tallies_register (&_alt_tallies, SYS_BASE) == EOK, "register alt") ;
    CHECK (tallies_start () == EOK, "restart") ;

    CHECK (TALLIES_GET(alt, alpha) == 0, "alt.alpha inherited %u",
            (unsigned)TALLIES_GET(alt, alpha)) ;
    CHECK (TALLIES_GET(alt, beta) == 0, "alt.beta inherited %u",
            (unsigned)TALLIES_GET(alt, beta)) ;
    CHECK (tallies_mismatched () >= 2, "only %u records dropped, expected >= 2",
            (unsigned)tallies_mismatched ()) ;

    tallies_stop () ;
}

/* Two blocks may not claim overlapping id ranges. */
static void
test_overlapping_base_rejected (void)
{
    printf ("  test_overlapping_base_rejected\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_register (&_alt_tallies, SYS_BASE + 1) != EOK,
            "overlapping base accepted") ;
    CHECK (tallies_register (&_alt_tallies, MOD_BASE) == EOK,
            "non-overlapping base rejected") ;

    /* ids are uint16_t, so a block may not run into the sentinel */
    CHECK (tallies_unregister (&_alt_tallies) == EOK, "unregister alt") ;
    CHECK (tallies_register (&_alt_tallies, TALLIES_BASE_NONE - 1) != EOK,
            "base running past the id space accepted") ;
    CHECK (tallies_register (&_alt_tallies, -1) != EOK, "negative base accepted") ;
}

/*
 * The persist pass claims an entry - clears its dirty flag - before writing it,
 * so a counter raised between two passes is written by the second one and not
 * dropped. The single threaded harness cannot interleave a raise with a write,
 * but it can prove the flag is not simply cleared for the whole pass.
 */
static void
test_dirty_survives_persist (void)
{
    printf ("  test_dirty_survives_persist\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;

    CHECK (TALLIES_ADD(sys, wdt, 1) == EOK, "add wdt") ;
    tallies_persist () ;

    /* raised after the pass that wrote it */
    CHECK (TALLIES_ADD(sys, wdt, 1) == EOK, "add wdt again") ;
    tallies_persist () ;

    tallies_stop () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "re-register") ;
    CHECK (tallies_start () == EOK, "restart") ;
    CHECK (TALLIES_GET(sys, wdt) == 2, "wdt %u after reload, expected 2",
            (unsigned)TALLIES_GET(sys, wdt)) ;

    tallies_stop () ;
}

/* The status snapshot reports the volume the counters actually went to. */
static void
test_status (void)
{
    NVOL3_STATUS_T st ;

    printf ("  test_status\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;

    CHECK (TALLIES_ADD(sys, wdt, 3) == EOK, "add wdt") ;
    CHECK (TALLIES_ADD(sys, init, 1) == EOK, "add init") ;
    tallies_persist () ;

    CHECK (tallies_status_get (&st) == EOK, "status") ;
    CHECK (st.records_used == 2, "records_used %u, expected 2",
            (unsigned)st.records_used) ;
    CHECK (st.records_max > st.records_used, "records_max %u not sane",
            (unsigned)st.records_max) ;
    CHECK (st.record_size >= TALLIES_RECORD_DATA_SIZE,
            "record_size %u smaller than the payload",
            (unsigned)st.record_size) ;
    CHECK (st.sector_size > 0, "sector_size 0") ;
    CHECK (st.hash_size > 0, "hash_size 0") ;
    CHECK (st.hash_used > 0 && st.hash_used <= st.records_used,
            "hash_used %u, expected 1..2", (unsigned)st.hash_used) ;
    CHECK (st.hash_max_chain > 0, "hash_max_chain 0 with records loaded") ;
    CHECK (st.lookup_bytes > 0, "lookup_bytes 0 with records loaded") ;

    CHECK (tallies_status_get (0) != EOK, "null status accepted") ;

    tallies_stop () ;
}

/* Out of range ids are rejected rather than read past the end of the block. */
static void
test_bounds (void)
{
    printf ("  test_bounds\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;

    CHECK (tallies_get_value (&_sys_tallies, 4000) == 0, "no bounds check") ;
    CHECK (tallies_inc (&_sys_tallies, 4000) != EOK, "inc out of range") ;
    CHECK (tallies_set (&_sys_tallies, __sys_tallie_last, 1) != EOK,
            "set at count accepted") ;
    CHECK (tallies_get (&_sys_tallies, 4000, 0) != EOK, "get with no out") ;

    /* an unregistered block is inert, not a crash */
    CHECK (tallies_inc (&_alt_tallies, 0) != EOK, "inc on unregistered block") ;

    tallies_stop () ;
}

/* Reset clears RAM and FLASH, and the zeros survive a reload. */
static void
test_reset (void)
{
    printf ("  test_reset\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (tallies_start () == EOK, "start") ;
    CHECK (TALLIES_ADD(sys, wdt, 42) == EOK, "add wdt") ;
    tallies_persist () ;

    tallies_reset () ;
    CHECK (TALLIES_GET(sys, wdt) == 0, "wdt %u after reset",
            (unsigned)TALLIES_GET(sys, wdt)) ;

    tallies_stop () ;
    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "re-register") ;
    CHECK (tallies_start () == EOK, "restart") ;
    CHECK (TALLIES_GET(sys, wdt) == 0, "wdt %u came back after reset",
            (unsigned)TALLIES_GET(sys, wdt)) ;

    tallies_stop () ;
}

/*
 * A counter has to work before the volume is up - "we ran out of memory" is
 * exactly the event you want a tallie for.
 */
static void
test_counts_before_start (void)
{
    printf ("  test_counts_before_start\n") ;
    tv_fresh () ;

    CHECK (tallies_register (&_sys_tallies, SYS_BASE) == EOK, "register sys") ;
    CHECK (TALLIES_INC(sys, init) == EOK, "inc before start") ;
    CHECK (TALLIES_GET(sys, init) == 1, "init %u before start",
            (unsigned)TALLIES_GET(sys, init)) ;

    /*
     * start() reloads from the volume, which is empty, so the RAM count is
     * replaced by the stored zero. Documented behaviour, asserted so that a
     * change to it is deliberate.
     */
    CHECK (tallies_start () == EOK, "start") ;
    CHECK (TALLIES_GET(sys, init) == 0, "start did not reload from the volume") ;

    tallies_stop () ;
}

/*===========================================================================*/

int
main (void)
{
    printf ("tallies regression tests\n") ;

    qoraal_init_default (&_test_cfg, 0) ;
    qoraal_flash_instance_init (&_test_flash_cfg) ;

    test_persist_and_reload () ;
    test_register_before_and_after_start () ;
    test_id_reuse_resets () ;
    test_overlapping_base_rejected () ;
    test_dirty_survives_persist () ;
    test_status () ;
    test_bounds () ;
    test_reset () ;
    test_counts_before_start () ;

    printf ("\n%d checks, %d failure(s)\n", _checks, _failures) ;
    return _failures ? 1 : 0 ;
}
