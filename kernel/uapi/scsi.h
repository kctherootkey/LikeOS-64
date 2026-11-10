// LikeOS SCSI UAPI - command opcodes, status codes, sense keys, device types
//
// The constants of the SCSI command set that userspace pass-through tools
// (smartmontools, hdparm, sg3_utils-style programs, eject, the optical
// drive helpers) build their command descriptor blocks from, and that the
// kernel's SCSI/ATA translation (kernel/dev/ata/ata_sat.c) and the ATAPI
// driver answer.  Names and values follow the established <scsi/scsi.h>.
//
// Status codes come in two spellings: the SAM_STAT_* values are the status
// byte as SAM defines it (what sg_io_hdr.status holds); the short names
// (GOOD, CHECK_CONDITION ...) are the same values shifted right by one, the
// "masked status" of sg_io_hdr.masked_status.
//
// Also here: the SCSI_IOCTL_* requests a disk node answers (the bus/target
// identification of SCSI_IOCTL_GET_IDLUN in particular).
//
// Mirrored into the userspace libc include tree as <scsi/scsi.h>; keep both
// copies identical.
//
// Copyright (C) 2026 The LikeOS Project

#ifndef _KERNEL_UAPI_SCSI_H_
#define _KERNEL_UAPI_SCSI_H_

#ifdef __cplusplus
extern "C" {
#endif

// ---- Command opcodes -------------------------------------------------------
#define TEST_UNIT_READY 0x00
#define REZERO_UNIT 0x01
#define REQUEST_SENSE 0x03
#define FORMAT_UNIT 0x04
#define READ_BLOCK_LIMITS 0x05
#define REASSIGN_BLOCKS 0x07
#define INITIALIZE_ELEMENT_STATUS 0x07
#define READ_6 0x08
#define WRITE_6 0x0a
#define SEEK_6 0x0b
#define READ_REVERSE 0x0f
#define WRITE_FILEMARKS 0x10
#define SPACE 0x11
#define INQUIRY 0x12
#define RECOVER_BUFFERED_DATA 0x14
#define MODE_SELECT 0x15
#define RESERVE 0x16
#define RELEASE 0x17
#define COPY 0x18
#define ERASE 0x19
#define MODE_SENSE 0x1a
#define START_STOP 0x1b
#define RECEIVE_DIAGNOSTIC 0x1c
#define SEND_DIAGNOSTIC 0x1d
#define ALLOW_MEDIUM_REMOVAL 0x1e
#define SET_WINDOW 0x24
#define READ_CAPACITY 0x25
#define READ_10 0x28
#define WRITE_10 0x2a
#define SEEK_10 0x2b
#define POSITION_TO_ELEMENT 0x2b
#define WRITE_VERIFY 0x2e
#define VERIFY 0x2f
#define SEARCH_HIGH 0x30
#define SEARCH_EQUAL 0x31
#define SEARCH_LOW 0x32
#define SET_LIMITS 0x33
#define PRE_FETCH 0x34
#define READ_POSITION 0x34
#define SYNCHRONIZE_CACHE 0x35
#define LOCK_UNLOCK_CACHE 0x36
#define READ_DEFECT_DATA 0x37
#define MEDIUM_SCAN 0x38
#define COMPARE 0x39
#define COPY_VERIFY 0x3a
#define WRITE_BUFFER 0x3b
#define READ_BUFFER 0x3c
#define UPDATE_BLOCK 0x3d
#define READ_LONG 0x3e
#define WRITE_LONG 0x3f
#define CHANGE_DEFINITION 0x40
#define WRITE_SAME 0x41
#define UNMAP 0x42
#define READ_TOC 0x43
#define READ_HEADER 0x44
#define GET_CONFIGURATION 0x46
#define GET_EVENT_STATUS_NOTIFICATION 0x4a
#define LOG_SELECT 0x4c
#define LOG_SENSE 0x4d
#define READ_DISC_INFORMATION 0x51
#define XDWRITEREAD_10 0x53
#define MODE_SELECT_10 0x55
#define RESERVE_10 0x56
#define RELEASE_10 0x57
#define MODE_SENSE_10 0x5a
#define PERSISTENT_RESERVE_IN 0x5e
#define PERSISTENT_RESERVE_OUT 0x5f
#define VARIABLE_LENGTH_CMD 0x7f
#define REPORT_LUNS 0xa0
#define SECURITY_PROTOCOL_IN 0xa2
#define MAINTENANCE_IN 0xa3
#define MAINTENANCE_OUT 0xa4
#define MOVE_MEDIUM 0xa5
#define EXCHANGE_MEDIUM 0xa6
#define READ_12 0xa8
#define SERVICE_ACTION_OUT_12 0xa9
#define WRITE_12 0xaa
#define READ_MEDIA_SERIAL_NUMBER 0xab
#define SERVICE_ACTION_IN_12 0xab
#define GET_PERFORMANCE 0xac
#define WRITE_VERIFY_12 0xae
#define VERIFY_12 0xaf
#define SEARCH_HIGH_12 0xb0
#define SEARCH_EQUAL_12 0xb1
#define SEARCH_LOW_12 0xb2
#define SECURITY_PROTOCOL_OUT 0xb5
#define READ_ELEMENT_STATUS 0xb8
#define SEND_VOLUME_TAG 0xb6
#define SET_CD_SPEED 0xbb
#define MECHANISM_STATUS 0xbd
#define READ_CD 0xbe
#define WRITE_LONG_2 0xea
#define EXTENDED_COPY 0x83
#define RECEIVE_COPY_RESULTS 0x84
#define ACCESS_CONTROL_IN 0x86
#define ACCESS_CONTROL_OUT 0x87
#define READ_16 0x88
#define COMPARE_AND_WRITE 0x89
#define WRITE_16 0x8a
#define READ_ATTRIBUTE 0x8c
#define WRITE_ATTRIBUTE 0x8d
#define WRITE_VERIFY_16 0x8e
#define VERIFY_16 0x8f
#define SYNCHRONIZE_CACHE_16 0x91
#define WRITE_SAME_16 0x93
#define ZBC_OUT 0x94
#define ZBC_IN 0x95
#define SERVICE_ACTION_BIDIRECTIONAL 0x9d
#define SERVICE_ACTION_IN_16 0x9e
#define SERVICE_ACTION_OUT_16 0x9f
#define ATA_16 0x85 // ATA PASS-THROUGH (16)
#define ATA_12 0xa1 // ATA PASS-THROUGH (12)

// SERVICE ACTION IN (16) service actions
#define SAI_READ_CAPACITY_16 0x10
#define SAI_GET_LBA_STATUS 0x12
#define SAI_REPORT_REFERRALS 0x13
// MAINTENANCE IN service actions
#define MI_REPORT_IDENTIFYING_INFORMATION 0x05
#define MI_REPORT_TARGET_PGS 0x0a
#define MI_REPORT_ALIASES 0x0b
#define MI_REPORT_SUPPORTED_OPERATION_CODES 0x0c
#define MI_REPORT_SUPPORTED_TASK_MANAGEMENT_FUNCTIONS 0x0d
#define MI_REPORT_PRIORITY 0x0e
#define MI_REPORT_TIMESTAMP 0x0f

// ---- SAM status byte (sg_io_hdr.status) ---------------------------------------
#define SAM_STAT_GOOD 0x00
#define SAM_STAT_CHECK_CONDITION 0x02
#define SAM_STAT_CONDITION_MET 0x04
#define SAM_STAT_BUSY 0x08
#define SAM_STAT_INTERMEDIATE 0x10
#define SAM_STAT_INTERMEDIATE_CONDITION_MET 0x14
#define SAM_STAT_RESERVATION_CONFLICT 0x18
#define SAM_STAT_COMMAND_TERMINATED 0x22 // obsolete in SAM-3
#define SAM_STAT_TASK_SET_FULL 0x28
#define SAM_STAT_ACA_ACTIVE 0x30
#define SAM_STAT_TASK_ABORTED 0x40

// ---- Masked status (status >> 1; sg_io_hdr.masked_status) ---------------------
#define GOOD 0x00
#define CHECK_CONDITION 0x01
#define CONDITION_GOOD 0x02
#define BUSY 0x04
#define INTERMEDIATE_GOOD 0x08
#define INTERMEDIATE_C_GOOD 0x0a
#define RESERVATION_CONFLICT 0x0c
#define COMMAND_TERMINATED 0x11
#define QUEUE_FULL 0x14
#define STATUS_MASK 0x3e

// ---- Sense keys -------------------------------------------------------------
#define NO_SENSE 0x00
#define RECOVERED_ERROR 0x01
#define NOT_READY 0x02
#define MEDIUM_ERROR 0x03
#define HARDWARE_ERROR 0x04
#define ILLEGAL_REQUEST 0x05
#define UNIT_ATTENTION 0x06
#define DATA_PROTECT 0x07
#define BLANK_CHECK 0x08
#define COPY_ABORTED 0x0a
#define ABORTED_COMMAND 0x0b
#define VOLUME_OVERFLOW 0x0d
#define MISCOMPARE 0x0e
#define COMPLETED 0x0f

// ---- Peripheral device types (INQUIRY byte 0, bits 4:0) ---------------------
#define TYPE_DISK 0x00
#define TYPE_TAPE 0x01
#define TYPE_PRINTER 0x02
#define TYPE_PROCESSOR 0x03 // HP scanners use this
#define TYPE_WORM 0x04 // Treated as ROM by our system
#define TYPE_ROM 0x05
#define TYPE_SCANNER 0x06
#define TYPE_MOD 0x07 // Magneto-optical disk - treated as TYPE_DISK
#define TYPE_MEDIUM_CHANGER 0x08
#define TYPE_COMM 0x09 // Communications device
#define TYPE_RAID 0x0c
#define TYPE_ENCLOSURE 0x0d // Enclosure Services Device
#define TYPE_RBC 0x0e
#define TYPE_OSD 0x11
#define TYPE_ZBC 0x14
#define TYPE_WLUN 0x1e // well-known logical unit
#define TYPE_NO_LUN 0x7f

// ---- Message codes (historic; part of the established header) ---------------
#define COMMAND_COMPLETE 0x00
#define EXTENDED_MESSAGE 0x01
#define SAVE_POINTERS 0x02
#define RESTORE_POINTERS 0x03
#define DISCONNECT 0x04
#define INITIATOR_ERROR 0x05
#define ABORT 0x06
#define MESSAGE_REJECT 0x07
#define NOP 0x08
#define MSG_PARITY_ERROR 0x09
#define LINKED_CMD_COMPLETE 0x0a
#define LINKED_FLG_CMD_COMPLETE 0x0b
#define BUS_DEVICE_RESET 0x0c
#define INITIATE_RECOVERY 0x0f
#define RELEASE_RECOVERY 0x10
#define SIMPLE_QUEUE_TAG 0x20
#define HEAD_OF_QUEUE_TAG 0x21
#define ORDERED_QUEUE_TAG 0x22

// ---- The SCSI_IOCTL_* requests of a disk node ----------------------------------
#define SCSI_IOCTL_SEND_COMMAND 1 // obsolete; answered with EINVAL
#define SCSI_IOCTL_TEST_UNIT_READY 2
#define SCSI_IOCTL_BENCHMARK_COMMAND 3
#define SCSI_IOCTL_SYNC 4
#define SCSI_IOCTL_START_UNIT 5
#define SCSI_IOCTL_STOP_UNIT 6
#define SCSI_IOCTL_DOORLOCK 0x5380 // lock the eject mechanism
#define SCSI_IOCTL_DOORUNLOCK 0x5381 // unlock the mechanism
#define SCSI_IOCTL_GET_IDLUN 0x5382 // struct scsi_idlun *
#define SCSI_IOCTL_PROBE_HOST 0x5385 // in: buffer length, out: host name
#define SCSI_IOCTL_GET_BUS_NUMBER 0x5386 // int *: the host number
#define SCSI_IOCTL_GET_PCI 0x5387 // char[20]: PCI slot name of the host

// SCSI_IOCTL_GET_IDLUN.  dev_id packs (target) | (lun << 8) |
// (channel << 16) | (host_no << 24); host_unique_id is a number unique to
// the host adapter.  On a SATA disk: host_no is the ATA port number - 1,
// channel 0, target the port multiplier link (0 without one), lun 0.
struct scsi_idlun {
	unsigned int dev_id;
	unsigned int host_unique_id;
};

#ifdef __cplusplus
}
#endif

#endif // _KERNEL_UAPI_SCSI_H_
