
//
// Very simple thread-safe TLSF wrapper.
// Only use *after* static initialization.
// For testing purposes.
//

#pragma once

#include <mutex>

#include "tlsf/tlsf.h"

#include "alloc-aligned.h"
#include "bit-tricks.h"

class TLSF
{
public:
	TLSF()
	{
		const size_t kTLSFPoolSize = 0x7d000000; /* 2GB */
		m_pool = mallocAligned(kTLSFPoolSize, 4096);
		m_instance = tlsf_create_with_pool(m_pool, kTLSFPoolSize);
	}

	~TLSF()
	{
		tlsf_destroy(m_instance);
		freeAligned(m_pool);
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
} static s_TLSF;

#define TLSF_NEW void* operator new(size_t size) { return s_TLSF.Allocate(size, RoundPow2_64(size)); }
#define TLSF_DELETE void operator delete(void* address) { return s_TLSF.Free(address); }
