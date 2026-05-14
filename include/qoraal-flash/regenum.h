/*
 * regenum.inc
 *
 *  Created on: Jun 22, 2020
 *      Author: natie
 */

#ifndef CORAL_UTILS_REGISTRY_REGENUM_H_
#define CORAL_UTILS_REGISTRY_REGENUM_H_

#include <stddef.h>
#include <stdint.h>
#include "qoraal/common/types.h"

/*===========================================================================*/
/* Client pre-compile time settings.                                         */
/*===========================================================================*/

/*===========================================================================*/
/* Constants.                                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Data structures and types.                                                */
/*===========================================================================*/
typedef QORAAL_ENUM_VALUE_T REGENUM_VALUE_T;
typedef QORAAL_ENUM_TYPE_T REGENUM_TYPE_T;

#define REGENUM_TYPE_ID_MAX 0x1FFF

/*===========================================================================*/
/* External declarations.                                                    */
/*===========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

    /* Register a single type into the global linked list.
     * 'id' is the unique discriminator stored in on-flash registry records
     * (must fit in 13 bits: 0-8191). The id and next fields of *type are set
     * by this function; all other fields must be filled by the caller. */
    int32_t regenum_register_type(REGENUM_TYPE_T *type, int32_t id);

    /* Linked-list iteration. */
    const REGENUM_TYPE_T *regenum_type_first(void);
    const REGENUM_TYPE_T *regenum_type_next(const REGENUM_TYPE_T *type);

    /* Look up by unique ID. */
    const REGENUM_TYPE_T *regenum_type_at(size_t id);

    /* Look up by name. */
    const REGENUM_TYPE_T *regenum_find_type(const char *type_name);

    /* Returns the head of the list and writes the count of registered types into *count. */
    const REGENUM_TYPE_T *regenum_type_list_get(size_t *count);

    /* Returns the name of the type with the given ID. */
    int32_t regenum_type_name_at(size_t id, const char **type_name);

    /* Returns the unique ID of the named type in *type_index. */
    int32_t regenum_type_index(const char *type_name, size_t *type_index);

    int32_t regenum_get_by_name(const char *type_name, const char *name, int32_t *value);
    int32_t regenum_get_by_id_and_name(int32_t id, const char *name, int32_t *value);
    int32_t regenum_get_by_value(const char *type_name, int32_t value, const char **name);
    int32_t regenum_get_by_id_and_value(int32_t id, int32_t value, const char **name);
    int32_t regenum_get_by_inx(const char *type_name, size_t idx, const char **name,
                               int32_t *value);
    int32_t regenum_get_by_id_and_inx(int32_t id, size_t idx, const char **name,
                                      int32_t *value);

    int32_t regenum_get_next(const char *type_name, int32_t value);
    int32_t regenum_get_next_by_id(int32_t id, int32_t value);
    int32_t regenum_get_prev(const char *type_name, int32_t value);
    int32_t regenum_get_prev_by_id(int32_t id, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* CORAL_UTILS_REGISTRY_REGENUM_I_ */
