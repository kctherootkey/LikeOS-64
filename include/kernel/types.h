// LikeOS-64 Common Types - Standard type definitions for kernel
// Provides consistent type definitions across all kernel modules

#ifndef _KERNEL_TYPES_H_
#define _KERNEL_TYPES_H_

// Standard integer types for 64-bit kernel
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned short uint16_t;
typedef short int16_t;
typedef unsigned char uint8_t;
typedef char int8_t;
typedef unsigned long size_t;

// Boolean type for kernel
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#endif

// Common NULL definition
#ifndef NULL
#define NULL ((void*)0)
#endif

#endif // _KERNEL_TYPES_H_
