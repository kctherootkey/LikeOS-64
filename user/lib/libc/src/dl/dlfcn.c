/*
 * LikeOS dlfcn.c - Dynamic linking API wrappers
 *
 * These thin wrappers call into ld-likeos.so's exported symbols.
 * When a dynamically linked program is loaded, the runtime linker
 * (ld-likeos.so) is already mapped into the process and its symbols
 * are available via the GOT/PLT.
 *
 * The _rtld_* functions are provided by ld-likeos.so with default
 * visibility, so they appear in the global symbol scope.
 *
 * Copyright (C) 2026 The LikeOS Project
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
extern void _rtld_lock(void);
extern void _rtld_unlock(void);

/* dlerror() is per thread, as in every other implementation: the message
 * describes THIS thread's last dlopen/dlsym/dlclose.  The loader keeps one
 * process-wide buffer that every call clears on entry, so the wrappers below
 * hold the loader lock around the call and the fetch of its outcome, and
 * keep the result here.  Without this a thread whose dlsym had just failed
 * could see dlerror() return NULL because another thread resolved a symbol
 * in between -- and GLib's g_module_symbol() reads "NULL symbol, no error"
 * as success, which is how GStreamer's start-up came to call a null
 * function pointer in the web process. */
#define DL_ERROR_MAX 256
static __thread char dl_error_buf[DL_ERROR_MAX];
static __thread int dl_error_set;

static void dl_capture_error(void)
{
	const char *m = _rtld_dlerror();
	size_t i = 0;

	if (!m) {
		dl_error_set = 0;
		return;
	}
	while (m[i] && i < DL_ERROR_MAX - 1) {
		dl_error_buf[i] = m[i];
		i++;
	}
	dl_error_buf[i] = '\0';
	dl_error_set = 1;
}

void *dlopen(const char *filename, int flags)
{
	_rtld_lock();
	void *r = _rtld_dlopen(filename, flags);
	dl_capture_error();
	_rtld_unlock();
	return r;
}

void *dlsym(void *handle, const char *symbol)
{
	_rtld_lock();
	void *r = _rtld_dlsym(handle, symbol);
	dl_capture_error();
	_rtld_unlock();
	return r;
}

int dlclose(void *handle)
{
	_rtld_lock();
	int r = _rtld_dlclose(handle);
	dl_capture_error();
	_rtld_unlock();
	return r;
}

/* This thread's last message, once: the next call answers NULL, as the
 * interface specifies. */
char *dlerror(void)
{
	if (!dl_error_set)
		return NULL;
	dl_error_set = 0;
	return dl_error_buf;
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
