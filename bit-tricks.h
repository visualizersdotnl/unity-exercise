
#pragma once

inline unsigned RoundPow2_32(unsigned value)
{
	--value;
	value |= value >> 1;
	value |= value >> 2;
	value |= value >> 4;
	value |= value >> 8;
	value |= value >> 16;
	return value+1;
}

inline size_t RoundPow2_64(size_t value)
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

// Thank you Bit Twiddling Hacks.
inline unsigned IsNotZero(unsigned value) { return ((value | (~value + 1)) >> 31) & 1; }
inline unsigned IsZero(unsigned value) { return 1 + (value >> 31) - (-value >> 31); }
