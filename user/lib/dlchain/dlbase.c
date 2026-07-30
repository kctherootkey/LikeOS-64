/*
 * libdlbase.so - the lower half of the dlopen dependency-chain test.
 *
 * Nothing links against this directly.  It exists so that libdlchain.so can
 * carry a DT_NEEDED on it, which is the situation that used to break: the
 * loader relocated only the object named in dlopen() and left anything pulled
 * in underneath with an unrelocated GOT.  Calling dlbase_value() through
 * libdlchain.so is what proves the dependency really was relocated.
 */

/* A global, so reaching it needs a relocated GOT entry rather than a
 * PC-relative access the linker could resolve at build time. */
int dlbase_magic = 0x5EED;

int dlbase_value(void)
{
	return dlbase_magic;
}

/* Set by the DT_INIT constructor below; libdlchain.so reports it so the test
 * can tell "relocated" from "relocated and initialised". */
int dlbase_ctor_ran = 0;

__attribute__((constructor)) static void dlbase_ctor(void)
{
	dlbase_ctor_ran = 1;
}
