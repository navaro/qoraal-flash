#include <stdio.h>
#include "qoraal/qoraal.h"
#include "qoraal/svc/svc_shell.h"
#include "qoraal-flash/regenum.h"
#include "qoraal-flash/registry.h"

SVC_SHELL_CMD_DECL( "reg", qshell_cmd_reg,  "[entry] [value]");
SVC_SHELL_CMD_DECL( "regadd", qshell_cmd_regadd,  "<entry> <value> [str/int/enum_type]");
SVC_SHELL_CMD_DECL( "regenum", qshell_cmd_regenum,  "[enum_type]");
SVC_SHELL_CMD_DECL( "regdel", qshell_cmd_regdel,  "<entry>");
SVC_SHELL_CMD_DECL( "regstats", qshell_cmd_regstats,  "");
SVC_SHELL_CMD_DECL( "regrepair", qshell_regrepair,  "");
SVC_SHELL_CMD_DECL( "regerase", qshell_regerase,  "");


#define REGISTRY_VALUE_MAX				192

static const char *
reg_type_name(uint16_t type, char * buffer, size_t length)
{
    const char * enum_type_name ;

    switch (REGISTRY_GET_TYPE(type)) {
    case REGISTRY_TYPE_STRING:
        return "str" ;

    case REGISTRY_TYPE_INT:
        return "int" ;

    case REGISTRY_TYPE_ENUM:
        if ((regenum_type_name_at(0, REGISTRY_GET_ENUM_TYPE(type), &enum_type_name) == EOK) &&
                enum_type_name) {
            snprintf(buffer, length, "enum:%s", enum_type_name) ;
        } else {
            snprintf(buffer, length, "enum:%u", (unsigned int)REGISTRY_GET_ENUM_TYPE(type)) ;
        }
        return buffer ;

    case REGISTRY_TYPE_BLOB:
        return "blb" ;

    default:
        snprintf(buffer, length, "type:%u", (unsigned int)REGISTRY_GET_TYPE(type)) ;
        return buffer ;
    }
}

static int32_t
reg_value_to_string(REGISTRY_KEY_T key, const char * raw_value, uint16_t type,
        char * value, uint32_t length)
{
    int32_t res ;

    res = registry_get_strval (key, value, length, &type) ;
    if (res >= 0) {
        return res ;
    }

    if (length == 0) {
        return E_PARM ;
    }

    switch (REGISTRY_GET_TYPE(type)) {
    case REGISTRY_TYPE_STRING:
        snprintf(value, length, "%s", raw_value) ;
        return EOK ;

    case REGISTRY_TYPE_INT:
    case REGISTRY_TYPE_ENUM: {
        int32_t intval = 0 ;
        memcpy(&intval, raw_value, sizeof(int32_t)) ;
        snprintf(value, length, "%d", (int)intval) ;
        return EOK ;
    }

    default:
        value[0] = '\0' ;
        return res ;
    }
}

static int32_t
reg_parse_type_arg(const char * type_arg, uint16_t * type)
{
    size_t enum_type = 0 ;
    const char *enum_name = type_arg ;

    if (!type_arg || !type) {
        return E_PARM ;
    }

    if (strcmp(type_arg, "str") == 0) {
        *type = REGISTRY_TYPE(REGISTRY_TYPE_STRING, 0) ;
        return EOK ;
    }

    if (strcmp(type_arg, "int") == 0) {
        *type = REGISTRY_TYPE(REGISTRY_TYPE_INT, 0) ;
        return EOK ;
    }

    if (strncmp(type_arg, "enum:", 5) == 0) {
        enum_name = type_arg + 5 ;
    }

    if (regenum_type_index(0, enum_name, &enum_type) == EOK) {
        *type = REGISTRY_TYPE(REGISTRY_TYPE_ENUM, enum_type) ;
        return EOK ;
    }

    return E_INVAL ;
}

static void
reg_print_enum_error(SVC_SHELL_IF_T * pif, const char * value, uint16_t type, int32_t res)
{
    char type_name[64] ;

    if (res == E_NOTFOUND) {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "enum value %s is not valid for %s" SVC_SHELL_NEWLINE,
            value, reg_type_name(type, type_name, sizeof(type_name))) ;
    } else {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "error %d for enum value %s in %s" SVC_SHELL_NEWLINE,
            res, value, reg_type_name(type, type_name, sizeof(type_name))) ;
    }
}

static void
reg_print (SVC_SHELL_IF_T * pif, REGISTRY_KEY_T key, const char* value, uint16_t type)
{
    char tmp[40] ;
    char type_name[64] ;
    char display_value[REGISTRY_VALUE_MAX] ;

    snprintf(tmp, sizeof(tmp), "%s:", key) ;
    if (REGISTRY_GET_TYPE(type) == REGISTRY_TYPE_BLOB) {
        uint8_t tmpvalue[128] ;
        registry_blob_value_get (key, tmpvalue, 128) ;
        svc_shell_print_table (pif, SVC_SHELL_OUT_STD,
            tmp, 24, "[%s] %s" SVC_SHELL_NEWLINE,
            reg_type_name(type, type_name, sizeof(type_name)), tmpvalue) ;
    } else if (reg_value_to_string(key, value, type, display_value, sizeof(display_value)) >= 0) {
        svc_shell_print_table (pif, SVC_SHELL_OUT_STD,
            tmp, 24, "[%s] %s" SVC_SHELL_NEWLINE,
            reg_type_name(type, type_name, sizeof(type_name)), display_value) ;
    }

}

static uint32_t
reg_show(SVC_SHELL_IF_T * pif, const char * search, char * value, uint32_t len)
{
    REGISTRY_KEY_T key ;
    uint16_t type ;
    uint32_t cnt = 0 ;
    int32_t res = registry_first (&key, value, &type, len) ;
    while ((res >= 0) || (res == E_NOTFOUND)) {

        if (!search || strstr(key, search)) {

            reg_print (pif, key, value, type) ;

            cnt++ ;
        }
        res = registry_next (&key, value, &type, len) ;

    }

    return cnt ;
}


int32_t qshell_cmd_reg (SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    uint16_t type ;
    char value[REGISTRY_VALUE_MAX] ;
    int32_t res = 0 ;

    if (argc == 1) {
        res = reg_show (pif, 0, value, REGISTRY_VALUE_MAX) ;
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
                SVC_SHELL_NEWLINE "        %d entries found." SVC_SHELL_NEWLINE, res) ;

    }
    else if (argc == 2) {

        res = registry_value_get (argv[1], value, &type, REGISTRY_VALUE_MAX) ;
        if (res > 0) {
            reg_print (pif, argv[1], value, type) ;

        } else {
            res = reg_show (pif, argv[1], value, REGISTRY_VALUE_MAX) ;
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                    SVC_SHELL_NEWLINE "        %d entries found." SVC_SHELL_NEWLINE, res) ;

        }

    }
    else if (argc == 3) {
        int32_t stored_type = registry_value_type (argv[1]) ;
        uint16_t type_hint = stored_type >= 0 ? (uint16_t)stored_type : REGISTRY_TYPE_NONE ;

        res = registry_set_strval (argv[1], argv[2], type_hint) ;

        if ((res < 0) && (stored_type >= 0) &&
                (REGISTRY_GET_TYPE((uint16_t)stored_type) == REGISTRY_TYPE_ENUM)) {
            reg_print_enum_error (pif, argv[2], (uint16_t)stored_type, res) ;
        } else {
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                "%s (%d)" SVC_SHELL_NEWLINE, res == EOK ? "OK" : "ERR", res) ;
        }

    }


    return res >= 0 ? SVC_SHELL_CMD_E_OK : SVC_SHELL_CMD_E_FAIL ;
}

int32_t
qshell_cmd_regadd(SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    uint16_t type ;
    int32_t res = SVC_SHELL_CMD_E_OK ;
    int32_t val ;

    if (argc < 3) {
        return SVC_SHELL_CMD_E_PARMS ;

    }

    res = registry_value_length (argv[1]) ;
    if (res > 0 ) {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "registry setting %s exists" SVC_SHELL_NEWLINE, argv[1]) ;

        return res ;
    }

    if (argc == 3) {

        if (svc_shell_scan_int (argv[2], (uint32_t*)&val) == EOK) {
            type = REGISTRY_TYPE(REGISTRY_TYPE_INT, 0) ;
        } else {
            type = REGISTRY_TYPE(REGISTRY_TYPE_STRING, 0) ;
        }
        res = EOK ;

    } else /*if(argc > 3)*/ {

        res = reg_parse_type_arg (argv[3], &type) ;

    }

    if (res == EOK) {

        res = registry_set_strval (argv[1], argv[2], type) ;

        if (res >= 0) {
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                "%s (%d)" SVC_SHELL_NEWLINE, res == EOK ? "OK" : "ERR", res) ;

        } else if (REGISTRY_GET_TYPE(type) == REGISTRY_TYPE_ENUM) {
            reg_print_enum_error (pif, argv[2], type, res) ;
        } else {
            svc_shell_print (pif, SVC_SHELL_OUT_STD,
                "error %d for %s not valid" SVC_SHELL_NEWLINE, res, argv[2]) ;

        }

    } else {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "type %s for %s not valid" SVC_SHELL_NEWLINE, argv[3], argv[2]) ;

    }

    return res ;
}

int32_t
qshell_cmd_regenum(SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    const REGENUM_TYPE_T *types ;
    size_t type_count = 0 ;
    size_t i ;
    uint32_t shown = 0 ;

    if (argc > 2) {
        return SVC_SHELL_CMD_E_PARMS ;
    }

    types = regenum_default_types_get(&type_count) ;
    if (!types || (type_count == 0)) {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "no enum types registered" SVC_SHELL_NEWLINE) ;
        return SVC_SHELL_CMD_E_OK ;
    }

    svc_shell_print (pif, SVC_SHELL_OUT_STD,
        "registered enum types: %u" SVC_SHELL_NEWLINE, (unsigned int)type_count) ;

    for (i = 0 ; i < type_count ; i++) {
        const REGENUM_TYPE_T *enum_type = regenum_type_at(0, i) ;
        char type_label[48] ;
        size_t j ;

        if (!enum_type || !enum_type->type_name) {
            continue ;
        }

        if ((argc == 2) && (strcmp(argv[1], enum_type->type_name) != 0)) {
            continue ;
        }

        shown++ ;
        snprintf(type_label, sizeof(type_label), "%u:%s", (unsigned int)i, enum_type->type_name) ;
        svc_shell_print_table (pif, SVC_SHELL_OUT_STD,
            type_label, 28, "%u values" SVC_SHELL_NEWLINE, (unsigned int)enum_type->count) ;

        if (argc == 2) {
            for (j = 0 ; j < enum_type->count ; j++) {
                svc_shell_print_table (pif, SVC_SHELL_OUT_STD,
                    "", 28, "  %-20s = %d" SVC_SHELL_NEWLINE,
                    enum_type->values[j].name,
                    (int)enum_type->values[j].value) ;
            }
        }
    }
   

    if ((argc == 2) && (shown == 0)) {
        svc_shell_print (pif, SVC_SHELL_OUT_STD,
            "enum type %s not found" SVC_SHELL_NEWLINE, argv[1]) ;
        return SVC_SHELL_CMD_E_FAIL ;
    }

    return SVC_SHELL_CMD_E_OK ;
}

int32_t qshell_cmd_regdel(SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    int32_t res ;

    if (argc != 2) {
        return SVC_SHELL_CMD_E_PARMS ;

    }

    res = registry_value_delete (argv[1]) ;
    svc_shell_print (pif, SVC_SHELL_OUT_STD, "%s (%d)\r\n", res == EOK ? "OK" : "ERR", res) ;

    return SVC_SHELL_CMD_E_OK ;
}

int32_t qshell_regerase (SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    int32_t status = registry_erase () ;
    svc_shell_print (pif, SVC_SHELL_OUT_STD, "%s\r\n",
            status == EOK ? "OK" : "ERR") ;

    return SVC_SHELL_CMD_E_OK ;
}

int32_t qshell_regrepair (SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    int32_t status = registry_repair () ;
    svc_shell_print (pif, SVC_SHELL_OUT_STD, "%s\r\n",
            status == EOK ? "OK" : "ERR") ;

    return SVC_SHELL_CMD_E_OK ;
}

int32_t qshell_cmd_regstats (SVC_SHELL_IF_T * pif, char** argv, int argc)
{
    registry_log_status () ;

    return SVC_SHELL_CMD_E_OK ;
}

void
keep_registrycmds(void)
{
    (void)qshell_cmd_reg ;
    (void)qshell_cmd_regadd ;
    (void)qshell_cmd_regenum ;
    (void)qshell_cmd_regdel ;
    (void)qshell_cmd_regstats ;
    (void)qshell_regerase ;
    (void)qshell_regrepair ;
}