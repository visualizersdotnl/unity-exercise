
/*
	Win32+GCC aligned alloc. & free. 
	Aligns to probable size of cache line.
*/

#pragma once

#include <stdlib.h>

void* mallocAligned(size_t size, size_t align);
void  freeAligned(void* address);

#ifdef _WIN32

	void* mallocAligned(size_t size, size_t align) { return _aligned_malloc(size, align); }
	void  freeAligned(void* address) {  _aligned_free(address); }

#elif defined(__GNUC__)

	void* mallocAligned(size_t size, size_t align) 
	{ 
		void* address;
		posix_memalign(&address, align, size);
		return address;
	}

	void freeAligned(void* address) { free(address); }

#endif
