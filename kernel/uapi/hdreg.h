// LikeOS ATA disk UAPI - the HDIO_* ioctls and the identify block layout
//
// The established hard-disk ioctl interface, as hdparm, smartmontools,
// udev-style identity probes and partitioning tools use it on an ATA disk
// node (/dev/sda).  Request numbers are the classic plain 0x03xx values,
// not _IOC-encoded; argument layouts are fixed by the interface.
//
// What LikeOS answers on a SATA disk driven by the AHCI driver:
//
//   HDIO_GETGEO        struct hd_geometry *  a CHS geometry (the drive's
//                      default CHS when it reports one, else 255 heads x 63
//                      sectors) and the device's start sector
//   HDIO_GET_IDENTITY  unsigned char[512]    the IDENTIFY (PACKET) DEVICE
//                      data exactly as the drive returned it (little-endian
//                      16-bit words, ATA string byte order untouched)
//   HDIO_GET_32BIT     long *                I/O support: always 0 on SATA
//   HDIO_GET_MULTCOUNT long *                current READ/WRITE MULTIPLE size
//   HDIO_GET_DMA       long *                1 (AHCI is always DMA)
//   HDIO_GET_WCACHE    long *                write cache enabled
//   HDIO_GET_ACOUSTIC  long *                AAM level, 0 when unsupported
//   HDIO_GET_KEEPSETTINGS, HDIO_GET_NOWERR, HDIO_GET_UNMASKINTR  long *
//   HDIO_GET_BUSSTATE  long *                BUSSTATE_ON
//   HDIO_SET_WCACHE    value                 SET FEATURES 02h/82h
//   HDIO_SET_ACOUSTIC  value                 SET FEATURES 42h/C2h
//   HDIO_SET_MULTCOUNT value                 SET MULTIPLE MODE
//   HDIO_DRIVE_CMD     unsigned char args[4 + 512*n], see below
//   HDIO_DRIVE_TASK    unsigned char args[7], see below
//   HDIO_DRIVE_TASKFILE struct ide_task_request_s + data, see below
//   HDIO_DRIVE_RESET   -                     device reset through EH
// Everything that only made sense for parallel IDE (HDIO_SET_PIO_MODE,
// HDIO_SCAN_HWIF, HDIO_SET_32BIT with a value other than 0, ...) answers
// EINVAL; unknown requests ENOTTY.  All of them require CAP_SYS_ADMIN
// (root) except the GET requests and HDIO_GETGEO.
//
// HDIO_DRIVE_CMD, args[0..3] in:
//   [0] command  [1] sector number (for SMART: LBA low; otherwise the sector
//   count)  [2] feature  [3] number of 512-byte data sectors the command
//   returns (0 = no data).  For SMART (0xB0) the LBA mid/high signature
//   (0x4F/0xC2) is supplied by the kernel and args[3] is also the count.
//   Out: [0] status  [1] error  [2] sector count register; the data (if
//   args[3] != 0) follows the 4 header bytes.
//
// HDIO_DRIVE_TASK, args[0..6] in:
//   [0] command  [1] feature  [2] sector count  [3] LBA low (sector number)
//   [4] LBA mid (cylinder low)  [5] LBA high (cylinder high)  [6] device.
//   No data phase.  Out: [0] status  [1] error  [2..6] the registers as the
//   drive left them.
//
// Mirrored into the userspace libc include tree as <sys/hdreg.h>; keep both
// copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_HDREG_H_
#define _KERNEL_UAPI_HDREG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __LIKEOS_KERNEL__
#include <kernel/uapi/types.h>
#else
#include <stdint.h>
#endif

// ---- The GET requests -------------------------------------------------------
#define HDIO_GETGEO 0x0301 // get device geometry (struct hd_geometry)
#define HDIO_GET_UNMASKINTR 0x0302 // get current unmask setting
#define HDIO_GET_MULTCOUNT 0x0304 // get current IDE blockmode setting
#define HDIO_GET_QDMA 0x0305 // get use-qdma flag
#define HDIO_SET_XFER 0x0306 // set transfer rate via proc
#define HDIO_OBSOLETE_IDENTITY 0x0307 // old 142-byte identity (EINVAL)
#define HDIO_GET_KEEPSETTINGS 0x0308 // get keep-settings-on-reset flag
#define HDIO_GET_32BIT 0x0309 // get current io_32bit setting
#define HDIO_GET_NOWERR 0x030a // get ignore-write-error flag
#define HDIO_GET_DMA 0x030b // get use-dma flag
#define HDIO_GET_NICE 0x030c // get nice flags
#define HDIO_GET_IDENTITY 0x030d // get IDE identification info (512 bytes)
#define HDIO_GET_WCACHE 0x030e // get write cache mode on|off
#define HDIO_GET_ACOUSTIC 0x030f // get acoustic value
#define HDIO_GET_ADDRESS 0x0310 // get CHS / LBA28 / LBA48 addressing mode
#define HDIO_GET_BUSSTATE 0x031a // get the bus state of the hwif
#define HDIO_TRISTATE_HWIF 0x031b // execute a channel tristate
#define HDIO_DRIVE_RESET 0x031c // execute a device reset
#define HDIO_DRIVE_TASKFILE 0x031d // execute raw taskfile
#define HDIO_DRIVE_TASK 0x031e // execute task and special drive command
#define HDIO_DRIVE_CMD 0x031f // execute a special drive command
#define HDIO_DRIVE_CMD_AEB HDIO_DRIVE_TASK

// ---- The SET requests (argument passed as the value) ----------------------
#define HDIO_SET_MULTCOUNT 0x0321 // change IDE blockmode
#define HDIO_SET_UNMASKINTR 0x0322 // permit other irqs during I/O
#define HDIO_SET_KEEPSETTINGS 0x0323 // keep ioctl settings on reset
#define HDIO_SET_32BIT 0x0324 // change io_32bit flags
#define HDIO_SET_NOWERR 0x0325 // change ignore-write-error flag
#define HDIO_SET_DMA 0x0326 // change use-dma flag
#define HDIO_SET_PIO_MODE 0x0327 // reconfig interface to new speed
#define HDIO_SCAN_HWIF 0x0328 // register and (re)scan interface
#define HDIO_SET_NICE 0x0329 // set nice flags
#define HDIO_UNREGISTER_HWIF 0x032a // unregister interface
#define HDIO_SET_WCACHE 0x032b // change write cache enable-disable
#define HDIO_SET_ACOUSTIC 0x032c // change acoustic behavior
#define HDIO_SET_BUSSTATE 0x032d // set the bus state of the hwif
#define HDIO_SET_QDMA 0x032e // change use-qdma flag
#define HDIO_SET_ADDRESS 0x032f // change lba addressing modes

// HDIO_GET_BUSSTATE / HDIO_SET_BUSSTATE values
#define BUSSTATE_OFF 0
#define BUSSTATE_ON 1
#define BUSSTATE_TRISTATE 2

// HDIO_GET_ADDRESS values
#define HDIO_ADDRESS_CHS 0
#define HDIO_ADDRESS_LBA28 1
#define HDIO_ADDRESS_LBA48 2

// Sizes of the HDIO_DRIVE_CMD / HDIO_DRIVE_TASK headers
#define HDIO_DRIVE_CMD_HDR_SIZE (4 * sizeof(unsigned char))
#define HDIO_DRIVE_HOB_HDR_SIZE (8 * sizeof(unsigned char))
#define HDIO_DRIVE_TASK_HDR_SIZE (8 * sizeof(unsigned char))

// ---- HDIO_GETGEO ------------------------------------------------------------
struct hd_geometry {
	unsigned char heads;
	unsigned char sectors;
	unsigned short cylinders; // truncated to 65535 on large disks
	unsigned long start; // first sector of the device (a partition's offset)
};

// ---- HDIO_DRIVE_TASKFILE ----------------------------------------------------
//
// The argument is an ide_task_request_t followed by out_size bytes of data
// to write and room for in_size bytes to read.  io_ports[] holds the
// register file in the classic order (data, feature/error, nsector, sector,
// lcyl, hcyl, select, command/status); hob_ports[] the high-order bytes of
// a 48-bit command.  out_flags/in_flags say which registers are valid.

// io_ports[] / hob_ports[] indices
#define IDE_DATA_OFFSET 0
#define IDE_ERROR_OFFSET 1
#define IDE_FEATURE_OFFSET 1
#define IDE_NSECTOR_OFFSET 2
#define IDE_SECTOR_OFFSET 3
#define IDE_LCYL_OFFSET 4
#define IDE_HCYL_OFFSET 5
#define IDE_SELECT_OFFSET 6
#define IDE_STATUS_OFFSET 7
#define IDE_COMMAND_OFFSET 7
#define IDE_CONTROL_OFFSET_HOB 7 // hob_ports[7]: the device control byte

// The standard "these registers are valid" masks for out_flags/in_flags
#define IDE_TASKFILE_STD_IN_FLAGS 0xFE
#define IDE_HOB_STD_IN_FLAGS 0x3C
#define IDE_TASKFILE_STD_OUT_FLAGS 0xFE
#define IDE_HOB_STD_OUT_FLAGS 0x3C

typedef unsigned char task_ioreg_t;
typedef unsigned long sata_ioreg_t;

typedef union ide_reg_valid_s {
	unsigned all : 16;
	struct {
		unsigned data : 1;
		unsigned error_feature : 1;
		unsigned sector : 1;
		unsigned nsector : 1;
		unsigned lcyl : 1;
		unsigned hcyl : 1;
		unsigned select : 1;
		unsigned status_command : 1;

		unsigned data_hob : 1;
		unsigned error_feature_hob : 1;
		unsigned sector_hob : 1;
		unsigned nsector_hob : 1;
		unsigned lcyl_hob : 1;
		unsigned hcyl_hob : 1;
		unsigned select_hob : 1;
		unsigned control_hob : 1;
	} b;
} ide_reg_valid_t;

typedef struct ide_task_request_s {
	unsigned char io_ports[8];
	unsigned char hob_ports[8]; // bytes 6 and 7 are unused
	ide_reg_valid_t out_flags;
	ide_reg_valid_t in_flags;
	int data_phase; // TASKFILE_*
	int req_cmd; // IDE_DRIVE_TASK_*
	unsigned long out_size; // bytes of data to the drive
	unsigned long in_size; // bytes of data from the drive
} ide_task_request_t;

// ide_task_request_t.req_cmd
#define IDE_DRIVE_TASK_NO_DATA 0
#define IDE_DRIVE_TASK_INVALID -1
#define IDE_DRIVE_TASK_SET_XFER 1
#define IDE_DRIVE_TASK_IN 2
#define IDE_DRIVE_TASK_OUT 3
#define IDE_DRIVE_TASK_RAW_WRITE 4

// ide_task_request_t.data_phase
#define TASKFILE_NO_DATA 0x0000
#define TASKFILE_IN 0x0001
#define TASKFILE_MULTI_IN 0x0002
#define TASKFILE_OUT 0x0004
#define TASKFILE_MULTI_OUT 0x0008
#define TASKFILE_IN_OUT 0x0010
#define TASKFILE_IN_DMA 0x0020
#define TASKFILE_OUT_DMA 0x0040
#define TASKFILE_IN_DMAQ 0x0080
#define TASKFILE_OUT_DMAQ 0x0100
#define TASKFILE_P_IN 0x0200
#define TASKFILE_P_OUT 0x0400
#define TASKFILE_P_IN_DMA 0x0800
#define TASKFILE_P_OUT_DMA 0x1000
#define TASKFILE_P_IN_DMAQ 0x2000
#define TASKFILE_P_OUT_DMAQ 0x4000
#define TASKFILE_48 0x8000
#define TASKFILE_INVALID 0x7fff

// ---- HDIO_GET_IDENTITY: the identify block as named fields -------------------
//
// The same 512 bytes as the IDENTIFY DEVICE data, word N at byte 2*N.  Only
// the fields of the classic layout are named; newer words live in the
// reserved arrays (use the word indices of the ATA command set for those).
struct hd_driveid {
	unsigned short config; // 0: lots of obsolete bit flags
	unsigned short cyls; // 1: obsolete "physical" cyls
	unsigned short reserved2; // 2: reserved (word 2)
	unsigned short heads; // 3: obsolete "physical" heads
	unsigned short track_bytes; // 4: unformatted bytes per track
	unsigned short sector_bytes; // 5: unformatted bytes per sector
	unsigned short sectors; // 6: obsolete "physical" sectors per track
	unsigned short vendor0; // 7: vendor unique
	unsigned short vendor1; // 8: vendor unique
	unsigned short vendor2; // 9: retired vendor unique
	unsigned char serial_no[20]; // 10-19: 0 = not_specified
	unsigned short buf_type; // 20: retired
	unsigned short buf_size; // 21: retired, 512 byte increments
	unsigned short ecc_bytes; // 22: for r/w long cmds; 0 = not_specified
	unsigned char fw_rev[8]; // 23-26: 0 = not_specified
	unsigned char model[40]; // 27-46: 0 = not_specified
	unsigned char max_multsect; // 47: 0=not_implemented
	unsigned char vendor3; // 47: vendor unique
	unsigned short dword_io; // 48: 0=not_implemented; 1=implemented
	unsigned char vendor4; // 49: vendor unique
	unsigned char capability; // 49: (upper byte) bit0 DMA, bit1 LBA,
				  //     bit2 IORDY disable, bit3 IORDY
	unsigned short reserved50; // 50: reserved
	unsigned char vendor5; // 51: vendor unique
	unsigned char tPIO; // 51: 0=slow, 1=medium, 2=fast
	unsigned char vendor6; // 52: vendor unique
	unsigned char tDMA; // 52: 0=slow, 1=medium, 2=fast
	unsigned short field_valid; // 53: bit0 cur_ fields, bit1 eide_ fields
	unsigned short cur_cyls; // 54: logical cylinders
	unsigned short cur_heads; // 55: logical heads
	unsigned short cur_sectors; // 56: logical sectors per track
	unsigned short cur_capacity0; // 57: logical total sectors on drive
	unsigned short cur_capacity1; // 58:  (2 words, misaligned int)
	unsigned char multsect; // 59: current multiple sector count
	unsigned char multsect_valid; // 59: when (bit0==1) multsect is ok
	unsigned int lba_capacity; // 60-61: total number of sectors (LBA28)
	unsigned short dma_1word; // 62: single-word dma info
	unsigned short dma_mword; // 63: multiple-word dma info
	unsigned short eide_pio_modes; // 64: bits 0:mode3 1:mode4
	unsigned short eide_dma_min; // 65: min mword dma cycle time (ns)
	unsigned short eide_dma_time; // 66: recommended mword dma cycle time
	unsigned short eide_pio; // 67: min cycle time (ns), no IORDY
	unsigned short eide_pio_iordy; // 68: min cycle time (ns), with IORDY
	unsigned short words69_70[2]; // 69-70: reserved / additional support
	unsigned short words71_74[4]; // 71-74: reserved for IDENTIFY PACKET
	unsigned short queue_depth; // 75: bits 4:0 maximum queue depth - 1
	unsigned short words76_79[4]; // 76-79: SATA capabilities/features
	unsigned short major_rev_num; // 80: major version number
	unsigned short minor_rev_num; // 81: minor version number
	unsigned short command_set_1; // 82: command/feature sets supported
	unsigned short command_set_2; // 83: command/feature sets supported
	unsigned short cfsse; // 84: command/feature set supported extension
	unsigned short cfs_enable_1; // 85: command/feature sets enabled
	unsigned short cfs_enable_2; // 86: command/feature sets enabled
	unsigned short csf_default; // 87: command/feature set default
	unsigned short dma_ultra; // 88: ultra DMA supported/selected
	unsigned short trseuc; // 89: time required for security erase
	unsigned short trsEuc; // 90: time required for enhanced erase
	unsigned short CurAPMvalues; // 91: current advanced power management
	unsigned short mprc; // 92: master password revision code
	unsigned short hw_config; // 93: hardware config (cable detection)
	unsigned short acoustic; // 94: automatic acoustic management
	unsigned short msrqs; // 95: stream minimum request size
	unsigned short sxfert; // 96: stream transfer time
	unsigned short sal; // 97: stream access latency
	unsigned int spg; // 98-99: stream performance granularity
	unsigned long long lba_capacity_2; // 100-103: 48-bit total sectors
	unsigned short words104_125[22]; // 104-125: reserved
	unsigned short last_lun; // 126: last logical unit number
	unsigned short word127; // 127: removable media status notification
	unsigned short dlf; // 128: device lock function (security status)
	unsigned short csfo; // 129: current set features options
	unsigned short words130_155[26]; // 130-155: reserved vendor words
	unsigned short word156; // 156: reserved vendor word
	unsigned short words157_159[3]; // 157-159: reserved vendor words
	unsigned short cfa_power; // 160: CFA power mode 1
	unsigned short words161_175[15]; // 161-175: reserved for CFA
	unsigned short words176_205[30]; // 176-205: current media serial number
	unsigned short words206_254[49]; // 206-254: reserved
	unsigned short integrity_word; // 255: checksum (high) and 0xA5 (low)
};

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_HDREG_H_
