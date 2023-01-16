/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	Please check 'solver_submitted.cpp' for original submission version, this is the competition one.

	This is an optimized version.

	- Always check for leaks (Windows debug build does it automatically).
	- FIXMEs.

	To do (low priority):
		- Building (or loading) my dictionary is slow(ish), I'm fine with that as I focus on the solver.
		- Fix class members (notation).

	There's this idea floating that if you have a hash and eliminate a part of the dict. tree that way
	by special-casing the first 3 characters you're golden, but it did not really help at all plus it 
	makes the algorithm less flexible. What if someone decides to change the rules? ;)

	Notes:
		- Currently tested on Windows 10 (VS2017), Linux & OSX.
		- It's currently faster on a proper multi-core CPU than the (Intel) Core M, probably due to those allocator locks.
		- Compile with full optimization (-O3 for ex.) for best performance.
		  Disabling C++ exceptions helps too, as they hinder inlining and are not used.
		  Please look at Albert's makefile for Linux/OSX optimal parameters!
		  And don't hesitate to play with them a little!
		- I could not assume anything about the test harness, so I did not; if you want debug output check debug_print().
		  ** I violate this to tell if this was compiled with or without NED_FLANDERS (see below).
		- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
		- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
		  is guarded by a mutex and no globals are used.
		- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.
		- My class design isn't really tight (functions and public member values galore), but for now that's fine.
		
		** Some of these stability claims only work if NED_FLANDERS (see below) is defined! **

	** 14/01/2020 **

	Fiddling around to make this faster; I have a few obvious things in mind; I won't be beautifying
	the code, so you're warned.

	- This is a non-Morton version that now handily outperforms the Morton version, so I deleted that one.
	- There's quite a bit of branching going on but trying to be smart doesn't always please the predictor/pipeline the best way, rather it's quite fast
	  in critical places because the predictor can do it's work very well (i.e. the cases lean 99% towards one side).
	- Just loosely using per-thread instances of CustomAlloc doesn't improve performance, I still think it could work because it enhances
	  locality and eliminates the need for those mutex locks.

	Threading issues (as seen in/by Superluminal):

	- Even distribution of words does not work well, but distributing them by letter is faster even though the thread loads are
	  uneven; it's a matter of how much traversal is done and the impact that has.
	- Using twice the amount of threads that would be sensible works way faster than bigger loads (once again, loads matter).
	- Currently there is code in place to use the "right" amount of threads and distribute words evenly but it is commented out.
	- I could add an option to sort the main dictionary, though currently I am loading a sorted version.

	Use profilers (MSVC & Superluminal) to really figure out what either works so well by accident or what could be fixed
	to use the "normal" amount of threads, evenly distributing the load. My gut says it's a matter of recursion.

	Thinking out loud: let's say I end up at the point of traversal with only one letter constantly being asked for (since the
	only child of the root node is 1 single letter), that probably means I ask for the same piece of memory again and again and
	cause less cache misses and less needless traversal.

	Conclusion for now: this model works because multiple smaller threads have a smaller footprint due to touching less memory,
	traversing less and thus are likely to be done much faster. The fact that some sleep more than others might just have to be
	taken for granted. That however does not mean that the memory allocation strategy for the tree must be altered by using sequential
	pools.

	** 16/01/2020 **

	I replaced DeepCopy for ThreadCopy, which keeps a sequential list of nodes and hands them out as needed, and releases the
	entire block of memory when the thread is done. I can see a speed increase without breaking consistent results.

	** 26/12/2022 **

	Uncle Henrik pointed out clearly and justly that my load balancing is off. I'm also toying around with the allocation
	and initialization of the node.

	** 30/12/2022 **
	
	More optimizations w/Albert. Now splitting the dictionary over an/the amount of threads.

	** 02/01/2023 **

	Introducing 'sse2neon'.
	Try OpenMP?

	** 03/01/2023 **

	Things that matter:
	- Node size (multiple megabytes per thread!).
	- Keep traversal as simple as possible, smart early-outs not seldom cause execution to shoot up due to unpredictability.
	- SSE non-cached writes did not help at all -> Removed.
	- Most low-level concerns are valid at this point.

	** 05/01/2023 **

	- Just using one block of memory per thread now, got a few bits to spare even.
	- Misc. fixes & cleaning.

	** 12/01/2023 **
	
	- Several mcro-optimizations (see Github history), still adhering, somewhat, to the Unity test API requirements.
	- I've move adding the offset to the 'visited' address to the caller.
	- It, logically (right?), seems important to have as little on the stack as possible during traversal.
	- Trying some things using prefetches; it looks as if there's not much to gain. On Apple M though...

	** 13+14/01/2023 **
	
	- Allocators: changed allocation strategy where each thread has it's own heap allocated from the global one.
	- Optimized allocators in various ways; we *do* however not free the heap on exit, but who cares, right?

	** 16/01/2023 **
	
	- Optimized pruning.
	- Set custom allocator (TLSF) heap to use 4-byte alignment in 64-bit builds as well (see top of impl. file).
	- Reverted all menial allocations back to the CRT heap, which just performs better, lesson: don't defer it all to a single mechanism without measuring/thinking.
*/

// Make VC++ 2015 shut up and walk in line.
#define _CRT_SECURE_NO_WARNINGS 

// We don't do C++ exceptions, so don't moan.
#pragma warning(disable : 4530)

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <memory>
#include <string>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cassert>
// #include <unordered_map>
// #include <atomic>
// #include <map>

#ifdef _WIN32
	#include <intrin.h>
#endif

#include "api.h"

#include "random.h"
#include "bit-tricks.h"
#include "inline.h"

// Undef. to skip dead end percentages and all prints and such.
// #define DEBUG_STATS

// Undef. to enable all the work I put in to place a, as it turns out, very forgiving test harness.
// But basically the only gaurantee here is that this works with my own test!
// #define NED_FLANDERS

// Undef. to use only 1 thread.
// #define SINGLE_THREAD

// Undef. to kill assertions.
// #define ASSERTIONS

#define GLOBAL_MEMORY_POOL_SIZE (1024*1024)*2000 // Min. 2GB!
#include "simple-tlsf.h" // Depends on Ned Flanders :)

// Undef. to kill prefetching
#ifdef _WIN32
//	#define NO_PREFETCHES // Seems to have some effect on Apple M
#endif

// Max. word length (for optimization)
#define MAX_WORD_LEN 15

#if defined(_DEBUG) || defined(ASSERTIONS)
	#ifdef _WIN32
		#define Assert(condition) if (!(condition)) __debugbreak();
	#else
		#define Assert assert
	#endif
#else
	inline void Assert(bool condition) {}
#endif

#if defined(DEBUG_STATS)
	#define debug_print printf
#else
	__inline void debug_print(const char* format, ...) {}
#endif

constexpr unsigned kAlphaRange = ('Z'-'A')+1;

#if defined(SINGLE_THREAD)
	constexpr size_t kNumThreads = 1;
#else
	const size_t kNumConcurrrency = std::thread::hardware_concurrency();
	
#if defined(_WIN32)
	const size_t kNumThreads = kNumConcurrrency+(kNumConcurrrency/2); // This causes all kinds of pre-emption stalls (see Superluminal) but for all the wrong reasons, better peak perf.
#else
	const size_t kNumThreads = kNumConcurrrency+(kNumConcurrrency/2);
#endif

#endif

// TLS
static thread_local unsigned s_iThread;

constexpr size_t kCacheLine = sizeof(size_t)*8;

#ifndef NO_PREFETCHES

// Far prefetch (Win32: only L3)
BOGGLE_INLINE_FORCE static void FarPrefetch(const char* address)
{
#ifdef __GNUC__
	__builtin_prefetch(address, 1, 3);
#elif defined(_WIN32)
	_mm_prefetch(address, _MM_HINT_T2);
#endif
}

// Near prefetch (Win32: Only L2+)
BOGGLE_INLINE_FORCE static void NearPrefetch(const char* address)
{
#ifdef __GNUC__
	__builtin_prefetch(address, 1, 1);
#elif defined(_WIN32)
	_mm_prefetch(address, _MM_HINT_T1);
#endif
}

// Immediate prefetch (Win32: all levels if possible)
BOGGLE_INLINE_FORCE static void ImmPrefetch(const char* address)
{
#ifdef __GNUC__
	__builtin_prefetch(address, 1, 0);
#elif defined(_WIN32)
	_mm_prefetch(address, _MM_HINT_T0);
#endif
}

#else

BOGGLE_INLINE_FORCE static void FarPrefetch(const char* address) {}
BOGGLE_INLINE_FORCE static void NearPrefetch(const char* address) {}
BOGGLE_INLINE_FORCE static void ImmPrefetch(const char* address) {}

#endif


// Dictionary word (score is best precalculated)
class Word
{
public:
	Word(unsigned score, const std::string& word) :
	score(score)
	{
		strcpy(this->word, word.c_str());
	}
	
	char word[MAX_WORD_LEN+1];
	size_t score;
};

// Number of words and number of nodes (1 for root) per thread.
class ThreadInfo
{
public:
	ThreadInfo() : 
		load(0), nodes(1) {}

	size_t load;
	size_t nodes;
};

static std::vector<ThreadInfo> s_threadInfo;

// Full dictionary
static std::vector<Word> s_words;

// Cheap way to tag along the tiles (few bits left)
constexpr unsigned kTileVisitedBit = 1<<7;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
BOGGLE_INLINE_FORCE unsigned LetterToIndex(unsigned letter)
{
	return letter - static_cast<unsigned>('A');
}

constexpr auto kIndexU = 'U'-'A'; // LetterToIndex('U');

// FWD.
class LoadDictionaryNode;
class DictionaryNode;

// A tree root per thread.
static std::vector<LoadDictionaryNode*> s_threadDicts;

// Load node.
class LoadDictionaryNode
{
	friend class DictionaryNode;

	friend void AddWordToDictionary(const std::string& word, size_t iThread);
	friend void FreeDictionary();

public:
	LoadDictionaryNode() {}

	// Destructor is not called when using ThreadCopy!
	~LoadDictionaryNode()
	{
		for (unsigned iChar = 0; iChar < kAlphaRange; ++iChar)
			delete m_children[iChar];
	}

	// Only called from LoadDictionary().
	LoadDictionaryNode* AddChild(char letter, size_t iThread)
	{
		const unsigned index = LetterToIndex(letter);

		// Adding child en route to a new word, so up the ref. count
		++m_wordRefCount;

		const unsigned bit = 1 << index;
		if (m_indexBits & bit)
		{
			auto* child = m_children[index];
			++child->m_wordRefCount;
			return child;
		}

		++s_threadInfo[iThread].nodes;

		m_indexBits |= bit;

		return m_children[index] = new LoadDictionaryNode();
	}

	BOGGLE_INLINE_FORCE LoadDictionaryNode* GetChild(unsigned index)
	{
		Assert(nullptr != m_children[index]);
		return m_children[index];
	}

private:
	int32_t m_wordIdx = -1; 
	uint32_t m_indexBits = 0;
	uint32_t m_wordRefCount = 1;
	LoadDictionaryNode* m_children[kAlphaRange] = { nullptr };
};

// Actual processing node.
class DictionaryNode
{
	friend class LoadDictionaryNode;

public:
	CUSTOM_NEW_THREAD(s_iThread)
	CUSTOM_DELETE_THREAD(s_iThread)
		
	DictionaryNode() {}

	class ThreadCopy
	{
	public:
		CUSTOM_NEW_THREAD(s_iThread)
		CUSTOM_DELETE_THREAD(s_iThread)
	
		ThreadCopy(unsigned iThread) : 
//			m_iThread(iThread), 
			m_iAlloc(0)
		{
			// Allocate pool for all necessary nodes: why here, you glorious titwillow?
			// Well, it sits nice and snug on it's on (probably) page boundary aligning nicely with this thread's cache as opposed to allocating
			// one huge block at once.
			const auto size = s_threadInfo[iThread].nodes*sizeof(DictionaryNode);
			m_pool = static_cast<DictionaryNode*>(s_threadCustomAlloc[iThread].AllocateAlignedUnsafe(size, kCacheLine));

			// Recursively copy them.
			Copy(s_threadDicts[iThread]);
			m_pool->m_children[kIndexU] = 0;
		}

		~ThreadCopy()
		{
//			s_threadCustomAlloc[m_iThread]->FreeUnsafe(m_pool);
		}

		BOGGLE_INLINE_FORCE DictionaryNode* Get() const
		{
			return m_pool;
		}

	private:
		uint32_t Copy(LoadDictionaryNode* parent)
		{
//			Assert(m_iAlloc < s_threadInfo[m_iThread].nodes);
			DictionaryNode* node = m_pool + m_iAlloc;
			++m_iAlloc;

			unsigned indexBits = node->m_indexBits = parent->m_indexBits;
			node->m_wordRefCount = parent->m_wordRefCount;
			node->m_wordIdx = parent->m_wordIdx;
			node->m_poolUpper32 = reinterpret_cast<uint64_t>(m_pool) & 0xffffffff00000000; // Yup, this will be downright dirty!

			const uint32_t nodeLower32 = reinterpret_cast<uint64_t>(node)&0xffffffff;

#ifdef _WIN32
			unsigned long index;
			if (_BitScanForward(&index, indexBits))
			{
#elif defined(__GNUC__)
			int index = __builtin_ffs(int(indexBits));
			if (index--)
			{
#endif
				indexBits >>= index;
			
				for (; index < kAlphaRange; ++index)
				{
					if (indexBits & 1)
					{
						node->m_children[index] = Copy(parent->GetChild(index));
						node->GetChild(index)->m_children[kIndexU] = nodeLower32;
					}

					indexBits >>= 1;
				}
			}

			return nodeLower32;
		}

	private:
		DictionaryNode* m_pool;
		size_t m_iAlloc;
	};

	// Destructor is not called when using ThreadCopy!
	~DictionaryNode() = delete;

public:
	BOGGLE_INLINE_FORCE bool HasChildren() const     { return m_indexBits > 0;     }
	BOGGLE_INLINE_FORCE bool IsWord() const          { return m_wordIdx > -1;      }

#if 0
	// FIXME: experimental
	BOGGLE_INLINE void Prune(const std::string& word)
	{
		auto* current = this;
		for (auto iLetter = 0; iLetter < word.size(); ++iLetter)
		{
			const auto index = LetterToIndex(word[iLetter]);
			if (kIndexU != index)
			{
				auto* child = current->GetChildChecked(index); // Because we prune topmost in TraverseCall()
				if (nullptr != child)
				{
					if (--child->m_wordRefCount == 0)
					{
						current->RemoveChild(index);
					}

					current = child;
				}
				else
				{
					// Most often if topmost was already pruned during traversal
					break;
				}
			}
		}
	}
#endif

	BOGGLE_INLINE_FORCE void PruneReverse()
	{
		DictionaryNode* current = this;
		while (uint32_t rootLower32 = current->m_children[kIndexU])
		{
			if (current->m_wordRefCount == 1)
			{
				current->m_indexBits = 0;
			}

			--current->m_wordRefCount;
			current = reinterpret_cast<DictionaryNode*>(m_poolUpper32|rootLower32);
			ImmPrefetch(reinterpret_cast<const char*>(current->m_children + kIndexU));
		}
	}

	BOGGLE_INLINE_FORCE void RemoveChild(unsigned index)
	{
		Assert(index < kAlphaRange);
//		Assert(HasChild(index));

		m_indexBits ^= 1 << index;
	}

	// Returns non-zero if true.
	BOGGLE_INLINE_FORCE unsigned HasChild(unsigned index) const
	{
		Assert(index < kAlphaRange);
		return m_indexBits & (1 << index);
	}

	BOGGLE_INLINE DictionaryNode* GetChild(unsigned index) const
	{
		constexpr auto kIndexU = 'U'-'A'; // LetterToIndex('U');
		Assert(kIndexU || HasChild(index));
		return reinterpret_cast<DictionaryNode*>(m_poolUpper32|m_children[index]);
	}

	// Returns NULL if no child
	BOGGLE_INLINE_FORCE DictionaryNode* GetChildChecked(unsigned index) const
	{
		if (!HasChild(index))
			return nullptr;
		else
		{
			return reinterpret_cast<DictionaryNode*>(m_poolUpper32|m_children[index]); // This dirty trick courtesty of Alex B.
		}
	}

	// If not -1, it's pointing to a word
	BOGGLE_INLINE_FORCE int32_t GetWordIndex() const
	{
		return m_wordIdx;
	}

private:
	uint32_t m_indexBits;
public:
	int32_t m_wordRefCount;
private:
	uint64_t m_poolUpper32;
	uint32_t m_children[kAlphaRange];
public:
	int32_t m_wordIdx;
private:
	uint32_t m_padding; // Pads to 128 bytes, plus we can use it!
};

// We keep one dictionary at a time so it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;

// Counters, the latter being useful to reserve space.
static unsigned s_longestWord;
static size_t s_wordCount;

#if 0
BOGGLE_INLINE static unsigned GetWordScore_Niels(size_t length) /* const */
{
	length -= 3;
	length = length > 5 ? 5 : length; // This nicely compiles to a conditional move

	constexpr unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
	return kLUT[length];
}

BOGGLE_INLINE static unsigned GetWordScore_Albert_1(size_t length) /* const */
{
	length = length > 8 ? 8 : length;
	length -= 3;

	// Courtesy of Albert S.
	size_t Albert = 0;
	Albert += length>>1;
	Albert += (length+1)>>2;
	Albert += length>>2;
	Albert += ((length+3)>>3)<<2;
	Albert += ((length+3)>>3<<1);
	return unsigned(Albert+1);
	}
}
#endif

BOGGLE_INLINE static uint8_t GetWordScore_Albert_2(size_t length) /* const */
{
	length = length > 8 ? 8 : length;

	// Courtesy of Albert S.
	size_t Albert = 0;
	Albert += (length-3)>>1;
	Albert += (length+10)>>4;
	Albert += (length+9)>>4;
	Albert += length<<2>>5<<2;
	Albert += length<<2>>5<<1;
	return uint8_t(Albert+1);
}

#ifdef NED_FLANDERS
// Scoped lock for all dictionary globals.
class DictionaryLock
{
public:
	DictionaryLock() :
		m_lock(s_dictMutex) {}
private:
	std::lock_guard<std::mutex> m_lock;
};
#endif // NED_FLANDERS

// Tells us if a word adheres to the rules.
static bool IsWordValid(const std::string& word)
{
	const size_t length = word.length();

	// Word not too short?
	if (length < 3 || length > MAX_WORD_LEN)
	{
		debug_print("Invalid word because it's got less than 3 or more than %u letters: %s\n", MAX_WORD_LEN, word.c_str());
		return false;
	}

	// Check if it violates the 'Qu' rule.
	std::string mutWord = word;
	auto iQ = mutWord.find_first_of('Q');
	while (std::string::npos != iQ)
	{
		auto next = iQ+1;
		if (next == length || mutWord[next] != 'U')
		{
			debug_print("Invalid word due to 'Qu' rule: %s\n", word.c_str());
			return false;
		}

		mutWord = mutWord.substr(next);
		iQ = mutWord.find_first_of('Q');
	}

	return true;
}

// Input word must be uppercase!
/* static */ void AddWordToDictionary(const std::string& word, size_t iThread)
{
	// Word of any use given the Boggle rules?	
	if (false == IsWordValid(word))
		return;
	
	const unsigned length = unsigned(word.length());
	if (length > s_longestWord)
	{
		s_longestWord = length;
	}

	LoadDictionaryNode* node = s_threadDicts[iThread];

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;

		// Get or create child node.
		node = node->AddChild(letter, iThread);

		// Handle 'Qu' rule.
		if ('Q' == letter)
		{
			// Verified to be 'Qu' by IsWordValid().
			// Skip over 'U'.
			++iLetter;
		}
	}

	// Store word in dictionary (FIXME: less ham-fisted please).
	s_words.emplace_back(Word(GetWordScore_Albert_2(length), word));

	// Store index in node.
	node->m_wordIdx = int(s_wordCount);

	++s_threadInfo[iThread].load;
	++s_wordCount;
}

void LoadDictionary(const char* path)
{
	// If the dictionary fails to load, you'll be left with an empty dictionary.
	FreeDictionary();
	
	if (nullptr == path)
		return;

	FILE* file = fopen(path, "r");
	if (nullptr == file)
	{
		debug_print("Can not open dictionary for read access at: %s\n", path);
		return;
	}

#ifdef NED_FLANDERS
	DictionaryLock lock;
#endif
	{
		size_t iThread = kNumThreads;
		while (iThread-- > 0)
			s_threadDicts.push_back(new LoadDictionaryNode());

		int character;
		std::string word;
		std::vector<std::string> words;
	
		do
		{
			character = fgetc(file);
			if (0 != isalpha((unsigned char) character))
			{
				// Boggle tiles are simply A-Z, where Q means 'Qu'.
				word += toupper(character);
			}
			else
			{
				// We've hit EOF or a non-alphanumeric character.
				if (false == word.empty()) // Got a word?
				{
					words.push_back(word); 
					word.clear();
				}
			}
		}
		while (EOF != character);

		fclose(file);
		
		// Assign words to threads sequentially
		const size_t numWords = words.size();
		const size_t wordsPerThread = numWords/kNumThreads;

		iThread = 0;
		size_t threadNumWords = 0;

		for (const auto &word : words)
		{
			AddWordToDictionary(word, iThread);

			if (++threadNumWords > wordsPerThread)
			{
				threadNumWords = 0;
				++iThread;
			}
		}

#ifdef NED_FLANDERS		
		// Check thread load total.
		size_t count = 0;
		for (CONST auto& info : s_threadInfo)
		{
			count += info.load;
		}

		if (count != s_wordCount)
			debug_print("Thread word count %zu != total word count %zu!", count, s_wordCount);
#endif
	}

	printf("Dictionary loaded. %zu words, longest being %u characters\n", s_wordCount, s_longestWord);
}

void FreeDictionary()
{
#ifdef NED_FLANDERS
	DictionaryLock lock;
#endif
	{
		// Delete roots;
		for (auto* root : s_threadDicts) 
			delete root;

		s_threadDicts.clear();

		// Reset thread information.		
		s_threadInfo.resize(kNumThreads, std::move(ThreadInfo()));

		// Reset counters.
		s_longestWord = 0;
		s_wordCount = 0;
	}
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.

#ifdef _WIN32
	#include <windows.h>
	#include <emmintrin.h>
#elif __GNUC__
	#include <pthread.h>

	#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		#include "sse2neon-02-01-2022/sse2neon.h" 
	#else // Most likely X86/X64
		#include <emmintrin.h>
	#endif
#endif


class Query
{
public:
	Query(Results& results, const char* sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_sanitized(sanitized)
,		m_width(width)
,		m_height(height) {}

	~Query() {}

public:
	// FIXME: slim this down, fix notation! 
	class ThreadContext
	{
	public:
		CUSTOM_NEW_THREAD(s_iThread)
		CUSTOM_DELETE_THREAD(s_iThread)

		ThreadContext(unsigned iThread, const Query* instance) :
		width(instance->m_width)
,		height(instance->m_height)
,		sanitized(instance->m_sanitized)
,		visited(nullptr)
,		m_iThread(iThread)
		{
			Assert(iThread < kNumThreads);
			Assert(nullptr != instance);

			s_iThread = m_iThread;
		}

		~ThreadContext() 
		{
			// We don't have to free this memory
//			s_threadCustomAlloc[m_iThread]->FreeUnsafe(visited);
		}

		void OnExecuteThread()
		{
			// Handle allocation and initialization of grid memory (local to our thread, again).
			const auto gridSize = width*height;
			visited = static_cast<char*>(s_threadCustomAlloc[m_iThread].AllocateAlignedUnsafe(gridSize, kCacheLine));
			memcpy(visited, sanitized, gridSize);

			// Reserve
			wordsFound.reserve(s_threadInfo[m_iThread].load); 
		}

		// Input
		const unsigned width, height;
		const char* sanitized;
		char* visited; // Contains letters and a few bits for state.

		// Output
		std::vector<unsigned> wordsFound;

#if defined(NED_FLANDERS)
		size_t reqStrBufSize = 0;
#endif

#if defined(DEBUG_STATS)
		unsigned maxDepth;
#endif

		const unsigned m_iThread;
	}; 

public:
	void Execute()
	{
		// Just in case another Execute() call is made on the same context: avoid leaking.
		FreeWords(m_results);

		// Bit of a step back from what it was, but as I'm picking words out of the global list now..
#ifdef NED_FLANDERS
		DictionaryLock dictLock;
#endif
		{
			// Already set
//			m_results.Count  = 0;
//			m_results.Score  = 0;

			// Kick off threads.
			std::vector<std::thread> threads;
			std::vector<ThreadContext> contexts;
			threads.reserve(kNumThreads);
			contexts.reserve(kNumThreads);

			debug_print("Kicking off %zu threads.\n", kNumThreads);
			
			for (unsigned iThread = 0; iThread < kNumThreads; ++iThread)
			{
				contexts.emplace_back(ThreadContext(iThread, this));
				threads.emplace_back(std::thread(ExecuteThread, &contexts[iThread]));

#ifdef _WIN32
				// Works as a mild stimulant, if you will

				SetThreadPriority(threads[iThread].native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
#elif defined(__GNUC__)
				// FIXME: should I?
#endif
			}

			m_results.Words = static_cast<char**>(s_globalCustomAlloc.AllocateAlignedUnsafe(s_wordCount*sizeof(char*), kCacheLine));

			for (auto& thread : threads)
				thread.join();
					
#if !defined(NED_FLANDERS)
			char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data; we'll just be copying pointers (not fool proof).

			for (const auto& context : contexts)
			{
				for (int wordIdx : context.wordsFound)
				{
					const auto& word = s_words[wordIdx];
					_mm_stream_si64((long long*) &(*words_cstr++), reinterpret_cast<long long>(word.word));
					m_results.Score += unsigned(word.score);
					++m_results.Count;
//					*words_cstr++ = const_cast<char*>(word.word);
//					*words_cstr++ = const_cast<char*>(word.word.c_str());
				}

				debug_print("Thread %u joined with %zu words (score %u).\n", context.m_iThread, contexts[context.m_iThread].wordsFound.size(), m_results.Score);
			}
#else
			size_t totReqStrBufLen = 0;
			for (const auto& context : contexts)
				totReqStrBufLen += context->reqStrBufSize;

			// Copy words to Results structure.
			// I'd rather set pointers into the dictionary, but that would break the results as soon as new dictionary is loaded.

			char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.
			char* resBuf = new char[totReqStrBufLen]; // Allocate sequential buffer.

			for (const auto& context : contexts)
			{
				const auto& wordsFound = context->wordsFound;
				for (int wordIdx : wordsFound)
				{
					const auto& word = s_words[wordIdx]; 
					*words_cstr = resBuf;
					strcpy(*words_cstr++, word.word.c_str());
					const size_t length = word.word.length();
					resBuf += length+1;
				}

				debug_print("Thread %u joined with %zu words (score %u).\n", context->iThread, wordsFound.size(), m_results.Score);
			}
#endif
		}
	}

private:
	static void ExecuteThread(ThreadContext* context);

private:
#if defined(DEBUG_STATS)
	static void BOGGLE_INLINE TraverseCall(ThreadContext& context, DictionaryNode *node, unsigned iX, unsigned offsetY, uint8_t depth);
	static void BOGGLE_INLINE TraverseBoard(ThreadContext& context, DictionaryNode *node, unsigned iX, unsigned offsetY, uint8_t depth);
#else
	static void BOGGLE_INLINE_FORCE TraverseCall(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY);
	static void BOGGLE_INLINE TraverseBoard(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY);
#endif

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;
};

/* static */ void Query::ExecuteThread(ThreadContext* context)
{
	// Initialize context
	context->OnExecuteThread();

	// Create copy of source dictionary tree
	auto threadCopy = DictionaryNode::ThreadCopy(context->m_iThread);
	auto* root = threadCopy.Get();

	// Grab stuff from context
	auto* visited = context->visited;
	const unsigned width  = context->width;
	const unsigned height = context->height;

	// Attempt to prefetch grid
	NearPrefetch(visited);

	auto& wordsFound = context->wordsFound;

#if defined(DEBUG_STATS)
	debug_print("Thread %u has a load of %zu words and %zu nodes.\n", context->iThread, s_threadInfo[context->iThread].load, s_threadInfo[context->iThread].nodes);

	context->maxDepth = 0;
#endif

	for (unsigned offsetY = 0; offsetY <= width*(height-1); offsetY += width) 
	{
		// Try to prefetch next horizontal line of board
		FarPrefetch(visited + offsetY+width);

		for (unsigned iX = 0; iX < width; ++iX) 
		{
			auto* curVisited = visited + offsetY+iX;

			// Try to prefetch next cache line in board
			NearPrefetch(curVisited + kCacheLine);
			
			if (auto* child = root->GetChildChecked(*curVisited))
			{
//				size_t before = wordsFound.size();

#if defined(DEBUG_STATS)
				unsigned depth = 0;
				TraverseBoard(*context, child, iX, offsetY, depth);
#else
				TraverseBoard(wordsFound, curVisited, child, width, height, iX, offsetY);
#endif

//				while (before < wordsFound.size())
//				{
//					root->Prune(s_words[wordsFound[before]].word);
//					++before;
//				}
			}
		}

	}

	std::sort(wordsFound.begin(), wordsFound.end());

#if defined(NED_FLANDERS)
	for (int wordIdx : wordsFound)
	{
		const size_t length = s_words[wordIdx].word.length();
		context->reqStrBufSize += length + 1; // Plus one for zero terminator
	}
#endif

#if defined(DEBUG_STATS)
	if (s_threadInfo[context->iThread].load > 0)
	{
		const float hitPct = float(wordsFound.size())/s_threadInfo[context->iThread].load;
		debug_print("Thread %u has max. traversal depth %u (max. %u), hit rate %.2f\n", context->iThread, context->maxDepth, s_longestWord, hitPct); 
	}
#endif
}

#if defined(DEBUG_STATS)
/* static */ BOGGLE_INLINE void Query::TraverseCall(ThreadContext& context, DictionaryNode *node, unsigned iX, unsigned offsetY, uint8_t depth)
#else
/* static */ BOGGLE_INLINE_FORCE void Query::TraverseCall(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY)
#endif
{
#if defined(DEBUG_STATS)
	auto* visited = context.visited + offsetY+iX;
#endif

	const unsigned tile = *visited;
	if (!(tile & kTileVisitedBit)) // Not visited?
	{
		if (auto* child = node->GetChildChecked(tile)) // With child?
		{
#if defined(DEBUG_STATS)
			TraverseBoard(context, child, iX, offsetY, depth);
#else
			TraverseBoard(wordsFound, visited, child, width, height, iX, offsetY);
#endif

			if (!child->HasChildren())
			{
				node->RemoveChild(*visited);
			}
		}
	}
}

#if defined(DEBUG_STATS)
/* static */ void BOGGLE_INLINE Query::TraverseBoard(ThreadContext& context, DictionaryNode* node, unsigned iX, unsigned offsetY, uint8_t depth)
#else
/* static */ void BOGGLE_INLINE Query::TraverseBoard(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY)
#endif
{
	Assert(nullptr != node);

#if defined(DEBUG_STATS)
	auto* visited = context.visited + offsetY+iX;

	++depth;

	Assert(depth < s_longestWord);
	context.maxDepth = std::max<unsigned>(context.maxDepth, unsigned(depth));
#endif

	const auto wordIdx = node->GetWordIndex();

	*visited |= kTileVisitedBit;

	// Recurse, as we've got a node that might be going somewhewre.
	// USUALLY the predictor does it's job and the branches aren't expensive at all.

#if defined(DEBUG_STATS)
	const auto width = context.width;
	const auto heightMinOne = context.height-1;
	const bool xSafe = iX < width-1;
	const bool ySafe = offsetY < width*heightMinOne;

	if (iX > 0)
		TraverseCall(context, node, iX-1, offsetY, depth);

	if (xSafe)
		TraverseCall(context, node, iX+1, offsetY, depth);

	if (offsetY >= width) 
	{
		if (iX > 0) 
			TraverseCall(context, node, iX-1, offsetY-width, depth);

		TraverseCall(context, node, iX, offsetY-width, depth);

		if (xSafe)
			TraverseCall(context, node, iX+1, offsetY-width, depth);
	}

	if (ySafe)
	{
		if (iX > 0)
			TraverseCall(context, node, iX-1, offsetY+width, depth);

		TraverseCall(context, node, iX, offsetY+width, depth);

		if (xSafe) 
			TraverseCall(context, node, iX+1, offsetY+width, depth);
	}
#else
	// 1st [ X X X ]
	// 2nd [ X O X ]
	// 3rd [ X X X ]

	if (offsetY >= width) 
	{
		if (iX > 0) TraverseCall(wordsFound, visited - width-1, node, width, height, iX-1, offsetY-width);
		TraverseCall(wordsFound, visited - width, node, width, height, iX, offsetY-width);
		if (iX < width-1) TraverseCall(wordsFound, visited - width+1, node, width, height, iX+1, offsetY-width);
	}

	if (iX > 0)
	{
		TraverseCall(wordsFound, visited-1, node, width, height, iX-1, offsetY);
	}

	if (iX < width-1) 
	{
		TraverseCall(wordsFound, visited+1, node, width, height, iX+1, offsetY);
	}

	if (offsetY < width*(height-1))
	{
		if (iX > 0) TraverseCall(wordsFound, visited + width-1, node, width, height, iX-1, offsetY+width);
		TraverseCall(wordsFound, visited + width, node, width, height, iX, offsetY+width);
		if (iX < width-1) TraverseCall(wordsFound, visited + width+1, node, width, height, iX+1, offsetY+width);
	}
#endif

#if defined(DEBUG_STATS)
	--depth;
#endif

	*visited ^= kTileVisitedBit;

	if (wordIdx >= 0) 
	{
#if defined(DEBUG_STATS)
		context.wordsFound.push_back(wordIdx);
#else
		wordsFound.push_back(wordIdx);
#endif

		node->m_wordIdx = -1;
		node->PruneReverse();
	}
}

Results FindWords(const char* board, unsigned width, unsigned height)
{
	debug_print("Using debug prints, takes a little off the performance.\n");

#if !defined(NED_FLANDERS)
	static bool warned = false;
	if (false == warned)
	{
		printf("Built without the NED_FLANDERS define, so the safety measures are largely off.\n");
		warned = true;
	}
#endif

#if defined(NED_FLANDERS)
	const size_t nodeSize = sizeof(DictionaryNode);
	debug_print("Node size: %zu\n", nodeSize);
#endif
	
	Results results;
	results.Words = nullptr;
	results.Count = 0;
	results.Score = 0;
	results.UserData = nullptr; // Didn't need it in this implementation.

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		const unsigned gridSize = width*height;

#ifdef NED_FLANDERS
		char* sanitized = static_cast<char*>(s_globalCustomAlloc.AllocateAligned(gridSize*sizeof(char), kCacheLine));

		// Sanitize that checks for illegal input and uppercases.
		size_t index = 0;
		for (unsigned iY = 0; iY < height; ++iY)
		{
			for (unsigned iX = 0; iX < width; ++iX)
			{
				// FIXME: does not check for 'u'!
				const char letter = board[index];
				if (0 != isalpha((unsigned char) letter))
				{
					const unsigned sanity = LetterToIndex(toupper(letter));
					sanitized[index] = sanity;
				}
				else
				{
					// Invalid character: skip query.
					return results;
				}

				++index;
			}
		}
#else
		char* sanitized = static_cast<char*>(s_globalCustomAlloc.AllocateAlignedUnsafe(gridSize, kCacheLine));

		// Sanitize that just reorders and expects uppercase.
		for (unsigned index = 0; index < gridSize; ++index)
		{
			const char letter = *board++;
			const unsigned sanity = LetterToIndex(letter); // LetterToIndex(toupper(letter));
			sanitized[index] = sanity;
		}
#endif

		// Allocate allocators (yup)
		s_threadCustomAlloc.reserve(kNumThreads);
		for (auto iThread = 0; iThread < kNumThreads; ++iThread)
		{
			const size_t threadHeapSize = 
				gridSize + 
				s_threadInfo[iThread].nodes*sizeof(DictionaryNode) + 
				(s_threadInfo[iThread].load*sizeof(int)) 
				+ 1024*1024; // Enough? (FIXME)
	
#ifdef NED_FLANDERS			
			s_threadCustomAlloc.emplace_back(CustomAlloc(static_cast<char*>(s_globalCustomAlloc.Allocate(threadHeapSize, kPageSize)), threadHeapSize));
#else
			s_threadCustomAlloc.emplace_back(CustomAlloc(static_cast<char*>(s_globalCustomAlloc.AllocateAlignedUnsafe(threadHeapSize, kPageSize)), threadHeapSize));
#endif
		}

		Query query(results, sanitized, width, height);
		query.Execute();

		for (auto& allocator : s_threadCustomAlloc)
		{
			void* pool = allocator.GetPool();

#ifdef NED_FLANDERS
			s_globalCustomAlloc.Free(pool);
#else
			s_globalCustomAlloc.FreeUnsafe(pool);
#endif
			
			// Not necessary, because:
			// - Allocator context itself is contained within pool just released, 100%
			// - Allocator destructor has zero work to do
//			delete allocator;
		}

		s_threadCustomAlloc.clear();

#ifdef NED_FLANDERS
		s_globalCustomAlloc.Free(sanitized);
#else
		s_globalCustomAlloc.FreeUnsafe(sanitized);
#endif
	}

	return results;
}

void FreeWords(Results results)
{
#ifdef NED_FLANDERS
	if (0 != results.Count && nullptr != results.Words)
	{
		// Allocated a single buffer.
		delete[] results.Words[0];
	}
#else
	if (nullptr != results.Words)
		s_globalCustomAlloc.FreeUnsafe((void*) results.Words);
#endif

	results.Words = nullptr;

	results.Count = results.Score = 0;
	results.UserData = nullptr;
}
