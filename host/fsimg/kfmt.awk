# Prints the kernel's own string formatter (ksnprintf and friends, kstr*)
# out of kernel/io/console.c, so the harness formats exactly as the kernel
# does.  Used by host/test-fs.sh.
#
# Copyright (C) 2026 The LikeOS Project
BEGIN { split("string_putchar kstrlen kstrcpy kstrncpy kstrncmp kstrcmp fmt_out fmt_pad fmt_integer kvprintf_to_buffer ksprintf kvsnprintf ksnprintf", a, " ");
  for (i in a) want[a[i]] = 1; st = 0 }
st == 2 { print; if ($0 ~ /^}/) { st = 0; print "" }; next }
st == 1 { hdr = hdr "\n" $0;
  if ($0 ~ /\);[ \t]*$/) { print hdr; st = 0; next }
  if ($0 ~ /\{[ \t]*$/ || $0 ~ /^\{/) { print hdr; st = 2; done[name] = 1 }
  next }
/^typedef struct \{$/ { tb = 1; tbuf = $0 "\n"; next }
tb { tbuf = tbuf $0 "\n"; if ($0 ~ /^}/) { tb = 0; if ($0 ~ /string_buffer_t;/) printf "%s\n", tbuf }; next }
/^#define FMT_/ { print; next }
/^enum fmt_len/ { st = 2; print; next }
/^[a-z].*\(/ {
  line = $0; sub(/\(.*/, "", line); n = split(line, w, /[ \t*]+/); name = w[n];
  if (!(name in want) || (name in done)) next;
  if ($0 ~ /\);[ \t]*$/) { print; next }
  hdr = $0;
  if ($0 ~ /\{[ \t]*$/) { print hdr; st = 2; done[name] = 1; next }
  st = 1; next }
