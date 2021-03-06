
/*
	Win32+GCC aligned alloc. & free. 
*/

#pragma once

#include <stdlib.h>

__inline void* mallocAligned(size_t size, size_t align);
__inline void  freeAligned(void* address);

#ifdef _WIN32

	__inline void* mallocAligned(size_t size, size_t align) { return _aligned_malloc(size, align); }
	__inline void  freeAligned(void* address) {  _aligned_free(address); }

#elif defined(__GNUC__)

	__inline void* mallocAligned(size_t size, size_t align) 
	{ 
		void* address;
		posix_memalign(&address, align, size);
		return address;
	}

	__inline void freeAligned(void* address) { free(address); }

#endif
