
/*
	Simple sequential/contiguous memory block allocator.
	Allocate at will, then dump at once.
	C-style, so no construction nor destruction.

	FIXME: OSX/Linux!
*/

#pragma once

#include <stdlib.h>

// FIXME: move elsewhere or pick a different allocator for the entire project.
//        Also a bit odd that both Windows and OSX don't implement aligned_alloc() (C++11).
#ifdef _WIN32

	inline void* AllocAligned(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
	inline void FreeAligned(void* address) { _aligned_free(address); }

#else // _GNUC_, hopefully

	inline void* AllocAligned(size_t size, size_t alignment) 
	{ 
		void* address;
		posix_memalign(&address, alignment, size);
		return address;
	}

	inline void FreeAligned(void* address) { free(address); }

#endif

template <typename T> class SeqAlloc
{
public:
	SeqAlloc(size_t count) :
		m_index(0)
	{
		m_blocks = static_cast<T*>(AllocAligned(count*sizeof(T), sizeof(size_t)<<3) /* Likely cache line size */);
	}

	~SeqAlloc()
	{
		FreeAligned(m_blocks);
	}

	inline T* New()
	{
		return m_blocks + m_index++;
	}

private:
	size_t m_index;
	T* m_blocks;
};
