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

/*===========================================================================*/
/* Client pre-compile time settings.                                         */
/*===========================================================================*/

/*===========================================================================*/
/* Constants.                                                                */
/*===========================================================================*/

/*===========================================================================*/
/* Data structures and types.                                                */
/*===========================================================================*/
typedef struct REGENUM_VALUE_S {
    const char *name;
    int32_t value;
} REGENUM_VALUE_T;

typedef struct REGENUM_TYPE_S {
    const char *type_name;
    const REGENUM_VALUE_T *values;
    size_t count;
} REGENUM_TYPE_T;

/*===========================================================================*/
/* External declarations.                                                    */
/*===========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

    int32_t regenum_register_types(const REGENUM_TYPE_T *types);
    const REGENUM_TYPE_T *regenum_default_types_get(size_t *count);
    const REGENUM_TYPE_T *regenum_type_at(const REGENUM_TYPE_T *types, size_t type_index);
    const REGENUM_TYPE_T *regenum_find_type(const REGENUM_TYPE_T *types,
                                            const char *type_name);
    int32_t regenum_type_name_at(const REGENUM_TYPE_T *types, size_t type_index,
                                 const char **type_name);
    int32_t regenum_type_index(const REGENUM_TYPE_T *types, const char *type_name,
                               size_t *type_index);

    int32_t regenum_get_by_name(const REGENUM_TYPE_T *types,
                                const char *type_name, const char *name, int32_t *value);
    int32_t regenum_get_by_type_index_and_name(const REGENUM_TYPE_T *types,
                                               size_t type_index, const char *name,
                                               int32_t *value);
    int32_t regenum_get_by_value(const REGENUM_TYPE_T *types,
                                 const char *type_name, int32_t value, const char **name);
    int32_t regenum_get_by_type_index_and_value(const REGENUM_TYPE_T *types,
                                                size_t type_index, int32_t value,
                                                const char **name);
    int32_t regenum_get_by_inx(const REGENUM_TYPE_T *types,
                               const char *type_name, size_t idx, const char **name,
                               int32_t *value);
    int32_t regenum_get_by_type_index_and_inx(const REGENUM_TYPE_T *types,
                                              size_t type_index, size_t idx,
                                              const char **name, int32_t *value);

    int32_t regenum_get_next(const REGENUM_TYPE_T *types,
                             const char *type_name, int32_t value);
    int32_t regenum_get_next_by_type_index(const REGENUM_TYPE_T *types,
                                           size_t type_index, int32_t value);
    int32_t regenum_get_prev(const REGENUM_TYPE_T *types,
                             const char *type_name, int32_t value);
    int32_t regenum_get_prev_by_type_index(const REGENUM_TYPE_T *types,
                                           size_t type_index, int32_t value);

#ifdef __cplusplus
}
#endif

#endif /* CORAL_UTILS_REGISTRY_REGENUM_I_ */
