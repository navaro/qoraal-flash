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


/* Head of the global linked list of registered enum types. */
static REGENUM_TYPE_T *_regenum_head = NULL;

/* --- Private helpers ---------------------------------------------------- */

/* Walk the linked list and return the type matching the given unique ID. */
static const REGENUM_TYPE_T *
regenum_type_by_id_internal(int32_t id)
{
    const REGENUM_TYPE_T *cur = _regenum_head;

    while (cur) {
        if (cur->id == id) {
            return cur;
        }
        cur = cur->next;
    }

    return NULL;
}

/* Walk the linked list and return the type matching the given name. */
static const REGENUM_TYPE_T *
regenum_find_type_internal(const char *type_name)
{
    const REGENUM_TYPE_T *cur = _regenum_head;

    if (!type_name) {
        return NULL;
    }

    while (cur) {
        if (cur->type_name && strcmp(cur->type_name, type_name) == 0) {
            return cur;
        }
        cur = cur->next;
    }

    return NULL;
}

/* --- Public API ---------------------------------------------------------- */

/*
 * Register a single enum type.  The caller fills all fields of *type
 * (type_name, values, count) before calling.  id must be unique across all
 * registered types; it is stored in type->id and is used as the enum-type
 * discriminator in the registry on-flash format (must fit in 8 bits: 0-255).
 * Types are appended in registration order so iteration is deterministic.
 */
int32_t
regenum_register_type(REGENUM_TYPE_T *type, int32_t id)
{
    REGENUM_TYPE_T *cur;
    REGENUM_TYPE_T *tail = NULL;

    if (!type || !type->type_name) {
        return E_PARM;
    }

    if (regenum_type_by_id_internal(id) != NULL) {
        return E_INVAL; /* duplicate ID */
    }

    type->id   = (uint16_t)id;
    type->next = NULL;

    /* Append at tail to preserve registration order. */
    cur = _regenum_head;
    while (cur) {
        tail = cur;
        cur  = cur->next;
    }

    if (tail) {
        tail->next = type;
    } else {
        _regenum_head = type;
    }

    return EOK;
}

/* Return the first registered type (head of the linked list). */
const REGENUM_TYPE_T *
regenum_type_first(void)
{
    return _regenum_head;
}

/* Return the type that follows 'type' in the linked list, or NULL. */
const REGENUM_TYPE_T *
regenum_type_next(const REGENUM_TYPE_T *type)
{
    return type ? type->next : NULL;
}

/* Look up a registered type by its unique ID. */
const REGENUM_TYPE_T *
regenum_type_at(size_t id)
{
    return regenum_type_by_id_internal((int32_t)id);
}

/* Look up a registered type by name. */
const REGENUM_TYPE_T *
regenum_find_type(const char *type_name)
{
    return regenum_find_type_internal(type_name);
}

/*
 * Walk the linked list and return its length via *count.
 * Returns the head pointer (same as regenum_type_first).
 */
const REGENUM_TYPE_T *
regenum_type_list_get(size_t *count)
{
    if (count) {
        size_t n = 0;
        const REGENUM_TYPE_T *cur = _regenum_head;

        while (cur) {
            n++;
            cur = cur->next;
        }
        *count = n;
    }

    return _regenum_head;
}

/* Return the name of the type with the given ID. */
int32_t
regenum_type_name_at(size_t id, const char **type_name)
{
    const REGENUM_TYPE_T *enum_type;

    enum_type = regenum_type_at(id);
    if (!enum_type || !type_name) {
        return E_NOTFOUND;
    }

    *type_name = enum_type->type_name;
    return EOK;
}

/* Return the unique ID of the named type in *type_index. */
int32_t
regenum_type_index(const char *type_name, size_t *type_index)
{
    const REGENUM_TYPE_T *found;

    if (!type_name || !type_index) {
        return E_PARM;
    }

    found = regenum_find_type_internal(type_name);
    if (!found) {
        return E_NOTFOUND;
    }

    *type_index = (size_t)found->id;
    return EOK;
}

int32_t
regenum_get_by_name(const char *type_name, const char *name, int32_t *value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_find_type_internal(type_name);
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
regenum_get_by_id_and_name(int32_t id, const char *name, int32_t *value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_type_by_id_internal(id);
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
regenum_get_by_value(const char *type_name, int32_t value, const char **name)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_find_type_internal(type_name);
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
regenum_get_by_id_and_value(int32_t id, int32_t value, const char **name)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_type_by_id_internal(id);
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
regenum_get_by_inx(const char *type_name, size_t idx, const char **name, int32_t *value)
{
    const REGENUM_TYPE_T *enum_type;

    enum_type = regenum_find_type_internal(type_name);
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
regenum_get_by_id_and_inx(int32_t id, size_t idx, const char **name, int32_t *value)
{
    const REGENUM_TYPE_T *enum_type;

    enum_type = regenum_type_by_id_internal(id);
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
regenum_get_next(const char *type_name, int32_t value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_find_type_internal(type_name);
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
regenum_get_next_by_id(int32_t id, int32_t value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_type_by_id_internal(id);
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
regenum_get_prev(const char *type_name, int32_t value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_find_type_internal(type_name);
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

int32_t
regenum_get_prev_by_id(int32_t id, int32_t value)
{
    const REGENUM_TYPE_T *enum_type;
    size_t i;

    enum_type = regenum_type_by_id_internal(id);
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

