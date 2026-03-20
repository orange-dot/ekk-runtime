/*
 * ekk_stdint.h -- minimal integer type definitions for l4v C parser.
 *
 * Copyright (c) 2026 Elektrokombinacija
 * SPDX-License-Identifier: MIT
 *
 * The system <stdint.h> may include GCC-specific extensions rejected by
 * the l4v C parser. This stub provides only the types used in the
 * verified core, defined as plain C typedef chains.
 *
 * Used when EKK_VERIFICATION is defined. Normal builds use <stdint.h>.
 */

#ifndef EKK_STDINT_H
#define EKK_STDINT_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

/* Limits */
#define INT8_MIN    (-128)
#define INT8_MAX    127
#define INT16_MIN   (-32768)
#define INT16_MAX   32767
#define INT32_MIN   (-2147483647 - 1)
#define INT32_MAX   2147483647
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   9223372036854775807LL

#define UINT8_MAX   255U
#define UINT16_MAX  65535U
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL

/* NULL and size_t — normally from <stddef.h> */
#ifndef NULL
#define NULL ((void *)0)
#endif
typedef unsigned long size_t;

#endif /* EKK_STDINT_H */
