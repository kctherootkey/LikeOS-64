// LikeOS -- UEFI runtime services.
//
// After ExitBootServices the firmware still offers a handful of services
// to the operating system: its variable store (boot entries, boot order,
// vendor settings), the real-time clock, reset.  This module calls the
// variable and time services so the boot entries can be managed from the
// running system (/sys/firmware/efi/efivars, kernel/fs/efivars.c).
//
// How a call is made (efi.c has the details):
//
//   * physical mode: SetVirtualAddressMap is never called, so every
//     pointer the firmware sees -- its own and ours -- is a physical
//     address.  A private page table maps every region the firmware
//     marked EFI_MEMORY_RUNTIME one-to-one (virtual == physical), plus a
//     bounce area and a stack of our own the same way, plus the kernel's
//     upper half so interrupts and exceptions still find their handlers;
//   * one call at a time (a sleeping mutex), with interrupts disabled on
//     the calling CPU while CR3 points at that table, and the FPU state
//     of the interrupted task saved around it (the firmware may use SSE);
//   * the firmware's UEFI x64 calling convention (ms_abi), through a small
//     assembly thunk that also moves to the private stack;
//   * an exception inside the firmware does not take the kernel down: the
//     handler (efi_rt_fault, called first thing from the exception path)
//     unwinds to the thunk, the call returns EFI_ABORTED and the runtime
//     services stay disabled for the rest of the boot.
//
// `efi=noruntime' on the kernel command line keeps them off.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef KERNEL_HAL_EFI_H
#define KERNEL_HAL_EFI_H

#include <kernel/uapi/types.h>
#include <kernel/mm/memory.h>

typedef uint64_t efi_status_t;

#define EFI_ERROR_BIT 0x8000000000000000ULL
#define EFI_ERR(n) (EFI_ERROR_BIT | (uint64_t)(n))
#define EFI_IS_ERROR(s) (((s) & EFI_ERROR_BIT) != 0)

#define EFI_SUCCESS 0ULL
#define EFI_LOAD_ERROR EFI_ERR(1)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_BAD_BUFFER_SIZE EFI_ERR(4)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_READY EFI_ERR(6)
#define EFI_DEVICE_ERROR EFI_ERR(7)
#define EFI_WRITE_PROTECTED EFI_ERR(8)
#define EFI_OUT_OF_RESOURCES EFI_ERR(9)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ACCESS_DENIED EFI_ERR(15)
#define EFI_TIMEOUT EFI_ERR(18)
#define EFI_ABORTED EFI_ERR(21)
#define EFI_SECURITY_VIOLATION EFI_ERR(26)

// A GUID in its EFI_GUID memory layout (Data1..Data3 little-endian).
typedef struct {
	uint8_t b[16];
} efi_guid_t;

// The global variable vendor GUID 8be4df61-93ca-11d2-aa0d-00e098032b8c.
#define EFI_GLOBAL_VARIABLE_GUID                                          \
	{                                                                 \
		{ 0x61, 0xdf, 0xe4, 0x8b, 0xca, 0x93, 0xd2, 0x11, 0xaa, 0x0d, \
		  0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }                    \
	}

// Variable attributes.
#define EFI_VARIABLE_NON_VOLATILE 0x00000001u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x00000002u
#define EFI_VARIABLE_RUNTIME_ACCESS 0x00000004u
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD 0x00000008u
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS 0x00000010u
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020u
#define EFI_VARIABLE_APPEND_WRITE 0x00000040u
#define EFI_VARIABLE_ENHANCED_AUTHENTICATED_ACCESS 0x00000080u
#define EFI_VARIABLE_ATTRIBUTES_MASK 0x000000ffu

// Bounds of what one call carries (the bounce area is sized by them).
#define EFI_VAR_NAME_MAX 512 // UCS-2 units of a variable name, NUL included
#define EFI_VAR_DATA_MAX (64 * 1024) // bytes of variable data written,
					// and read through the fixed bounce area
// The largest variable that can be read: up to EFI_VAR_DATA_MAX through the
// fixed bounce area, above it through a bounce region of the size asked for,
// mapped for the one call.
#define EFI_VAR_READ_MAX (1024 * 1024)

// EFI_TIME
struct efi_time {
	uint16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	uint8_t pad1;
	uint32_t nanosecond;
	int16_t time_zone;
	uint8_t daylight;
	uint8_t pad2;
};

// Called by kernel_main while the bootloader's boot_info is still mapped:
// keeps what efi_init() needs.
void efi_boot_info_save(const boot_info_t *bi);

// Set the runtime services up (page table, bounce area, a first call).
// After the heap, the per-CPU area and the exception handlers exist; may
// sleep.  Logs what it found; efi_runtime_available() says the outcome.
void efi_init(void);

// 1 while runtime services can be called (the bootloader passed them, no
// `efi=noruntime', the set-up and every call so far succeeded).
int efi_runtime_available(void);

// What the bootloader learned from the system table ("" / 0 when none).
const char *efi_fw_vendor(void);
uint32_t efi_fw_revision(void);
uint32_t efi_spec_revision(void);

// The firmware's status as a negative errno (0 for EFI_SUCCESS).
int efi_status_to_errno(efi_status_t s);

// The variable services.  Names are NUL-terminated UCS-2 in kernel memory
// (at most EFI_VAR_NAME_MAX units with the NUL); data written at most
// EFI_VAR_DATA_MAX bytes, read at most EFI_VAR_READ_MAX.  Sleep (one call
// at a time).  EFI_UNSUPPORTED when the runtime services are not available.
//
// get: *size is the buffer size in, the variable's size out (also on
// EFI_BUFFER_TOO_SMALL); data may be NULL with *size 0 to ask for the
// size; attr may be NULL.  A variable larger than EFI_VAR_READ_MAX reports
// EFI_BUFFER_TOO_SMALL with its size whatever the buffer;
// EFI_OUT_OF_RESOURCES when no memory could be found to bounce a large
// read through.
efi_status_t efi_get_variable(const uint16_t *name, const efi_guid_t *guid,
			      uint32_t *attr, uint64_t *size, void *data);
// next: name/guid in = the previous variable (name[0] == 0 starts),
// out = the next one; *name_size in/out bytes as the firmware defines it.
efi_status_t efi_get_next_variable_name(uint64_t *name_size, uint16_t *name,
					efi_guid_t *guid);
// set: size 0 deletes (unless attr has APPEND_WRITE).
efi_status_t efi_set_variable(const uint16_t *name, const efi_guid_t *guid,
			      uint32_t attr, uint64_t size, const void *data);
// EFI_UNSUPPORTED on a firmware older than UEFI 2.0.
efi_status_t efi_query_variable_info(uint32_t attr, uint64_t *max_storage,
				     uint64_t *remaining, uint64_t *max_size);
efi_status_t efi_get_time(struct efi_time *t);

// /sys/firmware/efi and its efivars directory (kernel/fs/efivars.c), when
// the runtime services work; after sysfs_init() and efi_init().
void efivars_init(void);

// From the exception handler, before anything else, for an exception taken
// in kernel mode: 1 when it happened inside a firmware call -- the frame's
// *rip/*rax/*rflags have then been rewritten so the return from the
// exception lands at the end of the call with EFI_ABORTED, and the runtime
// services are switched off; 0 otherwise (not ours, carry on as usual).
int efi_rt_fault(uint64_t vector, uint64_t error_code, uint64_t *rip,
		 uint64_t *rflags, uint64_t *rax);

#endif // KERNEL_HAL_EFI_H
