/*
 * Regression tests for syslog on nlog3.
 *
 * These drive the syslog API against a test-owned NOR simulation:
 *
 *   - writes AND the erased state behave like NOR (bits only go 1 -> 0)
 *   - power can be cut at the n-th write from now; the failing write can land
 *     part of its bytes, and every write and erase after it is dropped until
 *     the test "reboots"
 *   - the log region can be pre-filled with nlog2 data, the format syslog used
 *     before, to check what the first boot after the change does
 *
 * Build:  cmake -DBUILD_FLASH_TESTS=ON ..   ->  ./build/test/syslogtest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

#include "qoraal/qoraal.h"
#include "qoraal-flash/qoraal.h"
#include "qoraal-flash/syslog.h"
#include "qoraal-flash/nvram/nlog2.h"

/*===========================================================================*/
/* Simulated NOR FLASH with power-cut injection                              */
/*===========================================================================*/

#define TEST_FLASH_SIZE         (16u * 1024u)
#define TEST_SECTOR_SIZE        (1024u)
#define TEST_INFO_SECTORS       4
#define TEST_ASSERT_SECTORS     3
#define TEST_INFO_START         (0u)

static uint8_t  _flash[TEST_FLASH_SIZE] ;
static int32_t  _cut_in = -1 ;          /* writes left before the one that fails */
static uint32_t _cut_lands ;            /* bytes the failing write still lands */
static int      _cut_keeps_power ;      /* only that one write fails */
static int      _power_off ;            /* drop every write and erase once set */

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
    uint32_t lands = len ;
    int32_t res = EOK ;

    if (addr + len > TEST_FLASH_SIZE) return E_PARM ;
    if (_power_off) return EFAIL ;

    if (_cut_in == 0) {
        _cut_in = -1 ;
        _power_off = !_cut_keeps_power ;
        lands = (_cut_lands < len) ? _cut_lands : len ;
        res = EFAIL ;
    } else if (_cut_in > 0) {
        _cut_in-- ;
    }

    for (i = 0; i < lands; i++) {
        _flash[addr + i] &= data[i] ;   /* NOR: bits only clear */
    }
    return res ;
}

static int32_t
test_flash_erase (uint32_t addr_start, uint32_t addr_end)
{
    if (_power_off) return EFAIL ;
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

static uint32_t _now ;

static void *   t_malloc (QORAAL_HEAP heap, size_t size) { (void)heap; return malloc(size) ; }
static void     t_free (QORAAL_HEAP heap, void *mem)     { (void)heap; free(mem) ; }
static void     t_print (const char *s)                  { fputs(s, stdout) ; }
static int32_t  t_getch (uint32_t timeout_ms)            { (void)timeout_ms; return -1 ; }
static void     t_assert (const char *msg)               { printf("ASSERT: %s\n", msg ? msg : "") ; }
static uint32_t t_time (void)                            { return _now ; }
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
/* Test syslog: small sectors so wrapping is cheap                           */
/*===========================================================================*/

SYSLOG_INST_DECL(_sl, TEST_INFO_START,
                TEST_INFO_SECTORS, TEST_SECTOR_SIZE,
                TEST_ASSERT_SECTORS, TEST_SECTOR_SIZE)

#define LOG_MSG_SIZE        (sizeof(QORAAL_LOG_MSG_T) + SYSLOGLOG_MAX_MSG_SIZE)
#define MAX_COLLECT         256

static QORAAL_LOG_MSG_T *   _msg ;
static int                  _started ;

/* filled by sl_collect, newest first */
static uint16_t _ids[MAX_COLLECT] ;
static uint8_t  _sev[MAX_COLLECT] ;
static uint8_t  _fac[MAX_COLLECT] ;
static char     _txt[MAX_COLLECT][SYSLOGLOG_MAX_MSG_SIZE] ;

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

static void
sl_power_restore (void)
{
    _cut_in = -1 ;
    _cut_lands = 0 ;
    _cut_keeps_power = 0 ;
    _power_off = 0 ;
}

/* Restart syslog from what is on FLASH, the way a reboot would. */
static void
sl_reboot (void)
{
    if (_started) {
        syslog_stop () ;
    }
    sl_power_restore () ;
    syslog_init (&_sl) ;
    CHECK (syslog_start () == EOK, "syslog_start failed") ;
    _started = 1 ;
}

static void
sl_fresh (void)
{
    sl_power_restore () ;
    test_flash_erase (0, TEST_FLASH_SIZE - 1) ;
    sl_reboot () ;
}

/* Walks newest to oldest; returns the record count or a negative error. */
static int
sl_collect (uint32_t idx, uint16_t severity)
{
    SYSLOG_ITERATOR_T it ;
    int n = 0 ;
    int32_t res ;

    res = syslog_iterator_init (idx, severity, &it) ;
    if (res == E_EMPTY) return 0 ;
    if (res != EOK) return res ;

    do {
        res = syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) ;
        if (res < 0) return res ;
        if (n < MAX_COLLECT) {
            _ids[n] = _msg->id ;
            _sev[n] = _msg->severity ;
            _fac[n] = _msg->facillity ;
            snprintf (_txt[n], sizeof(_txt[n]), "%s", _msg->msg) ;
        }
        n++ ;
    } while ((res = syslog_iterator_prev (&it)) == EOK) ;

    return (res == E_BOF) ? n : res ;
}

/* Records of this text length that fit one sector. */
static int
sl_per_sector (uint32_t text_len)
{
    uint32_t rec = (uint32_t)(sizeof(NLOG3_RECORD_HEADER_T) + text_len + 7u) & ~7u ;
    return (int)((TEST_SECTOR_SIZE - 24u) / rec) ;   /* 24-byte sector header */
}

static void
sl_vappend (int32_t idx, const char *format, ...)
{
    va_list args ;
    va_start (args, format) ;
    syslog_vappend_fmtstr (idx, 0, SYSLOG_SEVERITY_ERROR, format, args) ;
    va_end (args) ;
}

/* Ids run down by one from the newest, with no gaps. */
static int
sl_ids_consecutive (int n)
{
    int i ;
    for (i = 1; i < n && i < MAX_COLLECT; i++) {
        if (_ids[i] != (uint16_t)(_ids[i-1] - 1)) return 0 ;
    }
    return 1 ;
}

/*===========================================================================*/
/* Tests                                                                     */
/*===========================================================================*/

static void
test_empty_log (void)
{
    SYSLOG_ITERATOR_T it ;

    printf ("  test_empty_log\n") ;
    sl_fresh () ;

    CHECK (syslog_iterator_init (0, SYSLOG_SEVERITY_DEBUG, &it) == E_EMPTY,
            "empty log did not report E_EMPTY") ;
    CHECK (syslog_platform_it_create (0) == 0, "iterator created on empty log") ;
    CHECK (sl_collect (1, SYSLOG_SEVERITY_DEBUG) == 0, "assert log not empty") ;
}

/* Every field written comes back, through both read paths. */
static void
test_round_trip (void)
{
    RTCLIB_DATE_T date = { .date = 0 } ;
    RTCLIB_TIME_T time = { .time = 0 } ;
    QORAAL_LOG_IT_T * pit ;
    int n ;
    int i ;

    printf ("  test_round_trip\n") ;
    sl_fresh () ;

    date.year = 2026 ; date.month = 10 ; date.day = 7 ;
    time.hour = 14 ; time.minute = 35 ; time.second = 9 ;
    _now = rtc_mktime (date, time) ;

    syslog_append (0, 11, SYSLOG_SEVERITY_ERROR, "first") ;
    syslog_append_fmtstr (0, 12, SYSLOG_SEVERITY_WARNING, "second %d", 2) ;
    syslog_append (0, 13, SYSLOG_SEVERITY_ASSERT, "third") ;
    _now = 0 ;

    n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n == 3, "%d records, expected 3", n) ;
    CHECK (strcmp (_txt[0], "third") == 0, "newest is '%s'", _txt[0]) ;
    CHECK (strcmp (_txt[1], "second 2") == 0, "middle is '%s'", _txt[1]) ;
    CHECK (strcmp (_txt[2], "first") == 0, "oldest is '%s'", _txt[2]) ;
    CHECK (_ids[2] == 0 && sl_ids_consecutive (n), "ids %u %u %u",
            _ids[0], _ids[1], _ids[2]) ;
    CHECK (_sev[0] == SYSLOG_SEVERITY_ASSERT && _sev[1] == SYSLOG_SEVERITY_WARNING &&
            _sev[2] == SYSLOG_SEVERITY_ERROR, "severities %u %u %u",
            _sev[0], _sev[1], _sev[2]) ;
    CHECK (_fac[0] == 13 && _fac[1] == 12 && _fac[2] == 11, "facilities %u %u %u",
            _fac[0], _fac[1], _fac[2]) ;

    /* the shell and web pages read through the platform iterator */
    pit = syslog_platform_it_create (0) ;
    CHECK (pit != 0, "platform iterator not created") ;
    if (pit) {
        int32_t res = pit->get (pit, _msg, LOG_MSG_SIZE) ;
        CHECK (res == (int32_t)(sizeof(QORAAL_LOG_MSG_T) + 5),
                "get returned %d", res) ;
        CHECK (_msg->len == 5 && strcmp (_msg->msg, "third") == 0,
                "get gave '%s' len %u", _msg->msg, _msg->len) ;
        CHECK (_msg->date.year == 2026 && _msg->date.month == 10 &&
                _msg->date.day == 7, "date %u-%u-%u",
                _msg->date.year, _msg->date.month, _msg->date.day) ;
        CHECK (_msg->time.hour == 14 && _msg->time.minute == 35 &&
                _msg->time.second == 9, "time %u:%u:%u",
                _msg->time.hour, _msg->time.minute, _msg->time.second) ;
        for (i = 0; pit->prev (pit) == EOK; i++) ;
        CHECK (i == 2, "platform prev stepped %d times, expected 2", i) ;
        CHECK (pit->get (pit, _msg, LOG_MSG_SIZE) > 0 &&
                strcmp (_msg->msg, "first") == 0,
                "after E_BOF the iterator sits on '%s'", _msg->msg) ;
        syslog_platform_it_destroy (pit) ;
    }

    /* the two logs are separate */
    syslog_append (1, 1, SYSLOG_SEVERITY_ASSERT, "assert only") ;
    CHECK (sl_collect (1, SYSLOG_SEVERITY_DEBUG) == 1 &&
            strcmp (_txt[0], "assert only") == 0 && _ids[0] == 0,
            "assert log content wrong") ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 3, "info log changed") ;
}

/* nlog3 has no record counter; ids must carry on from FLASH. */
static void
test_ids_continue_after_reboot (void)
{
    int i ;
    int n ;

    printf ("  test_ids_continue_after_reboot\n") ;
    sl_fresh () ;

    for (i = 0; i < 5; i++) {
        syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "boot1 %d", i) ;
    }
    sl_reboot () ;
    for (i = 0; i < 3; i++) {
        syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "boot2 %d", i) ;
    }

    n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n == 8, "%d records, expected 8", n) ;
    CHECK (_ids[0] == 7 && sl_ids_consecutive (n), "newest id %u", _ids[0]) ;
    CHECK (strcmp (_txt[0], "boot2 2") == 0 && strcmp (_txt[7], "boot1 0") == 0,
            "content '%s' .. '%s'", _txt[0], _txt[7]) ;
}

/* syslog_iterator_next used to call prev. Both stop in place at the ends. */
static void
test_next_and_prev (void)
{
    SYSLOG_ITERATOR_T it ;
    int i ;

    printf ("  test_next_and_prev\n") ;
    sl_fresh () ;

    for (i = 0; i < 6; i++) {
        syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "r%d", i) ;
    }

    CHECK (syslog_iterator_init (0, SYSLOG_SEVERITY_DEBUG, &it) == EOK, "init failed") ;
    CHECK (syslog_iterator_next (&it) == E_EOF, "next past newest not E_EOF") ;
    CHECK (syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) > 0 &&
            strcmp (_msg->msg, "r5") == 0, "after E_EOF sits on '%s'", _msg->msg) ;

    for (i = 0; i < 5; i++) {
        CHECK (syslog_iterator_prev (&it) == EOK, "prev %d failed", i) ;
    }
    CHECK (syslog_iterator_prev (&it) == E_BOF, "prev past oldest not E_BOF") ;
    CHECK (syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) > 0 &&
            strcmp (_msg->msg, "r0") == 0, "after E_BOF sits on '%s'", _msg->msg) ;

    for (i = 1; i < 6; i++) {
        char want[8] ;
        snprintf (want, sizeof(want), "r%d", i) ;
        CHECK (syslog_iterator_next (&it) == EOK, "next %d failed", i) ;
        CHECK (syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) > 0 &&
                strcmp (_msg->msg, want) == 0,
                "next gave '%s', expected '%s'", _msg->msg, want) ;
    }
}

static void
test_severity_filter (void)
{
    static const uint16_t sev[] = {
        SYSLOG_SEVERITY_ERROR, SYSLOG_SEVERITY_DEBUG, SYSLOG_SEVERITY_WARNING,
        SYSLOG_SEVERITY_INFO, SYSLOG_SEVERITY_ASSERT, SYSLOG_SEVERITY_REPORT
    } ;
    SYSLOG_ITERATOR_T it ;
    uint32_t i ;
    int n ;

    printf ("  test_severity_filter\n") ;
    sl_fresh () ;

    for (i = 0; i < sizeof(sev)/sizeof(sev[0]); i++) {
        syslog_append_fmtstr (0, 0, sev[i], "s%u", i) ;
    }

    n = sl_collect (0, SYSLOG_SEVERITY_WARNING) ;
    CHECK (n == 3, "%d records at WARNING or worse, expected 3", n) ;
    CHECK (strcmp (_txt[0], "s4") == 0 && strcmp (_txt[1], "s2") == 0 &&
            strcmp (_txt[2], "s0") == 0, "got '%s' '%s' '%s'",
            _txt[0], _txt[1], _txt[2]) ;

    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 6, "DEBUG does not see all") ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_NEVER) == 0, "NEVER saw records") ;

    /* next skips the non-matching newest record and stops on the last match */
    CHECK (syslog_iterator_init (0, SYSLOG_SEVERITY_ERROR, &it) == EOK, "init failed") ;
    CHECK (syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) > 0 &&
            strcmp (_msg->msg, "s4") == 0, "newest match '%s'", _msg->msg) ;
    CHECK (syslog_iterator_prev (&it) == EOK, "prev to s0 failed") ;
    CHECK (syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) > 0 &&
            strcmp (_msg->msg, "s0") == 0, "prev gave '%s'", _msg->msg) ;
    CHECK (syslog_iterator_next (&it) == EOK, "next to s4 failed") ;
    CHECK (syslog_iterator_next (&it) == E_EOF, "next past last match not E_EOF") ;
    CHECK (syslog_iterator_read (&it, _msg, LOG_MSG_SIZE) > 0 &&
            strcmp (_msg->msg, "s4") == 0, "after E_EOF sits on '%s'", _msg->msg) ;
}

/* Long messages used to over-read the static buffer; they now clip at 199. */
static void
test_message_limits (void)
{
    char longmsg[300] ;
    char half[151] ;
    SYSLOG_ITERATOR_T it ;
    int32_t res ;
    int n ;

    printf ("  test_message_limits\n") ;
    sl_fresh () ;

    memset (longmsg, 'L', sizeof(longmsg) - 1) ;
    longmsg[sizeof(longmsg) - 1] = '\0' ;
    memset (half, 'F', sizeof(half) - 1) ;
    half[sizeof(half) - 1] = '\0' ;

    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, longmsg) ;
    syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "%s%s", half, half) ;
    longmsg[199] = '\0' ;
    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, longmsg) ;
    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "") ;

    n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n == 4, "%d records, expected 4", n) ;
    CHECK (strlen (_txt[0]) == 0, "empty message came back as '%s'", _txt[0]) ;
    CHECK (strlen (_txt[1]) == 199 && strncmp (_txt[1], longmsg, 199) == 0,
            "199-char message came back %u long", (unsigned)strlen (_txt[1])) ;
    CHECK (strlen (_txt[2]) == 199 && _txt[2][0] == 'F' && _txt[2][198] == 'F',
            "300-char format came back %u long", (unsigned)strlen (_txt[2])) ;
    CHECK (strlen (_txt[3]) == 199 && _txt[3][198] == 'L',
            "299-char append came back %u long", (unsigned)strlen (_txt[3])) ;

    /* what is stored, read through a buffer bigger than any message */
    {
        static const int stored[] = { 0, 199, 199, 199 } ;
        uint32_t big_size = sizeof(QORAAL_LOG_MSG_T) + 512 ;
        QORAAL_LOG_MSG_T * big = malloc (big_size) ;
        int i = 0 ;

        CHECK (syslog_iterator_init (0, SYSLOG_SEVERITY_DEBUG, &it) == EOK, "init failed") ;
        do {
            res = syslog_iterator_read (&it, big, big_size) ;
            CHECK (res == (int32_t)(sizeof(QORAAL_LOG_MSG_T)) + stored[i],
                    "record %d stored %d characters, expected %d", i,
                    res - (int32_t)sizeof(QORAAL_LOG_MSG_T), stored[i]) ;
            i++ ;
        } while ((i < 4) && (syslog_iterator_prev (&it) == EOK)) ;
        CHECK (i == 4, "walked %d records", i) ;
        free (big) ;
    }

    /* a short read buffer truncates and still terminates */
    CHECK (syslog_iterator_init (0, SYSLOG_SEVERITY_DEBUG, &it) == EOK, "init failed") ;
    CHECK (syslog_iterator_prev (&it) == EOK, "prev failed") ;
    memset (_msg, 0x55, LOG_MSG_SIZE) ;
    res = syslog_iterator_read (&it, _msg, sizeof(QORAAL_LOG_MSG_T) + 10) ;
    CHECK (res == (int32_t)(sizeof(QORAAL_LOG_MSG_T) + 9), "short read returned %d", res) ;
    CHECK (_msg->len == 9 && _msg->msg[9] == '\0' && strlen (_msg->msg) == 9,
            "short read len %u", _msg->len) ;
    CHECK (syslog_iterator_read (&it, _msg, sizeof(QORAAL_LOG_MSG_T) + 1) == E_PARM,
            "no room for text was not E_PARM") ;
}

static void
test_bad_index (void)
{
    SYSLOG_ITERATOR_T it ;

    printf ("  test_bad_index\n") ;
    sl_fresh () ;

    syslog_append (2, 0, SYSLOG_SEVERITY_ERROR, "nowhere") ;
    syslog_append_fmtstr (5, 0, SYSLOG_SEVERITY_ERROR, "nowhere") ;
    sl_vappend (-1, "nowhere") ;

    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 0, "log 0 got a record") ;
    CHECK (sl_collect (1, SYSLOG_SEVERITY_DEBUG) == 0, "log 1 got a record") ;
    CHECK (syslog_iterator_init (2, SYSLOG_SEVERITY_DEBUG, &it) == E_PARM,
            "iterator on log 2 not E_PARM") ;
    CHECK (syslog_platform_it_create (2) == 0, "platform iterator on log 2") ;
}

/* Wrapping drops the oldest sector and keeps the newest, ids unbroken. */
static void
test_wrap (void)
{
    const int total = 600 ;
    int per0 = sl_per_sector (17) ;     /* "wrap message 0599" */
    int per1 = sl_per_sector (19) ;     /* "assert message 0599" */
    int n0 ;
    int n1 ;
    int n ;
    int i ;

    printf ("  test_wrap\n") ;
    sl_fresh () ;

    for (i = 0; i < total; i++) {
        syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "wrap message %04d", i) ;
        syslog_append_fmtstr (1, 0, SYSLOG_SEVERITY_ASSERT, "assert message %04d", i) ;
    }

    n0 = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n0 > 0 && _ids[0] == total - 1 && sl_ids_consecutive (n0),
            "info log: %d records, newest id %u", n0, _ids[0]) ;
    CHECK (strcmp (_txt[0], "wrap message 0599") == 0, "newest is '%s'", _txt[0]) ;

    n1 = sl_collect (1, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n1 > 0 && _ids[0] == total - 1 && sl_ids_consecutive (n1),
            "assert log: %d records, newest id %u", n1, _ids[0]) ;

    /*
     * One sector is the one being written and one is kept erased, so a log of
     * N sectors holds N-2 full sectors plus the partial current one.
     */
    CHECK (n0 > (TEST_INFO_SECTORS - 2) * per0 && n0 <= (TEST_INFO_SECTORS - 1) * per0,
            "info log kept %d records (%d per sector)", n0, per0) ;
    CHECK (n1 > (TEST_ASSERT_SECTORS - 2) * per1 && n1 <= (TEST_ASSERT_SECTORS - 1) * per1,
            "assert log kept %d records (%d per sector)", n1, per1) ;
    printf ("    kept %d of %d (info, %d sectors), %d of %d (assert, %d sectors), "
            "%d records per sector\n",
            n0, total, TEST_INFO_SECTORS, n1, total, TEST_ASSERT_SECTORS, per0) ;

    sl_reboot () ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == n0, "info log changed across reboot") ;
    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "after reboot") ;
    n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n > 0 && _ids[0] == total && strcmp (_txt[0], "after reboot") == 0 &&
            sl_ids_consecutive (n),
            "after reboot newest id %u '%s'", _ids[0], _txt[0]) ;
}

/*
 * Cut power at each write of an append (PENDING state, header, payload,
 * VALID state), with part of the failing write landing. The half-written
 * record must not show, earlier records must survive, and the next append
 * after the reboot gets the next id.
 */
static void
test_power_cut (void)
{
    static const uint32_t lands[] = { 0, 2, 9 } ;
    uint32_t step ;
    uint32_t l ;
    int cuts = 0 ;
    int n ;

    printf ("  test_power_cut\n") ;

    for (step = 0; step < 4; step++) {
        for (l = 0; l < sizeof(lands)/sizeof(lands[0]); l++) {
            int i ;

            sl_fresh () ;
            for (i = 0; i < 5; i++) {
                syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "keep %d", i) ;
            }

            /*
             * The state words only clear their low byte, so a VALID write
             * that lands any of its bytes has committed the record.
             */
            int committed = (step == 3) && (lands[l] > 0) ;
            int want = committed ? 6 : 5 ;
            const char *newest = committed ? "torn" : "keep 4" ;

            _cut_in = (int32_t)step ;
            _cut_lands = lands[l] ;
            syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "torn") ;
            CHECK (_power_off, "step %u: the append did not reach the cut", step) ;

            sl_reboot () ;
            n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
            CHECK (n == want && strcmp (_txt[0], newest) == 0 && _ids[0] == want - 1 &&
                    sl_ids_consecutive (n),
                    "step %u lands %u: %d records, newest '%s' id %u",
                    step, lands[l], n, n > 0 ? _txt[0] : "", n > 0 ? _ids[0] : 0) ;

            syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "after") ;
            n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
            CHECK (n == want + 1 && strcmp (_txt[0], "after") == 0 && _ids[0] == want &&
                    strcmp (_txt[1], newest) == 0 && sl_ids_consecutive (n),
                    "step %u lands %u: after reboot %d records, newest '%s' id %u",
                    step, lands[l], n, n > 0 ? _txt[0] : "", n > 0 ? _ids[0] : 0) ;
        }
    }

    /*
     * A cut while the append moves into the next sector: end marker, sector
     * header, staging the sector after, then the record itself.
     */
    for (step = 0; step < 10; step++) {
        int per = sl_per_sector (17) ;  /* "fill message 0000" */
        int i ;

        sl_fresh () ;
        for (i = 0; i < per; i++) {
            syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "fill message %04d", i) ;
        }

        _cut_in = (int32_t)step ;
        _cut_lands = 3 ;
        syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "crossing message") ;
        cuts += _power_off ;

        sl_reboot () ;
        n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
        CHECK ((n == per || n == per + 1) && sl_ids_consecutive (n) &&
                strncmp (_txt[n - per], "fill message", 12) == 0,
                "cut %u at sector change: %d records (had %d)", step, n, per) ;

        syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "after") ;
        i = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
        CHECK (i == n + 1 && strcmp (_txt[0], "after") == 0 && _ids[0] == n &&
                sl_ids_consecutive (i),
                "cut %u at sector change: after reboot %d records, newest '%s' id %u",
                step, i, _txt[0], _ids[0]) ;
    }
    printf ("    a sector-crossing append makes %d writes\n", cuts) ;
    CHECK (cuts >= 6, "only %d of the sector-crossing cuts landed", cuts) ;
}

/*
 * A write error with the power still on: that append is lost, the log keeps
 * going without a reboot, and the lost line does not use up an id.
 */
static void
test_write_error_without_reboot (void)
{
    uint32_t step ;
    int n ;
    int i ;

    printf ("  test_write_error_without_reboot\n") ;

    for (step = 0; step < 4; step++) {
        sl_fresh () ;
        for (i = 0; i < 5; i++) {
            syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "keep %d", i) ;
        }

        _cut_in = (int32_t)step ;
        _cut_keeps_power = 1 ;
        syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "lost") ;
        CHECK (_cut_in == -1, "step %u: the append did not reach the failure", step) ;

        syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "after") ;
        n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
        CHECK (n == 6 && strcmp (_txt[0], "after") == 0 && _ids[0] == 5 &&
                strcmp (_txt[1], "keep 4") == 0 && sl_ids_consecutive (n),
                "step %u: %d records, newest '%s' id %u", step, n, _txt[0], _ids[0]) ;

        sl_reboot () ;
        CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 6 && _ids[0] == 5,
                "step %u: changed across reboot", step) ;
    }
}

static void
test_reset (void)
{
    printf ("  test_reset\n") ;
    sl_fresh () ;

    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "gone") ;
    syslog_append (1, 0, SYSLOG_SEVERITY_ERROR, "stays") ;
    CHECK (syslog_reset (0) == EOK, "reset failed") ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 0, "log 0 not empty after reset") ;
    CHECK (sl_collect (1, SYSLOG_SEVERITY_DEBUG) == 1, "reset touched log 1") ;

    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "fresh") ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 1 && strcmp (_txt[0], "fresh") == 0,
            "append after reset failed") ;
    CHECK (syslog_reset (2) != EOK, "reset of log 2 succeeded") ;
}

/* First boot after the change: the region still holds nlog2 records. */
static void
test_upgrade_from_nlog2 (void)
{
    NLOG2_T old_info ;
    int i ;

    printf ("  test_upgrade_from_nlog2\n") ;
    memset (&old_info, 0, sizeof(old_info)) ;
    old_info.startaddr = TEST_INFO_START ;
    old_info.sectorcount = TEST_INFO_SECTORS ;
    old_info.sectorsize = TEST_SECTOR_SIZE ;
    if (_started) {
        syslog_stop () ;
        _started = 0 ;
    }
    sl_power_restore () ;
    test_flash_erase (0, TEST_FLASH_SIZE - 1) ;

    CHECK (nlog2_init (&old_info) == EOK, "nlog2 init failed") ;
    for (i = 0; i < 40; i++) {
        char line[32] ;
        snprintf (line, sizeof(line), "old nlog2 line %d", i) ;
        nlog2_append (&old_info, 1, line, (uint32_t)strlen (line) + 1) ;
    }

    sl_reboot () ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 0, "nlog2 data showed up as records") ;
    syslog_append (0, 0, SYSLOG_SEVERITY_ERROR, "first nlog3 line") ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 1 && _ids[0] == 0 &&
            strcmp (_txt[0], "first nlog3 line") == 0, "append after upgrade failed") ;
}

/* Rolling back to nlog2 firmware: nlog2 must reset, not misread nlog3 data. */
static void
test_rollback_to_nlog2 (void)
{
    NLOG2_T old_info ;
    NLOG2_ITERATOR_T it ;
    char line[32] ;
    int i ;

    printf ("  test_rollback_to_nlog2\n") ;
    memset (&old_info, 0, sizeof(old_info)) ;
    old_info.startaddr = TEST_INFO_START ;
    old_info.sectorcount = TEST_INFO_SECTORS ;
    old_info.sectorsize = TEST_SECTOR_SIZE ;
    sl_fresh () ;
    for (i = 0; i < 40; i++) {
        syslog_append_fmtstr (0, 0, SYSLOG_SEVERITY_ERROR, "nlog3 line %d", i) ;
    }
    syslog_stop () ;
    _started = 0 ;

    CHECK (nlog2_init (&old_info) == EOK, "nlog2 init over nlog3 data failed") ;
    CHECK (nlog2_append (&old_info, 1, "rolled back", 12) == EOK, "nlog2 append failed") ;
    CHECK (nlog2_iterator_init (&old_info, 0, &it) == EOK, "nlog2 iterator failed") ;
    memset (line, 0, sizeof(line)) ;
    CHECK (nlog2_iterator_read (&it, line, sizeof(line)) == 12 &&
            strcmp (line, "rolled back") == 0, "nlog2 newest is '%s'", line) ;
    CHECK (nlog2_iterator_prev (&it) != EOK, "nlog2 found older records") ;

    /* and forward again */
    sl_reboot () ;
    CHECK (sl_collect (0, SYSLOG_SEVERITY_DEBUG) == 0, "nlog2 data showed up as records") ;
}

/* The logger callback runs on many threads; half use each append path. */
#define THREADS             6
#define PER_THREAD          400

static void *
append_thread (void *arg)
{
    int t = (int)(intptr_t)arg ;
    char line[32] ;
    int i ;
    for (i = 0; i < PER_THREAD; i++) {
        if (t & 1) {
            syslog_append_fmtstr (0, (uint16_t)t, SYSLOG_SEVERITY_ERROR, "thread %d line %03d", t, i) ;
        } else {
            snprintf (line, sizeof(line), "thread %d line %03d", t, i) ;
            syslog_append (0, (uint16_t)t, SYSLOG_SEVERITY_ERROR, line) ;
        }
    }
    return 0 ;
}

static void
test_concurrent_appends (void)
{
    pthread_t th[THREADS] ;
    int last[THREADS] ;
    int ordered = 1 ;
    int n ;
    int i ;

    printf ("  test_concurrent_appends\n") ;
    sl_fresh () ;

    for (i = 0; i < THREADS; i++) {
        pthread_create (&th[i], 0, append_thread, (void *)(intptr_t)i) ;
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join (th[i], 0) ;
    }

    n = sl_collect (0, SYSLOG_SEVERITY_DEBUG) ;
    CHECK (n > 0 && _ids[0] == THREADS * PER_THREAD - 1 && sl_ids_consecutive (n),
            "%d records, newest id %u", n, _ids[0]) ;

    /* each thread's lines are intact and in order, newest first */
    for (i = 0; i < THREADS; i++) last[i] = PER_THREAD ;
    for (i = 0; i < n && i < MAX_COLLECT; i++) {
        int t ;
        int line ;
        if ((sscanf (_txt[i], "thread %d line %d", &t, &line) != 2) ||
                (t < 0) || (t >= THREADS) || (_fac[i] != t) || (line >= last[t])) {
            ordered = 0 ;
            break ;
        }
        last[t] = line ;
    }
    CHECK (ordered, "record %d '%s' out of order or mangled", i, i < n ? _txt[i] : "") ;
}

/*===========================================================================*/

int
main (void)
{
    printf ("syslog (nlog3) regression tests\n") ;
#if defined(CFG_NLOG3_PAYLOAD_CRC)
    printf ("  CFG_NLOG3_PAYLOAD_CRC on\n") ;
#endif

    qoraal_init_default (&_test_cfg, 0) ;
    qoraal_flash_instance_init (&_test_flash_cfg) ;
    _msg = malloc (LOG_MSG_SIZE) ;

    test_empty_log () ;
    test_round_trip () ;
    test_ids_continue_after_reboot () ;
    test_next_and_prev () ;
    test_severity_filter () ;
    test_message_limits () ;
    test_bad_index () ;
    test_wrap () ;
    test_power_cut () ;
    test_write_error_without_reboot () ;
    test_reset () ;
    test_upgrade_from_nlog2 () ;
    test_rollback_to_nlog2 () ;
    test_concurrent_appends () ;

    if (_started) {
        syslog_stop () ;
    }
    free (_msg) ;

    printf ("\n%d checks, %d failure(s)\n", _checks, _failures) ;
    return _failures ? 1 : 0 ;
}
