/*
    Copyright (C) 2015-2025, Navaro, All Rights Reserved
    SPDX-License-Identifier: MIT

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
 */

/**
 * @file    qoraal-flash/tallies_module.h
 * @brief   Generates a module's tallie block from its own list file.
 *
 * This is not a standalone header. It is included once per mode, with
 * TALLIES_MODULE and TALLIES_MODULE_LIST defined, and generates the enum, the
 * definition table, the RAM arrays and the block.
 *
 * The list file keeps the X-macro ergonomics of the old project-wide
 * tallies_lst.h, but it belongs to the module:
 *
 * @code
 *   // example_tallies_lst.h
 *   TALLIE_DEF       (started)
 *   TALLIE_DEF       (tick)
 *   TALLIE_DEF_RATE  (poll, SECONDS_TEN)
 * @endcode
 *
 * The module's own header declares the block:
 *
 * @code
 *   // example_tallies.h
 *   #define TALLIES_MODULE      example
 *   #define TALLIES_MODULE_LIST "example_tallies_lst.h"
 *   #include "qoraal-flash/tallies_module.h"
 *
 *   #define EXAMPLE_TALLIE_INC(x)  TALLIES_INC(example, x)
 * @endcode
 *
 * and exactly one .c defines it:
 *
 * @code
 *   #include "example_tallies.h"
 *   #define TALLIES_MODULE_DEFINE
 *   #include "qoraal-flash/tallies_module.h"
 * @endcode
 *
 * TALLIES_MODULE and TALLIES_MODULE_LIST are deliberately left defined after
 * the declare pass, so the defining .c does not have to repeat them.
 * TALLIES_MODULE_NAME is optional and defaults to the stringified module.
 *
 * When CONFIG_QORAAL_FLASH_TALLIES is off this header generates nothing and
 * the TALLIES_* accessors in tallies.h are no-ops, so module source is
 * unchanged either way.
 */

#include "qoraal-flash/tallies.h"

#ifndef TALLIES_MODULE
#error "define TALLIES_MODULE before including qoraal-flash/tallies_module.h"
#endif
#ifndef TALLIES_MODULE_LIST
#error "define TALLIES_MODULE_LIST before including qoraal-flash/tallies_module.h"
#endif

#if defined CONFIG_QORAAL_FLASH_TALLIES && CONFIG_QORAAL_FLASH_TALLIES

/*
 * Name construction. The two level indirection is what makes the arguments
 * expand before they are pasted.
 */
#undef _TALLIES_CAT_
#undef _TALLIES_CAT
#undef _TALLIES_STR_
#undef _TALLIES_STR
#undef _TALLIES_ID
#undef _TALLIES_SYM

#define _TALLIES_CAT_(a, b)     a ## b
#define _TALLIES_CAT(a, b)      _TALLIES_CAT_(a, b)
#define _TALLIES_STR_(x)        #x
#define _TALLIES_STR(x)         _TALLIES_STR_(x)

/** @brief __<module>_tallie_<x> - an enum member for one tallie. */
#define _TALLIES_ID(x)          _TALLIES_CAT(_TALLIES_CAT(__, TALLIES_MODULE), _TALLIES_CAT(_tallie_, x))
/** @brief _<module><suffix> - a generated object name. */
#define _TALLIES_SYM(suffix)    _TALLIES_CAT(_TALLIES_CAT(_, TALLIES_MODULE), suffix)

#ifndef TALLIES_MODULE_NAME
#define TALLIES_MODULE_NAME     _TALLIES_STR(TALLIES_MODULE)
#endif

#ifndef TALLIES_MODULE_DEFINE
/*---------------------------------------------------------------------------*/
/* Declare: the enum, the block and the accessors.                           */
/*---------------------------------------------------------------------------*/

#undef TALLIE_DEF
#undef TALLIE_DEF_RATE
#define TALLIE_DEF(x)           _TALLIES_ID(x) ,
#define TALLIE_DEF_RATE(x, s)   _TALLIES_ID(x) ,

typedef enum {
#include TALLIES_MODULE_LIST
    _TALLIES_ID(last)
} _TALLIES_CAT(TALLIES_MODULE, _tallies_t) ;

#undef TALLIE_DEF
#undef TALLIE_DEF_RATE

extern TALLIES_BLOCK_T _TALLIES_SYM(_tallies) ;

#else /* TALLIES_MODULE_DEFINE */
/*---------------------------------------------------------------------------*/
/* Define: the tables and the block itself. Include the module's own header   */
/* first, so that the enum above is in scope.                                 */
/*---------------------------------------------------------------------------*/

#undef TALLIE_DEF
#undef TALLIE_DEF_RATE
#define TALLIE_DEF(x)           { _TALLIES_STR(x), SECONDS_ZERO } ,
#define TALLIE_DEF_RATE(x, s)   { _TALLIES_STR(x), (s) } ,

static const TALLIES_DEF_T  _TALLIES_SYM(_tallies_defs)[] = {
#include TALLIES_MODULE_LIST
} ;

#undef TALLIE_DEF
#undef TALLIE_DEF_RATE

static TALLIES_ENTRY_T      _TALLIES_SYM(_tallies_entry)[_TALLIES_ID(last)] ;
static uint8_t              _TALLIES_SYM(_tallies_dirty)[_TALLIES_ID(last)] ;

TALLIES_BLOCK_T             _TALLIES_SYM(_tallies) =
        TALLIES_BLOCK_INIT(TALLIES_MODULE_NAME,
                           _TALLIES_SYM(_tallies_defs),
                           _TALLIES_SYM(_tallies_entry),
                           _TALLIES_SYM(_tallies_dirty),
                           _TALLIES_ID(last)) ;

#endif /* TALLIES_MODULE_DEFINE */

#endif /* CONFIG_QORAAL_FLASH_TALLIES */

/* One pass per include, whether or not anything was generated. */
#undef TALLIES_MODULE_DEFINE
