#!/bin/sh
# Build and run host/test-search.c.  Run from the repository root.
#
# Checks libc's <search.h> -- the AVL tree behind tsearch above all -- against
# the host glibc and against the invariants a tree has to hold.  See the
# comment at the top of host/test-search.c.
set -e

TMP=${TMPDIR:-/tmp}/likeos-search-test.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# search.c defines tsearch, hsearch and the rest under the names glibc uses, so
# it is compiled on its own and objcopy gives every symbol it defines or calls
# an lk_ prefix.  It calls malloc, free, strcmp and memcpy, which are renamed
# with it -- so those are supplied back below, mapped onto the host's.
#
# -fno-stack-protector: the host enables the protector by default, and the
# renaming would turn its __stack_chk_fail into a symbol nothing defines.
cc -O1 -g -fno-stack-protector -Wall -Wextra \
   -I user/lib/libc/include \
   -c user/lib/libc/src/stdlib/search.c -o "$TMP/search.o"
objcopy --prefix-symbols=lk_ "$TMP/search.o" "$TMP/search-renamed.o"

# The libc routines search.c calls, renamed along with everything else and so
# now undefined.  Forwarded to the host's, which is what the test wants: the
# code under test is the search tables, not the allocator.
cat > "$TMP/shims.c" <<'SHIM'
#include <stdlib.h>
#include <string.h>
/* errno is thread-local here and reached through __errno_location(). */
static int lk_errno_storage;
int *lk___errno_location(void) { return &lk_errno_storage; }
void *lk_malloc(size_t n) { return malloc(n); }
void *lk_calloc(size_t n, size_t s) { return calloc(n, s); }
void lk_free(void *p) { free(p); }
int lk_strcmp(const char *a, const char *b) { return strcmp(a, b); }
void *lk_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
SHIM

cc -O1 -g -Wall -Wextra -o "$TMP/test-search" \
   host/test-search.c "$TMP/search-renamed.o" "$TMP/shims.c"

"$TMP/test-search"
