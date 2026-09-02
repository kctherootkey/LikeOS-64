/*
 * LikeOS-64 dlfcn.c - Dynamic linking API wrappers
 *
 * These thin wrappers call into ld-likeos.so's exported symbols.
 * When a dynamically linked program is loaded, the runtime linker
 * (ld-likeos.so) is already mapped into the process and its symbols
 * are available via the GOT/PLT.
 *
 * The _rtld_* functions are provided by ld-likeos.so with default
 * visibility, so they appear in the global symbol scope.
 */

#include <dlfcn.h>
#include <link.h>
#include <stddef.h>

/* Declarations of the runtime linker functions */
extern void *_rtld_dlopen(const char *filename, int flags);
extern void *_rtld_dlsym(void *handle, const char *symbol);
extern int _rtld_dlclose(void *handle);
extern char *_rtld_dlerror(void);
extern int _rtld_find_object(void *addr, struct dl_find_object *result);
extern int _rtld_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *),
			      void *data);
extern int _rtld_dladdr(const void *addr, const char **fname, void **fbase,
			const char **sname, void **saddr);

void *dlopen(const char *filename, int flags)
{
	return _rtld_dlopen(filename, flags);
}

void *dlsym(void *handle, const char *symbol)
{
	return _rtld_dlsym(handle, symbol);
}

int dlclose(void *handle)
{
	return _rtld_dlclose(handle);
}

char *dlerror(void)
{
	return _rtld_dlerror();
}

int dladdr(const void *addr, Dl_info *info)
{
	if (!info)
		return 0;
	info->dli_fname = NULL;
	info->dli_fbase = NULL;
	info->dli_sname = NULL;
	info->dli_saddr = NULL;
	/* Unlike the rest of this file, a failure here is not an error to
	 * report: dladdr() returns zero for an address that belongs to no
	 * object, and callers use it as the question rather than as an
	 * assertion.  dlerror() is deliberately left alone. */
	return _rtld_dladdr(addr, &info->dli_fname, &info->dli_fbase,
			    &info->dli_sname, &info->dli_saddr);
}

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size,
				    void *data),
		    void *data)
{
	return _rtld_iterate_phdr(callback, data);
}

int _dl_find_object(void *address, struct dl_find_object *result)
{
	return _rtld_find_object(address, result);
}

void *dlvsym(void *handle, const char *symbol, const char *version)
{
	(void)version;
	return dlsym(handle, symbol);
}
