
// Simple-as-fuck thread-safe TLSF wrapper.
// Only use *after* static initialization.

#pragma once

// #include <mutex>

#include "tlsf/tlsf.h"

#include "alloc-aligned.h"
#include "bit-tricks.h"
// #include "spinlock.h"
#include "inline.h"

const size_t kPageSize = 4096; // Usually right ;)

class CustomAlloc
{
public:
	// m_isOwner will be false if it's there at all, so nothing to worry about
	CustomAlloc() {}

	explicit CustomAlloc(size_t poolSize) :
		m_instance(tlsf_create_with_pool(m_pool = mallocAligned(poolSize, kPageSize), poolSize)) {}

	explicit CustomAlloc(char* pool, size_t poolSize) :
		m_instance(tlsf_create_with_pool(m_pool = pool, poolSize)) {}

#ifdef NED_FLANDERS // We can get away by leaking some memory at the very end, right?
	~CustomAlloc()
	{
		// Does nothing!
//		tlsf_destroy(m_instance);
		
		if (true == m_isOwner)
			freeAligned(m_pool);
	}
#endif
	BOGGLE_INLINE_FORCE void* AllocateUnsafe(size_t size)
	{
		return tlsf_malloc(m_instance, size);
	}

	BOGGLE_INLINE_FORCE void* AllocateAlignedUnsafe(size_t size, size_t align)
	{
		return tlsf_memalign(m_instance, align, size);
	}

	BOGGLE_INLINE_FORCE void FreeUnsafe(void* address)
	{
		tlsf_free(m_instance, address);
	}

#ifdef NED_FLANDERS
	BOGGLE_INLINE void* Allocate(size_t size)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return AllocateUnsafe(size);
	}

	BOGGLE_INLINE void* AllocateAligned(size_t size, size_t align)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return AllocateAlignedUnsafe(size, align);
	}

	BOGGLE_INLINE void Free(void* address)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		tlsf_free(m_instance, address);
	}
#endif

	BOGGLE_INLINE_FORCE void* GetPool() const
	{
		return m_pool;
	}

	// Use this to wipe the allocator; you can only safely do this if you're not going to need anything allocated earlier
	BOGGLE_INLINE_FORCE void Reset(size_t poolSize) // Maybe I can fetch this from TLSF, but storing it i'd rather not (for performance)
	{
		// Simply recreating the instance wipes all history
		m_instance = tlsf_create_with_pool(m_pool, poolSize);
	}

private: 
	tlsf_t m_instance;
	void* m_pool;

#ifdef NED_FLANDERS
	const bool m_isOwner = false;
	std::mutex m_mutex;
#endif
};


// Global heap/pool
static CustomAlloc s_globalCustomAlloc(GLOBAL_MEMORY_POOL_SIZE);

// For now things seem much faster when *not* using this pool for just any allocation (only the ones you gave some thought)
// #define CUSTOM_NEW void* operator new(size_t size) { return s_globalCustomAlloc.AllocateUnsafe(size); }
// #define CUSTOM_DELETE void operator delete(void* address) { return s_globalCustomAlloc.FreeUnsafe(address); }

// Per thread heap/pool
static std::vector<CustomAlloc> s_threadCustomAlloc;
#define CUSTOM_NEW_THREAD(ThreadIndex) void* operator new(size_t size) { return s_threadCustomAlloc[ThreadIndex].AllocateUnsafe(size); }
#define CUSTOM_DELETE_THREAD(ThreadIndex) void operator delete(void* address) { return s_threadCustomAlloc[ThreadIndex].FreeUnsafe(address); }
