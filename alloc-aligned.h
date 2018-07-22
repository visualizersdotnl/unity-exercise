
/* Win32+GCC aligned alloc. & free. */

#pragma once

#include <stdlib.h>

void* CRT_AllocAligned(size_t size, size_t alignment);
void  CRT_FreeAligned(void* address);

#ifdef _WIN32

	void* CRT_AllocAligned(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
	void CRT_FreeAligned(void* address) {  _aligned_free(address); }

#elif defined(__GNUC__)

	void* CRT_AllocAligned(size_t size, size_t alignment) 
	{ 
		void* address;
		posix_memalign(&address, alignment, size);
		return address;
	}

	void CRT_FreeAligned(void* address) 
	{ 
		free(address); 
	}

#endif
