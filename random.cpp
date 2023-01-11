
/* GR-1: random number generator(s) -- (C) Tasty Chips Electronics  */

#include "random.h"
// #include <time.h>

/* Include 'Tiny Mersenne-Twister' 32-bit right here, since it's the only place we'll be using it. */
#include "tinymt/tinymt32.c"

static tinymt32_t s_genState;

#include <stdlib.h>

void initialize_random_generator()
{
	const uint32_t seed = 0xdeadbeef;
	tinymt32_init(&s_genState, seed);
}

float mt_randf()
{
	return tinymt32_generate_floatOC(&s_genState);
}

uint32_t mt_randu32()
{
	return tinymt32_generate_uint32(&s_genState);
}

int32_t mt_rand32() 
{ 
	return (int32_t) mt_randu32(); 
}

