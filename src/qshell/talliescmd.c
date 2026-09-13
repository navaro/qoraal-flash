#include <stdio.h>
#include <string.h>
#include "qoraal/qoraal.h"
#include "qoraal/svc/svc_shell.h"
#include "qoraal-flash/tallies.h"

#if defined CONFIG_QORAAL_FLASH_TALLIES && CONFIG_QORAAL_FLASH_TALLIES

SVC_SHELL_CMD_DECL( "tallies", qshell_cmd_tallies,  "[reset|<module>]");

/*
 * Dumps every registered block. This is the same walk a publisher will do:
 * block name plus local name composes the display name, so a module's counters
 * read as "cardreader.imgfail" without any module having to spell that out in
 * its own list.
 */
static void
tallies_dump (SVC_SHELL_IF_T * pif, const char * filter)
{
    TALLIES_BLOCK_T * blk ;
    uint32_t shown = 0 ;

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "id     name                        value  updated" SVC_SHELL_NEWLINE
            "--------------------------------------------------------------------"
            SVC_SHELL_NEWLINE) ;

    for (blk = tallies_block_first(); blk; blk = tallies_block_next(blk)) {
        uint16_t local ;

        if (filter && strcmp (filter, blk->name) != 0) {
            continue ;
        }

        for (local = 0; local < blk->count; local++) {
            TALLIES_ENTRY_T entry ;
            char name[64] ;

            if (tallies_get (blk, local, &entry) != EOK) {
                continue ;
            }

            snprintf (name, sizeof(name), "%s.%s", blk->name,
                    tallies_name (blk, local)) ;

            if (entry.date.date) {
                svc_shell_print (pif, SVC_SHELL_OUT_STD,
                        "%-6d %-24s %10u  %.4d-%.2d-%.2d %.2d:%.2d:%.2d"
                        SVC_SHELL_NEWLINE,
                        (int)(blk->base + local), name,
                        (unsigned int)entry.value,
                        entry.date.year, entry.date.month, entry.date.day,
                        entry.time.hour, entry.time.minute, entry.time.second) ;

            } else {
                svc_shell_print (pif, SVC_SHELL_OUT_STD,
                        "%-6d %-24s %10u  -" SVC_SHELL_NEWLINE,
                        (int)(blk->base + local), name,
                        (unsigned int)entry.value) ;
            }

            shown++ ;
        }
    }

    if (!shown) {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
                "no tallies%s" SVC_SHELL_NEWLINE,
                filter ? " for that module" : " registered") ;
        return ;
    }

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            SVC_SHELL_NEWLINE "%u tallies, %u reset on load, %u timer drops"
            SVC_SHELL_NEWLINE,
            (unsigned int)shown,
            (unsigned int)tallies_mismatched(),
            (unsigned int)tallies_timer_drops()) ;
}

int32_t qshell_cmd_tallies (SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    if (argc > 1) {
        if (strcmp (argv[1], "reset") == 0) {
            tallies_reset () ;
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                    "tallies reset" SVC_SHELL_NEWLINE) ;
            return SVC_SHELL_CMD_E_OK ;
        }

        if (strcmp (argv[1], "persist") == 0) {
            tallies_persist (1) ;
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                    "tallies persisted" SVC_SHELL_NEWLINE) ;
            return SVC_SHELL_CMD_E_OK ;
        }

        if (strcmp (argv[1], "status") == 0) {
            tallies_log_status () ;
            return SVC_SHELL_CMD_E_OK ;
        }

        tallies_dump (pif, argv[1]) ;
        return SVC_SHELL_CMD_E_OK ;
    }

    tallies_dump (pif, 0) ;

    return SVC_SHELL_CMD_E_OK ;
}

void
keep_talliescmds (void)
{
    (void)qshell_cmd_tallies ;
}

#else

void
keep_talliescmds (void)
{
}

#endif /* CONFIG_QORAAL_FLASH_TALLIES */
