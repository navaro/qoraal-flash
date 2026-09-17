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
            SVC_SHELL_NEWLINE "%u tallies, %u reset on load"
            SVC_SHELL_NEWLINE,
            (unsigned int)shown,
            (unsigned int)tallies_mismatched()) ;
}

/**
 * @brief   Report the state of the volume the counters are persisted to.
 *
 * The snapshot is taken under the volume lock by tallies_status_get(); all of
 * the printing below happens with that lock released, so a session on a slow
 * transport cannot hold up the persist task.
 */
static void
tallies_status (SVC_SHELL_IF_T * pif)
{
    NVOL3_STATUS_T status ;

    if (tallies_status_get (&status) != EOK) {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
                "tallies volume not available" SVC_SHELL_NEWLINE) ;
        return ;
    }

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "volume   %s, version 0x%.4x" SVC_SHELL_NEWLINE,
            status.name ? status.name : "(unnamed)",
            (unsigned int)status.version) ;

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "records  %u of %u, %u inuse, %u invalid, %u error"
            SVC_SHELL_NEWLINE,
            (unsigned int)status.records_used, (unsigned int)status.records_max,
            (unsigned int)status.inuse, (unsigned int)status.invalid,
            (unsigned int)status.error) ;

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "sector   0x%.6x in use, %u bytes, %u per record, next slot %u"
            SVC_SHELL_NEWLINE,
            (unsigned int)status.sector, (unsigned int)status.sector_size,
            (unsigned int)status.record_size, (unsigned int)status.next_idx) ;

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "         0x%.6x version 0x%.4x flags 0x%.8x" SVC_SHELL_NEWLINE,
            (unsigned int)status.sector1_addr,
            (unsigned int)status.sector1_version,
            (unsigned int)status.sector1_flags) ;
    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "         0x%.6x version 0x%.4x flags 0x%.8x" SVC_SHELL_NEWLINE,
            (unsigned int)status.sector2_addr,
            (unsigned int)status.sector2_version,
            (unsigned int)status.sector2_flags) ;

    /*
     * The lookup table is the only part of this that costs RAM rather than
     * FLASH, so it is the number worth watching when the hash size is tuned.
     */
    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "lookup   %u bytes, %u buckets, %u used, longest chain %u"
            SVC_SHELL_NEWLINE,
            (unsigned int)status.lookup_bytes, (unsigned int)status.hash_size,
            (unsigned int)status.hash_used,
            (unsigned int)status.hash_max_chain) ;

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "reset    %u records dropped on load" SVC_SHELL_NEWLINE,
            (unsigned int)tallies_mismatched()) ;
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
            tallies_persist () ;
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                    "tallies persisted" SVC_SHELL_NEWLINE) ;
            return SVC_SHELL_CMD_E_OK ;
        }

        if (strcmp (argv[1], "status") == 0) {
            tallies_status (pif) ;
            return SVC_SHELL_CMD_E_OK ;
        }

        tallies_dump (pif, argv[1]) ;
        return SVC_SHELL_CMD_E_OK ;
    }

    tallies_dump (pif, 0) ;

    return SVC_SHELL_CMD_E_OK ;
}

#endif /* CONFIG_QORAAL_FLASH_TALLIES */
