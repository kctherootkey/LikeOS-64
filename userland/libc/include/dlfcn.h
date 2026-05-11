/*
 * LikeOS-64 dlfcn.h - Dynamic linking interface
 *
 * Provides dlopen/dlsym/dlclose/dlerror for runtime loading
 * of shared libraries.  The actual work is done by ld-likeos.so;
 * these are thin wrappers that call into the runtime linker.
 */

#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

/* dlopen() mode flags */
#define RTLD_LAZY       0x0001  /* Lazy PLT binding */
#define RTLD_NOW        0x0002  /* Resolve all symbols immediately */
#define RTLD_GLOBAL     0x0100  /* Symbols available for subsequently loaded libs */
#define RTLD_LOCAL      0x0000  /* Symbols NOT available (default) */
#define RTLD_NOLOAD     0x0004  /* Don't load, just return handle if already loaded */
#define RTLD_NODELETE   0x1000  /* Don't unload on dlclose() */

/* Pseudo-handles for dlsym() */
#define RTLD_DEFAULT    ((void *)0)   /* Search default symbol scope */
#define RTLD_NEXT       ((void *)-1)  /* Search next object after caller */

/*
 * Open a shared library.
 * Returns a handle, or NULL on error (check dlerror()).
 */
void *dlopen(const char *filename, int flags);

/*
 * Look up a symbol in a shared library.
 * Returns the address of the symbol, or NULL on error (check dlerror()).
 */
void *dlsym(void *handle, const char *symbol);

/*
 * Close a shared library previously opened with dlopen().
 * Returns 0 on success, non-zero on error.
 */
int dlclose(void *handle);

/*
 * Return a human-readable string describing the most recent
 * dlopen/dlsym/dlclose error.  Returns NULL if no error occurred
 * since the last call to dlerror().
 */
char *dlerror(void);

/*
 * Translate an address to symbolic information.
 * Returns non-zero on success, 0 on failure.
 */
typedef struct {
    const char *dli_fname;  /* Pathname of shared object containing address */
    void       *dli_fbase;  /* Base address of shared object */
    const char *dli_sname;  /* Name of nearest symbol before address */
    void       *dli_saddr;  /* Address of nearest symbol */
} Dl_info;

int dladdr(const void *addr, Dl_info *info);

#ifdef __cplusplus
}
#endif

#endif /* _DLFCN_H */
