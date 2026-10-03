/* Host test prelude for synchronisation objects (host/test-drm-syncobj.sh):
 * the fence library's prelude, plus what drm_syncobj.c uses of the
 * scheduler header it replaces.
 *
 * Copyright (C) 2026 The LikeOS Project */
#ifndef HOST_TEST_DRM_SYNCOBJ_PRELUDE_H
#define HOST_TEST_DRM_SYNCOBJ_PRELUDE_H
#include "test-drm-fence-prelude.h"

static inline void task_set_fd_flags(task_t *t, unsigned fd, uint8_t flags)
{
	(void)t;
	(void)fd;
	(void)flags;
}

#endif
