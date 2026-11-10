/*
 * scsi/scsi_ioctl.h - the SCSI_IOCTL_* requests of disk and optical nodes.
 *
 * The established interface keeps these in a header of their own; LikeOS
 * defines them, with struct scsi_idlun, in <scsi/scsi.h> (mirrored from
 * <kernel/uapi/scsi.h>), and this header only forwards there so programs
 * that include it by this name compile unchanged.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _SCSI_SCSI_IOCTL_H
#define _SCSI_SCSI_IOCTL_H

#include <scsi/scsi.h>

#endif
