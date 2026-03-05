/*
 * LikeOS-64 test shared library implementation
 *
 * Provides simple functions for dlopen/dlsym testing.
 */

#include "testlib.h"

/* Global counter to test data segment in shared libs */
static int g_counter = 0;

/* Exported global variable */
int testlib_version = 1;

int testlib_add(int a, int b)
{
    return a + b;
}

int testlib_mul(int a, int b)
{
    return a * b;
}

const char *testlib_hello(void)
{
    return "Hello from libtestlib.so!";
}

int testlib_counter(void)
{
    return g_counter++;
}

void testlib_counter_reset(void)
{
    g_counter = 0;
}

/* Constructor: called by ld-likeos.so during DT_INIT_ARRAY processing */
static void __attribute__((constructor)) testlib_init(void)
{
    /* Mark that the library was properly initialized */
    g_counter = 0;
}
