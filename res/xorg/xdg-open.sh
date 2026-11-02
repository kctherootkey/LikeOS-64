#!/bin/sh
#
# xdg-open - open a file, folder or URL with the application the system
# associates with it.
#
# The association is the desktop's own (mimeapps.list and the installed
# .desktop files) as GIO reads it, so this is "gio open" under the name
# programs call: luakit's "show download" opens the folder in File Explorer,
# a PDF link opens in the PDF viewer, a mailto: link in the mail program.
#
# Copyright (C) 2026 The LikeOS Project

if [ $# -ne 1 ]; then
	echo "Usage: xdg-open { file | URL }" >&2
	exit 1
fi
case "$1" in
--help | -h)
	echo "Usage: xdg-open { file | URL }"
	exit 0
	;;
--version)
	echo "xdg-open (LikeOS)"
	exit 0
	;;
esac
exec gio open "$1"
