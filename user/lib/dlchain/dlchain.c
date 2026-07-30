/*
 * libdlchain.so - the upper half of the dlopen dependency-chain test.
 *
 * Links against libdlbase.so (DT_NEEDED), so dlopen("/lib/libdlchain.so")
 * has to load, relocate and initialise libdlbase.so as a side effect.  Every
 * function here reaches into that dependency, so if the loader skipped it the
 * calls return the wrong value or fault instead of quietly appearing to work.
 */

extern int dlbase_value(void);
extern int dlbase_magic;
extern int dlbase_ctor_ran;

/* Calls through the PLT into the dependency. */
int dlchain_call_dep(void)
{
	return dlbase_value();
}

/* Reads a data symbol from the dependency (GLOB_DAT rather than JUMP_SLOT). */
int dlchain_read_dep_data(void)
{
	return dlbase_magic;
}

/* Reports whether the dependency's constructor ran. */
int dlchain_dep_ctor_ran(void)
{
	return dlbase_ctor_ran;
}

int dlchain_ctor_ran = 0;

__attribute__((constructor)) static void dlchain_ctor(void)
{
	dlchain_ctor_ran = 1;
}

int dlchain_own_ctor_ran(void)
{
	return dlchain_ctor_ran;
}
