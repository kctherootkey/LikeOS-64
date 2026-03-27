/*
 * aclikeos.h - ACPICA platform header for LikeOS-64
 */

#ifndef __ACLIKEOS_H__
#define __ACLIKEOS_H__

/* 64-bit x86_64 kernel */
#define ACPI_MACHINE_WIDTH          64
#define COMPILER_DEPENDENT_INT64    long long
#define COMPILER_DEPENDENT_UINT64   unsigned long long

/* No system C library — ACPICA provides internal implementations */
/* Do NOT define ACPI_USE_SYSTEM_CLIBRARY or ACPI_USE_STANDARD_HEADERS */

/* Single-threaded — no preemption in kernel ACPI calls */
#define ACPI_SINGLE_THREADED

/* We provide the RSDP address ourselves */
#define ACPI_USE_NATIVE_RSDP_POINTER

/* Enable error messages for debugging, suppress verbose debug */
/* #define ACPI_NO_ERROR_MESSAGES */
#undef ACPI_DEBUG_OUTPUT

/* Disable features we don't need */
#undef ACPI_DEBUGGER
#undef ACPI_DISASSEMBLER
#undef ACPI_DBG_TRACK_ALLOCATIONS

/* Use ACPICA's local cache implementation */
#define ACPI_USE_LOCAL_CACHE

/* Flush CPU cache for sleep states */
#define ACPI_FLUSH_CPU_CACHE()  __asm__ __volatile__("wbinvd":::"memory")

/* va_list — GCC built-in, no header needed.
 * Only define if acgcc.h hasn't already provided them
 * (via ACPI_USE_BUILTIN_STDARG). */
#ifndef va_arg
typedef __builtin_va_list   va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#endif

/* NULL */
#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* __ACLIKEOS_H__ */
