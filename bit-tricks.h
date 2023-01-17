
#pragma once

#include "inline.h"

BOGGLE_INLINE_FORCE unsigned RoundPow2_32(unsigned value)
{
	--value;
	value |= value >> 1;
	value |= value >> 2;
	value |= value >> 4;
	value |= value >> 8;
	value |= value >> 16;
	return value+1;
}

BOGGLE_INLINE_FORCE size_t RoundPow2_64(size_t value)
{
	--value;
	value |= value >> 1;
	value |= value >> 2;
	value |= value >> 4;
	value |= value >> 8;
	value |= value >> 16;
	value |= value >> 32;
	return value+1;
}

#ifdef _WIN32
	#pragma warning(disable:4146) // 'unary minus operator applied, result still unsigned' (IsZero())
#endif

// Thank you Bit Twiddling Hacks.
BOGGLE_INLINE_FORCE unsigned IsNotZero(unsigned value) { return ((value | (~value + 1)) >> 31) & 1; }
BOGGLE_INLINE_FORCE unsigned IsZero(unsigned value) { return 1 + (value >> 31) - (-value >> 31); }
