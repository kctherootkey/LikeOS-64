#!/bin/sh
# Fetch the current EasyList and EasyPrivacy into this directory.
#
# These are the filter lists luakit's ad blocker loads on the image (see the
# staging recipe in the top-level Makefile).  They are snapshots: EasyList
# marks itself as expiring after a few days, so run this before a release
# build, then rebuild the image.  The files are redistributed unchanged,
# under the terms in their own headers (https://easylist.to/pages/licence.html).
set -e
here=$(cd "$(dirname "$0")" && pwd)
for list in easylist easyprivacy; do
	curl -sSL --fail --max-time 120 \
		-o "$here/$list.txt.new" "https://easylist.to/easylist/$list.txt"
	# A list is a header line and thousands of rules; anything else is an
	# error page, and the previous snapshot stays.
	if [ "$(head -c 14 "$here/$list.txt.new")" != "[Adblock Plus " ]; then
		echo "$list: not a filter list, keeping the previous one" >&2
		rm -f "$here/$list.txt.new"
		exit 1
	fi
	mv "$here/$list.txt.new" "$here/$list.txt"
	echo "$list: $(sed -n 's/^! Version: //p' "$here/$list.txt" | head -1)"
done
