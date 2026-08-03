#!/bin/sh
# Build and run host/test-unicode.c.  Run from the repository root.
#
# The libc sources are compiled with -nostdinc against the LikeOS headers so
# they see their own declarations, and with every public name redefined to a
# likeos_* one so the result can be linked alongside the host C library and
# compared against it function by function.
set -e

SRC=user/lib/libc/src/locale
OUT=${TMPDIR:-/tmp}/likeos-unicode-test.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

REN="-Derrno=likeos_errno"
for f in mbrtowc wcrtomb mbtowc wctomb mbstowcs wcstombs mbsrtowcs wcsrtombs \
         mbsnrtowcs wcsnrtombs mbrlen mblen mbsinit btowc wctob \
         wcwidth wcswidth \
         iswalpha iswdigit iswalnum iswspace iswblank iswprint iswgraph \
         iswpunct iswupper iswlower iswcntrl iswxdigit \
         towupper towlower wctype iswctype wctrans towctrans; do
	REN="$REN -D$f=likeos_$f"
done

# errno is a plain int here and a TLS symbol in the host libc, so the renamed
# reference needs its own definition rather than resolving to the host's.
echo 'int likeos_errno;' > "$OUT/errno_stub.c"

for f in multibyte wcwidth wctype unicode; do
	cc -c -O1 -w -nostdinc -ffreestanding \
	   -Iuser/lib/libc/include -I"$SRC" $REN "$SRC/$f.c" -o "$OUT/$f.o"
done

cc -O1 -o "$OUT/test-unicode" host/test-unicode.c "$OUT/errno_stub.c" \
   "$OUT/multibyte.o" "$OUT/wcwidth.o" "$OUT/wctype.o" "$OUT/unicode.o"

"$OUT/test-unicode"
