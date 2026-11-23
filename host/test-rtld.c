/* Host test for the dynamic loader's dlopen failure paths; built and run by
 * host/test-rtld.sh, which passes the scratch directory holding the test
 * libraries:
 *
 *   liba.so -> libb.so -> libgone.so   (libgone.so.keep: the missing one)
 *   libok.so, libtls.so (initial-exec TLS), libtrunc.so (cut short),
 *   libtext.so (not an object at all)
 *
 * A failed dlopen must leave nothing behind -- no slot, no mapping, no TLS
 * -- so that the next dlopen of the same name tries again from scratch.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void hk_setup(uint64_t base, uint64_t tls_limit);
void *hk_dlopen(const char *f);
void *hk_dlsym(void *h, const char *s);
int hk_dlclose(void *h);
char *hk_dlerror(void);
int hk_nlive(void);
int hk_has(const char *sub);
int hk_refcount(void *h);
void hk_tls_state(int *modid, uint64_t *size);
unsigned long long hk_adds_minus_subs(void);

static int g_fail;
static const char *g_dir;

static void check(const char *what, int ok)
{
	printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
	if (!ok)
		g_fail++;
}

/* Is anything mapped from a file whose path contains `sub'? */
static int mapped(const char *sub)
{
	char line[4096];
	FILE *f = fopen("/proc/self/maps", "r");
	int hit = 0;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, g_dir) && strstr(line, sub))
			hit = 1;
	fclose(f);
	return hit;
}

static char *path(const char *name)
{
	static char buf[8][4096];
	static int n;
	char *b = buf[n++ & 7];

	snprintf(b, 4096, "%s/%s", g_dir, name);
	return b;
}

static int copy(const char *from, const char *to)
{
	char cmd[9000];

	snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", from, to);
	return system(cmd);
}

/* dlopen `name' expecting failure, and check that nothing stayed. */
static void expect_fail(const char *label, const char *name,
			const char *err_has, const char *err_has2)
{
	int live = hk_nlive();
	unsigned long long net = hk_adds_minus_subs();
	int modid;
	uint64_t tls;
	char what[512];

	hk_tls_state(&modid, &tls);
	void *h = hk_dlopen(path(name));
	const char *e = hk_dlerror();

	printf("  [%s] dlerror: %s\n", label, e ? e : "(none)");
	snprintf(what, sizeof(what), "%s: dlopen fails", label);
	check(what, h == NULL);
	snprintf(what, sizeof(what), "%s: dlerror names %s", label, err_has);
	check(what, e && strstr(e, err_has) && strstr(e, path(name)));
	if (err_has2) {
		snprintf(what, sizeof(what), "%s: dlerror says %s", label,
			 err_has2);
		check(what, e && strstr(e, err_has2));
	}
	snprintf(what, sizeof(what), "%s: no slot left behind", label);
	check(what, hk_nlive() == live && hk_has("liba.so") == 0 &&
			    hk_has("libb.so") == 0 && hk_has(name) == 0);
	snprintf(what, sizeof(what), "%s: adds and removals balance", label);
	check(what, hk_adds_minus_subs() == net);
	snprintf(what, sizeof(what), "%s: nothing left mapped", label);
	check(what, mapped("/liba.so") == 0 && mapped("/libb.so") == 0 &&
			    mapped(name) == 0);
	int modid2;
	uint64_t tls2;
	hk_tls_state(&modid2, &tls2);
	snprintf(what, sizeof(what), "%s: TLS ids and space given back", label);
	check(what, modid2 == modid && tls2 == tls);
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	g_dir = argv[1];

	/* Room for every library the test loads, so the loader's fixed
	 * placement lands nowhere the host put anything. */
	void *area = mmap(NULL, 64 << 20, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (area == MAP_FAILED)
		return 2;
	munmap(area, 64 << 20);
	hk_setup((uint64_t)(uintptr_t)area, 64);

	printf("missing dependency two levels down (liba -> libb -> libgone)\n");
	expect_fail("tree 1", "liba.so", "libgone.so", "no such file");
	expect_fail("tree 2", "liba.so", "libgone.so", "no such file");
	printf("the dependency in the middle on its own\n");
	expect_fail("mid", "libb.so", "libgone.so", "no such file");

	printf("other failures after the slot is taken\n");
	expect_fail("truncated", "libtrunc.so", "libtrunc.so", "cannot read");
	expect_fail("truncated again", "libtrunc.so", "libtrunc.so",
		    "cannot read");
	expect_fail("not an object", "libtext.so", "libtext.so",
		    "not a valid shared object");
	expect_fail("missing", "libnothere.so", "libnothere.so",
		    "no such file");
	expect_fail("static TLS", "libtls.so", "libtls.so", "static TLS");
	expect_fail("static TLS again", "libtls.so", "libtls.so", "static TLS");

	printf("a good library still loads\n");
	{
		void *h = hk_dlopen(path("libok.so"));
		int (*f)(void) = h ? (int (*)(void))hk_dlsym(h, "ok_value") :
				     NULL;
		check("libok.so loads", h != NULL);
		check("and works", f && f() == 42);
		check("refcount 1", h && hk_refcount(h) == 1);
		if (h)
			hk_dlclose(h);
		check("and closes", mapped("libok.so") == 0);
	}

	printf("the missing dependency appears: the same name now loads\n");
	if (copy(path("libgone.so.keep"), path("libgone.so")) != 0) {
		check("copy libgone.so", 0);
	} else {
		void *h = hk_dlopen(path("liba.so"));
		int (*f)(void) = h ? (int (*)(void))hk_dlsym(h, "a_value") :
				     NULL;
		if (!h)
			printf("  dlerror: %s\n", hk_dlerror());
		check("liba.so loads", h != NULL);
		check("and calls through both dependencies", f && f() == 80);
		check("no load marks left", hk_has("no-such-name") == 0);
		void *h2 = hk_dlopen(path("liba.so"));
		check("a second dlopen finds the same object", h2 == h);
		if (h2)
			hk_dlclose(h2);
		if (h)
			hk_dlclose(h);
	}

	printf(g_fail ? "FAILED: %d\n" : "all passed\n", g_fail);
	return g_fail ? 1 : 0;
}
