# LikeOS installer -- the source files of likeos-installer, shared by the
# host build (Makefile.host) and the LikeOS build (user/bin/installer/installer.mk).
# Paths are relative to user/bin/installer/frontend; the including makefile
# sets INSTALLER_FE_DIR (this directory) and INSTALLER_FE_TOP (the tree).
#
# Copyright (C) 2026 The LikeOS Project

INSTALLER_FE_SRCS := \
	main.c app.c wizard.c layout.c sheet.c keyboard.c backend-bridge.c \
	plans.c autopilot.c \
	widgets/ring-spinner.c widgets/progress-ring.c widgets/choice-list.c \
	widgets/password-entry.c widgets/buttons.c widgets/notice.c \
	widgets/field.c \
	art/art-common.c art/art-world.c art/art-devices.c art/art-people.c \
	art/art-status.c art/icons.c \
	pages/page-common.c pages/password-pair.c \
	pages/page-splash.c pages/page-language.c pages/page-keyboard.c \
	pages/page-preparing.c pages/page-disk.c pages/page-region.c \
	pages/page-layout.c pages/page-hostname.c pages/page-root.c \
	pages/page-account.c pages/page-summary.c pages/page-installing.c \
	pages/page-finished.c pages/page-error.c

# The stand-in backend (--mock): part of every build, under its own names
# (mock/il-mock-names.h).
INSTALLER_FE_MOCK_SRCS := mock/il-mock.c

# libinstaller's function table: in a build that links libinstaller
# (compiled with -DINSTALLER_HAVE_REAL).
INSTALLER_FE_REAL_SRCS := backend-real.c

INSTALLER_FE_HDRS := $(addprefix $(INSTALLER_FE_DIR)/, \
	app.h wizard.h layout.h sheet.h keyboard.h backend-bridge.h \
	backend-ops.h backend-api.h autopilot.h mock/il-mock-names.h \
	widgets/widgets.h art/art.h art/art-private.h pages/pages.h \
	pages/password-pair.h) \
	$(INSTALLER_FE_TOP)/user/bin/installer/backend/include/installer.h

# What goes into the program's resources, and where it is found.
INSTALLER_FE_RES_DIRS := \
	--sourcedir=$(INSTALLER_FE_DIR)/style \
	--sourcedir=$(INSTALLER_FE_TOP)/res/xorg/icons/LikeOS/256x256/apps \
	--sourcedir=$(INSTALLER_FE_TOP)
INSTALLER_FE_RES_DEPS := $(INSTALLER_FE_DIR)/style/installer.gresource.xml \
	$(INSTALLER_FE_DIR)/style/installer.css $(INSTALLER_FE_DIR)/style/help.txt \
	$(INSTALLER_FE_TOP)/res/xorg/icons/LikeOS/256x256/apps/likeos-logo.png \
	$(INSTALLER_FE_TOP)/COPYING
