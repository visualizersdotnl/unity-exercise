
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
	CustomAlloc(size_t poolSize) :
		m_isOwner(true)
,		m_pool(mallocAligned(poolSize, kPageSize))
	{
		m_instance = tlsf_create_with_pool(m_pool, poolSize);
	}

	CustomAlloc(char* pool, size_t poolSize) :
		m_isOwner(false)
,		m_pool(pool)
	{
		m_instance = tlsf_create_with_pool(pool, poolSize);
	}

	~CustomAlloc()
	{
		tlsf_destroy(m_instance);
		
		if (true == m_isOwner)
			freeAligned(m_pool);
	}

	BOGGLE_INLINE void* AllocateUnsafe(size_t size, size_t align)
	{
//		m_peakUse += size;
		return tlsf_memalign(m_instance, align, size);
	}

	BOGGLE_INLINE_FORCE void FreeUnsafe(void* address)
	{
		tlsf_free(m_instance, address);
	}

	BOGGLE_INLINE void* Allocate(size_t size, size_t align)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return AllocateUnsafe(size, align);
	}

	BOGGLE_INLINE void Free(void* address)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		tlsf_free(m_instance, address);
	}

	BOGGLE_INLINE_FORCE void* GetPool() const
	{
		return m_pool;
	}

//	BOGGLE_INLINE size_t GetPeakUse() const
//	{
//		return m_peakUse;
//	}

private: 
	tlsf_t m_instance;
	bool m_isOwner;
	void* m_pool;
	std::mutex m_mutex;
	//	size_t m_peakUse = 0;
};

// Global heap/pool
static CustomAlloc s_globalCustomAlloc(GLOBAL_MEMORY_POOL_SIZE);
#define CUSTOM_NEW void* operator new(size_t size) { return s_globalCustomAlloc.AllocateUnsafe(size, 16); }
#define CUSTOM_DELETE void operator delete(void* address) { return s_globalCustomAlloc.FreeUnsafe(address); }

// Per thread heap/pool
static std::vector<CustomAlloc*> s_threadCustomAlloc;
#define CUSTOM_NEW_THREAD(ThreadIndex) void* operator new(size_t size) { return s_threadCustomAlloc[ThreadIndex]->AllocateUnsafe(size, 16); }
#define CUSTOM_DELETE_THREAD(ThreadIndex) void operator delete(void* address) { return s_threadCustomAlloc[ThreadIndex]->FreeUnsafe(address); }
