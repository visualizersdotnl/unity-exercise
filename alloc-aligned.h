
/*
	Win32+GCC aligned alloc. & free. 
	Aligns to probable size of cache line.
*/

#pragma once

#include <stdlib.h>

void* mallocAligned(size_t size);
void  freeAligned(void* address);

#ifdef _WIN32

	void* mallocAligned(size_t size) { return _aligned_malloc(size, sizeof(size_t)<<3); }
	void  freeAligned(void* address) {  _aligned_free(address); }

#elif defined(__GNUC__)

	void* mallocAligned(size_t size) 
	{ 
		void* address;
		posix_memalign(&address, sizeof(size_t)<<3, size);
		return address;
	}

	void freeAligned(void* address) { free(address); }

#endif
