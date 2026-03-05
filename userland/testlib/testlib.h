/*
 * LikeOS-64 test shared library header
 *
 * This library is used to verify dlopen/dlsym/dlclose functionality.
 */

#ifndef _TESTLIB_H
#define _TESTLIB_H

/* Simple arithmetic function */
int testlib_add(int a, int b);

/* Multiply two integers */
int testlib_mul(int a, int b);

/* Return a constant greeting string */
const char *testlib_hello(void);

/* Access a global counter: increment and return old value */
int testlib_counter(void);

/* Reset the global counter to zero */
void testlib_counter_reset(void);

#endif /* _TESTLIB_H */
