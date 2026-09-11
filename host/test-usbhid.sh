#!/bin/sh
# Build and run host/test-usbhid.c.  Run from the repository root.
#
# Exercises the USB HID mouse report descriptor parser and Report Protocol
# decoder from kernel/dev/hid/usb_hid.c against real-world descriptors.
# The code under test is cut out of the live kernel source at build time, so
# there is no copy to drift.  See the comment at the top of host/test-usbhid.c.
set -e

TMP=${TMPDIR:-/tmp}/likeos-usbhid-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# The two layout typedefs from the header ...
awk '/Report Protocol mouse layout \(parsed/{f=1} f{print} /} usbhid_mouse_layout_t;/{f=0}' \
    include/kernel/dev/hid/usb_hid.h > "$TMP/usbhid-types-under-test.h"
# ... and the parser + decoder from the driver, up to the first function that
# needs the xHCI stack (hid_mouse_fetch_layout).
awk '/^\/\/ HID Report Descriptor Parser \(mouse\)/{f=1} /^\/\/ Fetch and parse the mouse.s report descriptor/{f=0} f{print}' \
    kernel/dev/hid/usb_hid.c > "$TMP/usbhid-code-under-test.h"

cc -O1 -g -Wall -Wextra -Wno-unused-function -I "$TMP" \
   -o "$TMP/test" host/test-usbhid.c

timeout 30 "$TMP/test"
