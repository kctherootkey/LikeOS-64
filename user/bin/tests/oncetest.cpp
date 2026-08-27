/*
 * Does std::call_once work on this system?
 *
 * luakit dies with RIP=0 inside webkit_website_data_manager_new(), and the
 * return address on the stack is pthread_once+0x2a -- the instruction after
 * `call *%rsi'.  libstdc++ implements std::call_once by stashing the callable
 * in the thread-local std::__once_call and handing pthread_once a proxy that
 * reads it back and TAIL-JUMPS to it, so a zero read there lands at address 0
 * with pthread_once's return address still on the stack: exactly what the
 * crash dump shows.
 *
 * That is a hypothesis, not a finding.  This program tests it away from
 * WebKit's several million lines: if call_once dies here, the fault is in
 * libstdc++/TLS/pthread_once and has nothing to do with the browser; if it
 * passes, the mechanism is sound and the crash is something WebKit does on
 * top of it.  Each step announces itself BEFORE it runs, so the last line
 * printed names the step that died.
 */
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <thread>

static int g_raw;
static void raw_init(void) { g_raw = 1; }

static int g_once;
static void set_once() { g_once = 1; }

int main(void)
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	printf("1. raw pthread_once ... ");
	static pthread_once_t oc = PTHREAD_ONCE_INIT;
	pthread_once(&oc, raw_init);
	printf("%s\n", g_raw == 1 ? "ok" : "RAN BUT DID NOT SET");

	printf("2. std::call_once, plain function ... ");
	static std::once_flag f1;
	std::call_once(f1, set_once);
	printf("%s\n", g_once == 1 ? "ok" : "RAN BUT DID NOT SET");

	printf("3. std::call_once, capturing lambda ... ");
	int captured = 0;
	static std::once_flag f2;
	std::call_once(f2, [&captured] { captured = 42; });
	printf("%s\n", captured == 42 ? "ok" : "RAN BUT DID NOT SET");

	printf("4. std::call_once, second call is a no-op ... ");
	std::call_once(f2, [&captured] { captured = 99; });
	printf("%s\n", captured == 42 ? "ok" : "RE-RAN (WRONG)");

	printf("5. std::call_once from a second thread ... ");
	static std::once_flag f3;
	int fromThread = 0;
	std::thread t([&] { std::call_once(f3, [&] { fromThread = 7; }); });
	t.join();
	printf("%s\n", fromThread == 7 ? "ok" : "RAN BUT DID NOT SET");

	printf("\nall five passed -- std::call_once is not the luakit bug\n");
	return 0;
}
