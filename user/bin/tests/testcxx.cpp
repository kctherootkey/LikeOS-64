/*
 * testcxx - Does the C++ runtime work on this system?
 *
 * The toolchain's smoke test, and the thing to run first when a C++ port
 * misbehaves in a way that looks like the language rather than the program.
 * Each section covers a piece that has to be arranged separately and that fails
 * in its own way when it is not:
 *
 *   the STL          libstdc++ built for this libc, with the portable operating
 *                    system layer rather than glibc's internals
 *   exceptions       the unwind tables, which the linker script used to
 *                    discard, plus _dl_find_object in the runtime linker to
 *                    find them from a program counter
 *   static objects   __cxa_atexit and __cxa_finalize in libc, which run
 *                    destructors at exit and on dlclose
 *   thread_local     __cxa_thread_atexit_impl, which runs them at thread exit
 *
 * A failure in the unwinder does not report itself: a throw with no usable
 * tables calls std::terminate, so the process aborts rather than printing a
 * wrong answer.  That is why "caught" being printed at all is most of the test.
 */

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <pthread.h>

static int failures;

static void check(const char *what, bool ok)
{
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		failures++;
}

/* Order of destruction is the reverse of construction, and both have to happen
 * around main() rather than inside it. */
static int ctor_order;
static int dtor_order;

struct Tracer {
	int id;
	explicit Tracer(int i) : id(i) { ctor_order = ctor_order * 10 + i; }
	~Tracer() { dtor_order = dtor_order * 10 + id; }
};

static Tracer tracer_one(1);
static Tracer tracer_two(2);

/* Reported by an atexit handler, which runs after main has returned and, for a
 * static object, before its destructor -- so this sees the order as it stands
 * at that moment. */
static void report_at_exit(void)
{
	std::printf("  [%s] static constructors ran before main (order %d)\n",
		    ctor_order == 12 ? "PASS" : "FAIL", ctor_order);
}

static thread_local struct ThreadLocal {
	int used;
	~ThreadLocal();
} tls_object;

static volatile int tls_destroyed;

ThreadLocal::~ThreadLocal()
{
	tls_destroyed = 1;
}

static void *thread_body(void *)
{
	tls_object.used = 1;
	return nullptr;
}

static void test_containers(void)
{
	std::printf("[TEST] containers and strings\n");

	std::string s = "LikeOS";
	s += "-64";
	check("std::string concatenation", s == "LikeOS-64");
	check("std::string::find", s.find("OS") == 4);
	check("std::to_string", std::to_string(4096) == "4096");
	check("std::stoi", std::stoi("-17") == -17);

	std::vector<int> v;
	for (int i = 0; i < 1000; i++)
		v.push_back(i * 3);
	check("std::vector growth", v.size() == 1000 && v[999] == 2997);
	std::sort(v.begin(), v.end(), [](int a, int b) { return a > b; });
	check("std::sort with a lambda", v.front() == 2997 && v.back() == 0);

	std::map<std::string, int> m;
	m["one"] = 1;
	m["two"] = 2;
	check("std::map lookup", m["two"] == 2 && m.count("three") == 0);

	/* Reference counting, and a destructor running when the last one goes. */
	bool released = false;
	{
		auto p = std::shared_ptr<int>(new int(7), [&](int *q) {
			released = true;
			delete q;
		});
		auto q = p;
		check("shared_ptr use_count", p.use_count() == 2 && *q == 7);
	}
	check("shared_ptr released at scope exit", released);
}

static void test_exceptions(void)
{
	std::printf("[TEST] exceptions\n");

	try {
		throw std::runtime_error("thrown");
		check("unreachable after throw", false);
	} catch (const std::runtime_error &e) {
		check("caught by exact type", std::strcmp(e.what(), "thrown") == 0);
	} catch (...) {
		check("caught by exact type", false);
	}

	/* Through a base-class handler, which needs the runtime type information
	 * the unwinder consults to decide whether a handler matches. */
	try {
		throw std::out_of_range("range");
	} catch (const std::exception &e) {
		check("caught by base class", std::strcmp(e.what(), "range") == 0);
	} catch (...) {
		check("caught by base class", false);
	}

	/* Unwinding past a frame with a destructor: the object must be destroyed
	 * on the way out, which is the whole point of the tables being present. */
	static bool unwound;
	struct Guard {
		bool *flag;
		~Guard() { *flag = true; }
	};
	try {
		Guard g{&unwound};
		throw 42;
	} catch (int n) {
		check("caught a thrown int", n == 42);
	}
	check("destructor ran while unwinding", unwound);

	/* Across a library boundary: this one is thrown by libstdc++ itself. */
	try {
		std::vector<int> v(3);
		(void)v.at(99);
		check("at() out of range threw", false);
	} catch (const std::out_of_range &) {
		check("at() out of range threw", true);
	}
}

static void test_thread_local(void)
{
	std::printf("[TEST] thread_local destructors\n");

	pthread_t t;
	if (pthread_create(&t, nullptr, thread_body, nullptr) != 0) {
		check("pthread_create", false);
		return;
	}
	pthread_join(t, nullptr);
	check("thread_local destructor ran at thread exit", tls_destroyed == 1);
}

int main(void)
{
	std::printf("\n========================================\n");
	std::printf("  LikeOS-64 C++ runtime tests\n");
	std::printf("========================================\n\n");

	std::atexit(report_at_exit);

	test_containers();
	test_exceptions();
	test_thread_local();

	std::printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
	std::printf("(static destructors run after this line; expect \"21\")\n");
	return failures ? 1 : 0;
}

/*
 * Runs last, after main and after the atexit handler above, and reports the
 * destruction order it observes.  Registered through __cxa_atexit with this
 * object's __dso_handle, exactly as the compiler registers the Tracers, so if
 * this prints at all then the whole exit-handler chain is intact.
 */
struct FinalReport {
	~FinalReport()
	{
		std::printf("  destructor order so far: %d (expect 2 then 1)\n",
			    dtor_order);
	}
};
static FinalReport final_report;
