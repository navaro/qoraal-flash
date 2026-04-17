/*
 * regenum.c
 *
 *  Created on: Apr 19, 2021
 *      Author: natie
 */


#include <stdint.h>
#include <string.h>
#include "qoraal/errordef.h"
#include "qoraal-flash/regenum.h"


static const REGENUM_TYPE_T *_regenum_default_types = 0;

static const REGENUM_TYPE_T *
regenum_resolve_types(const REGENUM_TYPE_T *types)
{
    return types ? types : _regenum_default_types;
}

static const REGENUM_TYPE_T *
regenum_find_type_internal(const REGENUM_TYPE_T *types, const char *type_name)
{
    size_t i;

    if (!types || !type_name) {
        return NULL;
    }

    for (i = 0; types[i].type_name != NULL; i++) {
        if (strcmp(type_name, types[i].type_name) == 0) {
            return &types[i];
        }
    }

    return NULL;
}

int32_t
regenum_register_types(const REGENUM_TYPE_T *types)
{
    _regenum_default_types = types;

    return EOK;
}

const REGENUM_TYPE_T *
regenum_default_types_get(size_t *count)
{
    if (count) {
        size_t i = 0;
        if (_regenum_default_types) {
            while (_regenum_default_types[i].type_name) i++;
        }
        *count = i;
    }

    return _regenum_default_types;
}

const REGENUM_TYPE_T *
regenum_find_type(const REGENUM_TYPE_T *types, const char *type_name)
{
    types = regenum_resolve_types(types);
    return regenum_find_type_internal(types, type_name);
}

int32_t
regenum_get_by_name(const REGENUM_TYPE_T *types,
                    const char *type_name, const char *name, int32_t *value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    types = regenum_resolve_types(types);
    enum_type = regenum_find_type_internal(types, type_name);
    if (!enum_type || !name) {
        return E_NOTFOUND;
    }

    for (i = 0; i < enum_type->count; i++) {
        if (strcmp(enum_type->values[i].name, name) == 0) {
            if (value) {
                *value = enum_type->values[i].value;
            }
            return EOK;
        }
    }

    return E_NOTFOUND;
}

int32_t
regenum_get_by_value(const REGENUM_TYPE_T *types,
                     const char *type_name, int32_t value, const char **name)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    types = regenum_resolve_types(types);
    enum_type = regenum_find_type_internal(types, type_name);
    if (!enum_type) {
        if (name) {
            *name = "";
        }
        return E_NOTFOUND;
    }

    for (i = 0; i < enum_type->count; i++) {
        if (enum_type->values[i].value == value) {
            if (name) {
                *name = enum_type->values[i].name;
            }
            return EOK;
        }
    }

    if (name) {
        *name = "";
    }
    return E_NOTFOUND;
}

int32_t
regenum_get_by_inx(const REGENUM_TYPE_T *types,
                   const char *type_name, size_t idx, const char **name, int32_t *value)
{
    const REGENUM_TYPE_T *enum_type;

    types = regenum_resolve_types(types);
    enum_type = regenum_find_type_internal(types, type_name);
    if (!enum_type) {
        return E_NOTFOUND;
    }

    if (idx >= enum_type->count) {
        return E_EOF;
    }

    if (name) {
        *name = enum_type->values[idx].name;
    }
    if (value) {
        *value = enum_type->values[idx].value;
    }

    return EOK;
}

int32_t
regenum_get_next(const REGENUM_TYPE_T *types,
                 const char *type_name, int32_t value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    types = regenum_resolve_types(types);
    enum_type = regenum_find_type_internal(types, type_name);
    if (!enum_type || enum_type->count == 0) {
        return E_NOTFOUND;
    }

    for (i = 0; i < enum_type->count; i++) {
        if (enum_type->values[i].value == value) {
            break;
        }
    }

    if (i >= (enum_type->count - 1)) {
        i = 0;
    } else {
        i++;
    }

    return enum_type->values[i].value;
}

int32_t
regenum_get_prev(const REGENUM_TYPE_T *types,
                 const char *type_name, int32_t value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    types = regenum_resolve_types(types);
    enum_type = regenum_find_type_internal(types, type_name);
    if (!enum_type || enum_type->count == 0) {
        return E_NOTFOUND;
    }

    for (i = 0; i < enum_type->count; i++) {
        if (enum_type->values[i].value == value) {
            break;
        }
    }

    if (i >= enum_type->count || i == 0) {
        i = enum_type->count - 1;
    } else {
        i--;
    }

    return enum_type->values[i].value;

}

