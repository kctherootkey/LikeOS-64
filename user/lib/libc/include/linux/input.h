/*
 * The event-device interface at the include path input libraries use
 * (libevdev, mtdev, libinput, libmanette): struct input_event, struct
 * input_id, struct input_absinfo, the multitouch slot request, event masks,
 * force-feedback effects and every EVIOC* request.
 *
 * On LikeOS the interface lives in <sys/input.h> (a copy of the kernel's own
 * definition, kernel/uapi/input.h); this header forwards to it together with
 * the event codes and the fixed-width types, so both include paths give the
 * same structures and numbers.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#ifndef _LIKEOS_UAPI_INPUT_H
#define _LIKEOS_UAPI_INPUT_H

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/input.h>

#endif /* _LIKEOS_UAPI_INPUT_H */
