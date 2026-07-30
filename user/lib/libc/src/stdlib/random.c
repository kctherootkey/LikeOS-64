/*
 * Pseudo-random number generators: rand()/srand() and the random() family.
 *
 * random() uses the classic additive-feedback generator: a degree-31 trinomial
 * x^31 + x^3 + 1 over a 31-entry state table, which is what the BSD and GNU
 * implementations use and what programs expect the quality and period of.  A
 * bare linear congruential generator would be far weaker in the low bits, and
 * code that takes random() % n leans on exactly those bits.
 *
 * rand() is the same generator, as POSIX permits and every mainstream libc
 * does, so the two agree rather than being independently seeded.
 *
 * NOT thread safe, matching the rest of this library: the state is global and
 * unlocked.  Use rand_r() where a thread-local sequence is needed.
 *
 * None of this is suitable for anything security-related — getrandom(2) is the
 * source for that.
 */
#include <stdlib.h>
#include <stdint.h>

#define DEG 31 /* degree of the trinomial      */
#define SEP 3  /* feedback separation          */

static int32_t r_state[DEG];
static int r_f; /* front pointer  */
static int r_r; /* rear pointer   */
static int r_seeded;

/*
 * Seed the table with a Lehmer generator (16807, 2^31-1), evaluated with
 * Schrage's method so the intermediate product never overflows 32 bits.
 * Using a different generator to fill the table than to run it is deliberate:
 * it decorrelates the initial state from the seed.
 */
static void random_seed(unsigned int seed)
{
	int32_t word;
	int i;

	/* Seed 0 would make the Lehmer step a fixed point, producing an
	 * all-zero table and a constant output stream. */
	if (seed == 0)
		seed = 1;

	word = (int32_t)(seed & 0x7FFFFFFF);
	if (word == 0)
		word = 1;
	r_state[0] = word;

	for (i = 1; i < DEG; i++) {
		int32_t hi = word / 127773;
		int32_t lo = word % 127773;
		word = 16807 * lo - 2836 * hi;
		if (word < 0)
			word += 2147483647;
		r_state[i] = word;
	}

	r_f = SEP;
	r_r = 0;
	r_seeded = 1;

	/* Run the generator well past the table length so the output no longer
	 * reflects the seeding pattern. */
	for (i = 0; i < DEG * 10; i++)
		(void)random();
}

long random(void)
{
	uint32_t result;

	if (!r_seeded)
		random_seed(1); /* the standard default seed */

	/* Deliberately unsigned: the addition is meant to wrap modulo 2^32. */
	r_state[r_f] = (int32_t)((uint32_t)r_state[r_f] +
				 (uint32_t)r_state[r_r]);
	/* The low bit of an additive generator is much weaker than the rest,
	 * so it is discarded rather than returned. */
	result = ((uint32_t)r_state[r_f] >> 1) & 0x7FFFFFFF;

	if (++r_f >= DEG)
		r_f = 0;
	if (++r_r >= DEG)
		r_r = 0;

	return (long)result;
}

void srandom(unsigned int seed)
{
	random_seed(seed);
}

/*
 * initstate()/setstate(): the size argument selects the generator's degree in
 * other implementations.  Here the degree-31 generator is always used and the
 * caller's buffer is not written to, so a state saved with one call cannot be
 * restored by another.  That is enough for the reason these are actually
 * called — getting a reproducible sequence from a known seed — and avoids
 * pretending to support a state layout that is not stable across builds.
 */
char *initstate(unsigned int seed, char *state, size_t size)
{
	(void)size;
	random_seed(seed);
	return state;
}

char *setstate(char *state)
{
	return state;
}

/*
 * rand()/srand() share the generator above, which is what POSIX allows and
 * what avoids the two drifting apart.  RAND_MAX is 2^31-1 to match.
 */
int rand(void)
{
	return (int)random();
}

void srand(unsigned int seed)
{
	random_seed(seed);
}

/*
 * rand_r(): the caller owns the state, so this one IS reentrant.  It is a
 * separate, much simpler generator because the caller's state is a single
 * unsigned int — there is nowhere to keep a 31-entry table.
 */
int rand_r(unsigned int *seedp)
{
	unsigned int next;

	if (!seedp)
		return 0;
	next = *seedp;
	next = next * 1103515245u + 12345u;
	*seedp = next;
	/* Take the high bits: the low ones of an LCG have very short periods. */
	return (int)((next >> 16) & 0x7FFF);
}

/*
 * The 48-bit family: drand48, lrand48, mrand48 and friends.
 *
 * A linear congruential generator on 48 bits, with the multiplier and addend
 * fixed by the standard:
 *
 *     X(n+1) = (0x5DEECE66D * X(n) + 0xB) mod 2^48
 *
 * Those constants are not an implementation choice.  Programs that seed with
 * srand48() and expect a particular sequence -- test suites, simulations
 * reproducing a published run -- depend on the exact arithmetic, so this is
 * specified down to the last bit and there is nothing to tune.
 *
 * Each function takes a different slice of the state, and the slice is what
 * distinguishes them: the LOW bits of an LCG have very short periods (the
 * bottom bit alternates), so every one of these reads from the top.
 *
 * Two forms of each: the plain name uses one shared, global state (not thread
 * safe, like the rest of this library), and the e/n/j-prefixed variants take
 * the caller's own state and are reentrant.
 */

#define D48_A 0x5DEECE66DULL
#define D48_C 0xBULL
#define D48_M 0xFFFFFFFFFFFFULL /* 2^48 - 1 */

static uint64_t d48_state = 0x1234ABCD330EULL; /* the standard's initial X */
static uint64_t d48_a = D48_A;
static uint64_t d48_c = D48_C;

/* Advance a 48-bit state and return the new value. */
static uint64_t d48_step(uint64_t *x)
{
	*x = (d48_a * *x + d48_c) & D48_M;
	return *x;
}

/* The three ways to read a stepped state. */
static double d48_to_double(uint64_t x)
{
	/* All 48 bits, scaled into [0, 1). */
	return (double)x / 281474976710656.0; /* 2^48 */
}

static long d48_to_long(uint64_t x)
{
	return (long)(x >> 17); /* top 31 bits, non-negative */
}

static long d48_to_signed(uint64_t x)
{
	return (long)(int32_t)(x >> 16); /* top 32 bits, signed */
}

/* Pack and unpack the three-halfword form the caller-supplied variants use. */
static uint64_t d48_load(unsigned short xsubi[3])
{
	return ((uint64_t)xsubi[2] << 32) | ((uint64_t)xsubi[1] << 16) |
	       (uint64_t)xsubi[0];
}

static void d48_store(unsigned short xsubi[3], uint64_t x)
{
	xsubi[0] = (unsigned short)(x & 0xFFFF);
	xsubi[1] = (unsigned short)((x >> 16) & 0xFFFF);
	xsubi[2] = (unsigned short)((x >> 32) & 0xFFFF);
}

double drand48(void)
{
	return d48_to_double(d48_step(&d48_state));
}

long lrand48(void)
{
	return d48_to_long(d48_step(&d48_state));
}

long mrand48(void)
{
	return d48_to_signed(d48_step(&d48_state));
}

double erand48(unsigned short xsubi[3])
{
	uint64_t x = d48_load(xsubi);
	double r = d48_to_double(d48_step(&x));

	d48_store(xsubi, x);
	return r;
}

long nrand48(unsigned short xsubi[3])
{
	uint64_t x = d48_load(xsubi);
	long r = d48_to_long(d48_step(&x));

	d48_store(xsubi, x);
	return r;
}

long jrand48(unsigned short xsubi[3])
{
	uint64_t x = d48_load(xsubi);
	long r = d48_to_signed(d48_step(&x));

	d48_store(xsubi, x);
	return r;
}

/*
 * srand48(): the seed supplies the HIGH 32 bits and the low 16 are set to the
 * constant 0x330E.  That constant is part of the specification -- it stops a
 * seed of 0 from producing an all-zero state, which an LCG never escapes.
 */
void srand48(long seed)
{
	d48_state = (((uint64_t)(uint32_t)seed) << 16) | 0x330EULL;
	d48_a = D48_A;
	d48_c = D48_C;
}

/*
 * seed48(): set the whole 48-bit state directly and return a pointer to the
 * PREVIOUS value, so a caller can save and restore a sequence.  The returned
 * buffer is static and is overwritten by the next call.
 */
unsigned short *seed48(unsigned short seed16v[3])
{
	static unsigned short previous[3];

	d48_store(previous, d48_state);
	d48_state = d48_load(seed16v);
	d48_a = D48_A;
	d48_c = D48_C;
	return previous;
}

/*
 * lcong48(): replace the state AND the multiplier and addend, i.e. choose a
 * different generator.  srand48() and seed48() put the standard constants
 * back, which is what makes it possible to return to the specified sequence.
 */
void lcong48(unsigned short param[7])
{
	d48_state = ((uint64_t)param[2] << 32) | ((uint64_t)param[1] << 16) |
		    (uint64_t)param[0];
	d48_a = ((uint64_t)param[5] << 32) | ((uint64_t)param[4] << 16) |
		(uint64_t)param[3];
	d48_c = (uint64_t)param[6];
}
