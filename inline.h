
/*
	Here you can define the keyword you want to use to "muscle" functions as inline, if necessary or wanted.
*/

#pragma once

#ifdef _WIN32
	// MSVC must be manhandled to do what I want.
	#define BOGGLE_INLINE __forceinline 
#else
	// According to Albert on Linux (so probably also OSX) no explicit hints yield the best result.
	#define BOGGLE_INLINE
#endif