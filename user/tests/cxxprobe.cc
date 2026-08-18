/*
 * cxxprobe -- what a C++ main executable needs from this system, one step at a
 * time.
 *
 * gdb is the first C++ *executable* ported here (the GTK programs are C, and
 * libstdc++ had only ever been exercised from shared libraries), and it dies at
 * start-up with RIP=0 -- a jump through a null pointer -- before printing
 * anything.  An 11 MB binary with 115 static constructors is a poor instrument
 * for finding out why, so this is the small one: it is built by the same
 * compiler wrapper, against the same sysroot, with the same default link that
 * gdb's configure produces, and it prints a marker before and after every step.
 * Whichever marker is missing names the feature that is broken.
 *
 * Deliberately uses write(2) rather than <iostream>: iostreams are themselves
 * set up by a static constructor in libstdc++, so printing through them cannot
 * report that constructors did not run.
 */
#include <chrono>
#include <string>
#include <vector>

#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static void say(const char *s)
{
	write(1, s, strlen(s));
}

/* A static constructor.  If .init_array is never walked, g_ctor_ran stays 0 and
 * main() says so -- which is the single most important thing to learn here,
 * because every uninitialised C++ global is a potential null call later. */
static int g_ctor_ran;

struct ctor_probe {
	ctor_probe() { g_ctor_ran = 1; }
};
static ctor_probe g_probe;

/* A global with a non-trivial constructor: this one has to be built by
 * .init_array too, and it allocates, so it also exercises operator new before
 * main(). */
static std::string g_greeting("constructed");

int main(void)
{
	char buf[128];

	say("cxxprobe: start\n");

	say(g_ctor_ran ? "  [ok]   static constructor ran\n"
		       : "  [FAIL] static constructor did NOT run\n");

	snprintf(buf, sizeof buf, "  %s   global std::string = \"%s\"\n",
		 g_greeting == "constructed" ? "[ok] " : "[FAIL]",
		 g_greeting.c_str());
	say(buf);

	say("  ... sbrk\n");
	void *brk0 = sbrk(0);
	snprintf(buf, sizeof buf, "  [ok]   sbrk(0) = %p\n", brk0);
	say(buf);

	say("  ... getrusage\n");
	struct rusage ru;
	int rc = getrusage(RUSAGE_SELF, &ru);
	snprintf(buf, sizeof buf, "  [ok]   getrusage = %d\n", rc);
	say(buf);

	/* The two libstdc++ calls gdb makes on this path, in the same order. */
	say("  ... steady_clock::now\n");
	auto t = std::chrono::steady_clock::now();
	snprintf(buf, sizeof buf, "  [ok]   steady_clock = %lld\n",
		 (long long)t.time_since_epoch().count());
	say(buf);

	say("  ... operator new / std::vector\n");
	std::vector<int> v;
	for (int i = 0; i < 1000; i++)
		v.push_back(i);
	snprintf(buf, sizeof buf, "  [ok]   vector[999] = %d\n", v[999]);
	say(buf);

	/* Exception unwinding: needs .eh_frame_hdr found through PT_GNU_EH_FRAME
	 * and libgcc's unwinder.  gdb throws in normal operation. */
	say("  ... throw / catch\n");
	try {
		throw std::string("thrown");
	} catch (const std::string &e) {
		snprintf(buf, sizeof buf, "  [ok]   caught \"%s\"\n",
			 e.c_str());
		say(buf);
	}

	say("cxxprobe: all steps passed\n");
	return 0;
}
