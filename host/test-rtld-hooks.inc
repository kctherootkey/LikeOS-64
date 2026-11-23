/* Appended to a copy of user/lib/rtld/rtld.c by host/test-rtld.sh: the
 * handful of entry points host/test-rtld.c drives the loader through, and a
 * stand-in for the assembly resolver the copy is built without.  Every
 * symbol of the copy except these hk_ ones is made local afterwards, so the
 * loader's own globals (_r_debug, __tls_get_addr, ...) cannot take the host
 * loader's place.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

void _dl_runtime_resolve(void)
{
	rtld_die("lazy binding is not used by the host test");
}

/* Pretend start-up is over: the TLS block exists (so dlopen does not lay
 * one out and load %fs), with `tls_limit' bytes of static space, and
 * libraries go into the range at `base', reserved by the caller. */
void hk_setup(uint64_t base, uint64_t tls_limit)
{
	g_tls_initialised = 1;
	g_tls_tp = NULL;
	g_tls_static_limit = tls_limit;
	g_lib_mmap_base = base;
}

void *hk_dlopen(const char *f)
{
	return _rtld_dlopen(f, 2);
}

void *hk_dlsym(void *h, const char *s)
{
	return _rtld_dlsym(h, s);
}

int hk_dlclose(void *h)
{
	return _rtld_dlclose(h);
}

char *hk_dlerror(void)
{
	return _rtld_dlerror();
}

/* Slots in use. */
int hk_nlive(void)
{
	int n = 0;

	for (int i = 0; i < g_ndsos; i++)
		if (g_dsos[i].name || g_dsos[i].symtab)
			n++;
	return n;
}

static int hk_contains(const char *s, const char *sub)
{
	if (!s)
		return 0;
	for (; *s; s++) {
		size_t k = 0;

		while (sub[k] && s[k] == sub[k])
			k++;
		if (!sub[k])
			return 1;
	}
	return 0;
}

/* Is any slot named, or loaded from a path, containing `sub'?  Also checks
 * that no slot carries the marks of a load in progress. */
int hk_has(const char *sub)
{
	for (int i = 0; i < g_ndsos; i++) {
		if (g_dsos[i].loading || g_dsos[i].load_refs)
			return 2;
		if (hk_contains(g_dsos[i].name, sub) ||
		    hk_contains(g_dsos[i].path, sub))
			return 1;
	}
	return 0;
}

int hk_refcount(void *h)
{
	return ((dso_t *)h)->refcount;
}

void hk_tls_state(int *modid, uint64_t *size)
{
	*modid = g_tls_next_modid;
	*size = g_tls_static_size;
}

unsigned long long hk_adds_minus_subs(void)
{
	return g_dl_adds - g_dl_subs;
}
