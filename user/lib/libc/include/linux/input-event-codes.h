/*
 * The event codes alone, at the include path programs use when they need
 * the names (EV_*, SYN_*, KEY_*, BTN_*, REL_*, ABS_*, MSC_*, SW_*, LED_*,
 * SND_*, REP_*, FF_*, INPUT_PROP_*, BUS_*, MT_TOOL_*) but not the ioctl
 * interface.
 *
 * On LikeOS the codes are part of <sys/input.h>, the copy of the kernel's
 * event-device interface (kernel/uapi/input.h); this header forwards to it.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_UAPI_INPUT_EVENT_CODES_H
#define _LIKEOS_UAPI_INPUT_EVENT_CODES_H

#include <sys/input.h>

#endif /* _LIKEOS_UAPI_INPUT_EVENT_CODES_H */
