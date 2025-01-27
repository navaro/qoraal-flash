#include <stdio.h>
#include "qoraal/qoraal.h"
#include "qoraal/svc/svc_shell.h"
#include "qoraal-flash/syslog.h"

SVC_SHELL_CMD_DECL( "log", qshell_cmd_log,  "[log] [severity] [cnt]");


int32_t qshell_cmd_log (SVC_SHELL_IF_T * pif, char** argv, int argc)
{
#define LOG_MSG_SIZE    (sizeof(QORAAL_LOG_MSG_T) + 200)
    QORAAL_LOG_MSG_T *  msg = qoraal_malloc(QORAAL_HeapAuxiliary, LOG_MSG_SIZE) ;
    unsigned int cnt = 16 ;
    unsigned int severity = 6 ;
    QORAAL_LOG_IT_T * it = 0 ;
    uint32_t log = 0 ;

    int32_t res = 0 ;

    if (argc > 1) {
        sscanf(argv[1], "%u", &log) ;
        if (log) log = 1 ;

    }
    if (argc > 2) {
        sscanf(argv[2], "%u", &severity) ;

    }
    if (argc > 3) {
        sscanf(argv[3], "%u", &cnt) ;

    }

    it = syslog_platform_it_create (log) ;

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "severity<=%d   (log [severity] [count])" SVC_SHELL_NEWLINE
            "---------------------------------------" SVC_SHELL_NEWLINE,
            severity) ;

    if (it) {
        while (cnt && (it->get (it, msg, LOG_MSG_SIZE) >= EOK)) {

            if (msg->severity <= severity) {

                svc_shell_print (pif, SVC_SHELL_OUT_STD,
                        "%.6d (%d) - "
                        "%.4d-%.2d-%.2d "
                        "%.2d:%.2d:%.2d:  "
                        "%s\r\n" ,
                        msg->id,
                        msg->severity,
                        msg->date.year, msg->date.month, msg->date.day,
                        msg->time.hour, msg->time.minute, msg->time.second,
                        msg->msg) ;

                cnt-- ;

            }

            if (it->prev(it) != EOK) break ;

        }
        syslog_platform_it_destroy (it) ;

    }

    qoraal_free (QORAAL_HeapAuxiliary, msg) ;

    return SVC_SHELL_CMD_E_OK ;
}

void
keep_syslogcmds(void)
{
    (void)qshell_cmd_log ;
}