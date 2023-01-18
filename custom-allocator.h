
// Custom allocator using TLSF:
// - Global and per-thread heap.
// - Both lockless ('Unsafe') and locked allocations.
// - Optional new and delete operator overloads.

#pragma once

#include <mutex>

#include "tlsf/tlsf.h"

#include "alloc-aligned.h"
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

	// The 'Unsafe' functions are lockless, do not use them multi-threaded.
	// The ones under NED_FLANDERS do have a lock.

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
		std::lock_guard lock(m_mutex);
		return AllocateUnsafe(size);
	}

	BOGGLE_INLINE void* AllocateAligned(size_t size, size_t align)
	{
		std::lock_guard lock(m_mutex);
		return AllocateAlignedUnsafe(size, align);
	}

	BOGGLE_INLINE void Free(void* address)
	{
		std::lock_guard lock(m_mutex);
		tlsf_free(m_instance, address);
	}
#endif

	BOGGLE_INLINE_FORCE void* GetPool() const
	{
		return m_pool;
	}

	// Use this to wipe the allocator state; this invalidates all allocations
	BOGGLE_INLINE void Reset(size_t poolSize) 
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

// Per-thread heap/pool
static std::vector<CustomAlloc> s_threadCustomAlloc;

// STL per-thread allocator (C++17) -> use with caution!
// Source: https://codereview.stackexchange.com/questions/217488/a-c17-stdallocator-implementation
template <typename T>
	class ThreadAllocator {
public:
	using value_type = T;
	using propagate_on_container_move_assignment = std::true_type;
	using is_always_equal = std::true_type;

	ThreadAllocator() = default;
	ThreadAllocator(const ThreadAllocator&) = default;
	~ThreadAllocator() = default;

	template <class U>
		constexpr ThreadAllocator(const ThreadAllocator<U>&) noexcept {}

	BOGGLE_INLINE_FORCE T* allocate(std::size_t n)
	{
		if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
			return static_cast<T*>(
				s_threadCustomAlloc[s_iThread].AllocateAlignedUnsafe(n * sizeof(T), static_cast<std::align_val_t>(alignof(T)))
				);
		else
			return static_cast<T*>(
				s_threadCustomAlloc[s_iThread].AllocateUnsafe(n * sizeof(T))
				);
	}

	BOGGLE_INLINE_FORCE void deallocate(T* p, std::size_t n)
	{	
		s_threadCustomAlloc[s_iThread].FreeUnsafe(p);
	}
};

template <class T, class U> constexpr bool operator==(const ThreadAllocator<T>&, const ThreadAllocator<U>&) noexcept { return true;  }
template <class T, class U> constexpr bool operator!=(const ThreadAllocator<T>&, const ThreadAllocator<U>&) noexcept { return false; }
