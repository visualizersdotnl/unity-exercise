
/*
	Simple sequential/contiguous memory block allocator.
	Allocate at will, then dump at once.
	C-style, so no construction nor destruction.
*/

#pragma once

template <typename T> class SeqAlloc
{
public:
	SeqAlloc(size_t count) :
		m_index(0)
	{
		m_blocks = static_cast<T*>(malloc(count*sizeof(T)));
	}

	~SeqAlloc()
	{
		free(m_blocks);
	}

	inline T* New()
	{
		return m_blocks + m_index++;
	}

private:
	size_t m_index;
	T* m_blocks;
};
