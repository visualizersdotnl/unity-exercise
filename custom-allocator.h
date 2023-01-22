
#pragma once

#include <mutex>

#include "tlsf/tlsf.h"

#include "alloc-aligned.h"
#include "inline.h"

#if defined(FOR_INTEL)
	const size_t kPageSize = 4096; // 4KB
#elif defined(FOR_ARM)
	const size_t kPageSize = 4096*4; // 16KB (assuming Apple M/Silicon)
#endif

class CustomAlloc
{
public:
	// m_isOwner will be false if it's there at all, so nothing to worry about
	CustomAlloc() {}

	explicit CustomAlloc(size_t poolSize) :
		m_instance(tlsf_create_with_pool(m_pool = mallocAligned(poolSize, kPageSize), poolSize)) 
	{
#if defined(NED_FLANDERS)
		m_isOwner = true;
#endif
	}

	explicit CustomAlloc(char* pool, size_t poolSize) :
		m_instance(tlsf_create_with_pool(m_pool = pool, poolSize))
	{
#if defined(NED_FLANDERS)
		m_isOwner = false;
#endif
	}

#if defined(NED_FLANDERS)

	// That's right, we don't release an owned pool (s_globalCustomAlloc), why would we?
	~CustomAlloc()
	{
		// Does nothing!
//		tlsf_destroy(m_instance);
		
		if (true == m_isOwner)
			freeAligned(m_pool);
	}

	CustomAlloc(const CustomAlloc& RHS) :
		m_instance(RHS.m_instance)
,		m_pool(RHS.m_pool)
,		m_isOwner(RHS.m_isOwner)
	{
		RHS.m_isOwner = false;
	}

	CustomAlloc& operator=(const CustomAlloc& RHS)
	{
		m_instance = RHS.m_instance;
		m_pool = RHS.m_pool;
		m_isOwner = RHS.m_isOwner;

		RHS.m_isOwner = false;

		return *this;
	}

#endif

	// The 'Unsafe' functions are lockless, do not use them multi-threaded.
	BOGGLE_INLINE_FORCE void* AllocateUnsafe(size_t size)
	{
//		m_approxLoad += size;
		return tlsf_malloc(m_instance, size);
	}

	BOGGLE_INLINE_FORCE void* AllocateAlignedUnsafe(size_t size, size_t align)
	{
//		m_approxLoad += size;
		return tlsf_memalign(m_instance, align, size);
	}

	BOGGLE_INLINE_FORCE void FreeUnsafe(void* address)
	{
		/* const auto freed = */ tlsf_free(m_instance, address);
//		m_approxLoad -= freed;
	}

#if defined(NED_FLANDERS)
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
		FreeUnsafe(address);
	}
#endif

	// Approximation, doesn't keep overhead and alignment in mind.
	BOGGLE_INLINE_FORCE void* GetPool() const
	{
		return m_pool;
	}

	// Use this to wipe the allocator state; this invalidates all allocations.
	BOGGLE_INLINE void Reset(size_t poolSize) 
	{
		// Simply recreating the instance wipes all history
		m_instance = tlsf_create_with_pool(m_pool, poolSize);
//		m_approxLoad = 0;
	}

/*
	// Returns allocated blocks *without* their overhead et cetera.
	BOGGLE_INLINE_FORCE size_t GetApproxLoad() const {
		return m_approxLoad;
	}
*/

private: 
	tlsf_t m_instance;
	void* m_pool;

#if defined(NED_FLANDERS)
	mutable bool m_isOwner = false;
	std::mutex m_mutex;
#endif

//	size_t m_approxLoad = 0;
};

// Global heap/pool
static CustomAlloc s_globalCustomAlloc(GLOBAL_MEMORY_POOL_SIZE);
static std::vector<CustomAlloc> s_threadCustomAlloc;

#if 0

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
				s_threadCustomAlloc[s_iThread].AllocateAlignedUnsafe(n * sizeof(T), static_cast<size_t>(alignof(T)))
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

#endif