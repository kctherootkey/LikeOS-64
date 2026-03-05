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
#include <stddef.h>

/* Declarations of the runtime linker functions */
extern void *_rtld_dlopen(const char *filename, int flags);
extern void *_rtld_dlsym(void *handle, const char *symbol);
extern int   _rtld_dlclose(void *handle);
extern char *_rtld_dlerror(void);

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
