
// Simple-as-fuck thread-safe TLSF wrapper.
// Only use *after* static initialization.

#pragma once

// #include <mutex>

#include "tlsf/tlsf.h"

#include "alloc-aligned.h"
#include "bit-tricks.h"
#include "spinlock.h"

const size_t kPageSize = 4096; // Usually right ;)

class CustomAlloc
{
public:
	CustomAlloc() :
		CustomAlloc(0x7d000000 >> 1 /* 1GB */) {}

	CustomAlloc(size_t poolSize)
	{
		m_pool = mallocAligned(poolSize, kPageSize);
		m_instance = tlsf_create_with_pool(m_pool, poolSize);
	}

	~CustomAlloc()
	{
		tlsf_destroy(m_instance);
		freeAligned(m_pool);
	}

	__forceinline void* AllocateUnsafe(size_t size, size_t align)
	{
		void* address = tlsf_memalign(m_instance, align, size);
		return address;
	}

	__forceinline void* Allocate(size_t size, size_t align)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
//		m_lock.lock();
		void* address = AllocateUnsafe(size, align);
//		m_lock.unlock();
		return address;
	}

	__forceinline void Free(void* address)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
//		m_lock.lock();
		tlsf_free(m_instance, address);
//		m_lock.unlock();
	}

//	inline tlsf_t Get() { return m_instance; }

private: 
	void* m_pool;
	tlsf_t m_instance;
 
	std::mutex m_mutex;
	// SpinLock m_lock;
};

// For global use, otherwise use TLSF directly
static CustomAlloc s_customAlloc;
#define CUSTOM_NEW void* operator new(size_t size) { return s_customAlloc.Allocate(size, sizeof(16)); }
#define CUSTOM_DELETE void operator delete(void* address) { return s_customAlloc.Free(address); }
