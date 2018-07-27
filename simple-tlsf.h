
//
// Simple-as-fuck thread-safe TLSF wrapper.
// Only use *after* static initialization.
// For testing purposes.
//

#pragma once

#include <mutex>

#include "tlsf/tlsf.h"

#include "alloc-aligned.h"
#include "bit-tricks.h"

const size_t kPageSize = 4096; // Usually right ;)

class CustomAlloc
{
public:
	CustomAlloc()
	{
		const size_t kPoolSize = 0x7d000000; /* 2GB */
		m_pool = mallocAligned(kPoolSize, kPageSize);
		m_instance = tlsf_create_with_pool(m_pool, kPoolSize);
	}

	~CustomAlloc()
	{
		tlsf_destroy(m_instance);
		freeAligned(m_pool);
	}

	inline void* Allocate(size_t size)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return tlsf_malloc(m_instance, size);
	}

	inline void* Allocate(size_t size, size_t align)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		void *address = tlsf_memalign(m_instance, align, size);
		return address;
	}

	inline void Free(void* address)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		tlsf_free(m_instance, address);
	}

	inline tlsf_t Get() { return m_instance; }

private: 
	void* m_pool;
	tlsf_t m_instance;

	std::mutex m_mutex;
} static s_customAlloc;

#define CUSTOM_NEW void* operator new(size_t size) { return s_customAlloc.Allocate(size, RoundPow2_64(size)); }
// #define CUSTOM_NEW void* operator new(size_t size) { return s_customAlloc.Allocate(size); }
#define CUSTOM_DELETE void operator delete(void* address) { return s_customAlloc.Free(address); }
