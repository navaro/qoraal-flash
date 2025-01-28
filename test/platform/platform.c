#include <stdio.h>
#include <stdlib.h>
#include "platform.h"
#include "qoraal/common/rtclib.h"
#include "qoraal/common/dictionary.h"
#include "qoraal-flash/nvram/nvol3.h"
#include "qoraal-flash/registry.h"
#include "qoraal-flash/syslog.h"

#define PLATFORM_FLASH_SIZE     (1024*1024*10)


static SVC_SERVICES_T _platform_complete_service_id = SVC_SERVICES_INVALID ;
static uint8_t        _platform_flash[PLATFORM_FLASH_SIZE]  ;


REGISTRY_INSTANCE_DECL(_system_registry, \
        0, 
        64*1024, // total size is 64K * 2
        24, 
        128, 
        101)

SYSLOG_LOG_DECL(_system_syslog, 64*1024*2, 5, 8*1024, 3, 4*1024)



int32_t         
platform_init ()
{
    platform_flash_erase (0, PLATFORM_FLASH_SIZE-1) ;
    registry_init () ;
    syslog_init () ;
    return 0 ;
}

int32_t         
platform_start ()
{
    os_thread_sleep (100) ;
    registry_start (&_system_registry) ;
    syslog_start (&_system_syslog) ;
    return 0 ;
}


void *      
platform_malloc (QORAAL_HEAP heap, size_t size)
{
    return malloc (size) ;
}
void        

platform_free (QORAAL_HEAP heap, void *mem)
{
    free (mem) ;
}

void
platform_print (const char *format)
{
    printf ("%s", format) ;
}

void
platform_assert (const char *format)
{
    printf ("%s", format) ;
    abort () ;
}

uint32_t    
platform_current_time (void)
{
    return os_sys_timestamp () / 1000 ;
}

uint32_t 
platform_wdt_kick (void)
{
    return 20 ;
}

int32_t
platform_flash_erase (uint32_t addr_start, uint32_t addr_end)
{
    if (addr_end < addr_start) return E_PARM ;
    if (addr_start >= PLATFORM_FLASH_SIZE) return E_PARM ;
    if (addr_end >= PLATFORM_FLASH_SIZE) {
        addr_end = PLATFORM_FLASH_SIZE - 1 ;
    }
    memset ((void*)(_platform_flash + addr_start), 0xFF, addr_end - addr_start) ;

    return EOK ;
}

int32_t
platform_flash_write (uint32_t addr, uint32_t len, const uint8_t * data)
{
    uint32_t i ;
    if (addr >= PLATFORM_FLASH_SIZE) return E_PARM ;
    if (addr + len >= PLATFORM_FLASH_SIZE) return E_PARM ;

    for (i=0; i<len; i++) {
        _platform_flash[i+addr] &= data[i] ;
    }


    // memcpy ((void*)(_nvram_test + addr), data, len) ;

    return EOK ;
}

int32_t
platform_flash_read (uint32_t addr, uint32_t len, uint8_t * data)
{
    if (addr >= PLATFORM_FLASH_SIZE) return E_PARM ;
    if (addr + len >= PLATFORM_FLASH_SIZE) return E_PARM ;

    memcpy (data, (void*)(_platform_flash + addr), len) ;

    return EOK ;
}





static p_sem_t    _main_stop_sem ;
void 
status_callback (SVC_SERVICES_T  id, int32_t status)
{
    if (status == SVC_SERVICE_STATUS_STOPPED && id == _platform_complete_service_id) {
        os_sem_signal (&_main_stop_sem) ;
    }
}

void
platform_wait_for_exit (SVC_SERVICES_T service_id)
{
    _platform_complete_service_id = service_id ;
    os_sem_create (&_main_stop_sem, 0) ;
    SVC_SERVICE_HANDLER_T  handler ;
    svc_service_register_handler (&handler, status_callback) ;
    os_sem_wait (&_main_stop_sem) ;
    svc_service_unregister_handler (&handler) ;
    os_sem_delete (&_main_stop_sem) ;
}
