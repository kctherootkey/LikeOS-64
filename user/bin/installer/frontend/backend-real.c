/* LikeOS installer -- libinstaller as the backend of the wizard.
 *
 * Compiled only into a build that links libinstaller (INSTALLER_HAVE_REAL;
 * see backend-ops.h): the function table of the real library.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include "installer.h"
#include "backend-ops.h"

const IlBackend il_backend_real = {
	.name = "libinstaller",
	.is_mock = 0,
	IL_BACKEND_FUNCTIONS,
};
