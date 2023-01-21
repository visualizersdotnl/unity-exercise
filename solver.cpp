/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	Please check 'solver_submitted.cpp' for original submission version, this is the competition one.

	This is an optimized version, using OpenMP.

	- Always check for leaks (Windows debug build does it automatically).
	- FIXMEs.

	To do (low priority):
		- Building (or loading) my dictionary is slow(ish), I'm fine with that as I focus on the solver; or should I precalculate even more?

	Notes:
		- Currently tested on Windows 10 (VS2019), Linux & OSX.
		- Compile with full optimization (-O3 for ex.) for best performance.
		  Disabling C++ exceptions helps too, as they hinder inlining and are not used.
		- I could not assume anything about the test harness, so I did not; if you want debug output check debug_print().
		  ** I violate this to tell if this was compiled with or without NED_FLANDERS (see below).
		- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
		- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
		  is guarded by a mutex and no globals are used.
		- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.
		- My class design isn't really tight (functions and public member values galore), but for now that's fine.
		
	Most of these (mostly thread-safety related) stability claims only work if NED_FLANDERS (see below) is defined.

	Optimization ideas:
	- That memcpy() taking 3% (of whatever) is nagging me -> WIP
	- Src: https://squadrick.dev/journal/going-faster-than-memcpy.html
	- Copy() can be faster (analyze it)
	- Prefetches help, not clear if it does on ARM/Silicon
	- Streaming helps on Intel, not so much on ARM/Silicon?
	- Try 'reverse pruning' only to a certain degree (first test up to 3-letter words, then move up, maybe correlate it to an actual value (heuristic)) -> WIP

	Things about the OpenMP version:
	- Results are (FIXME) invalid as soon as a new dictionary is loaded!
	- ...
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

#include <omp.h>

#ifdef _WIN32
	#include <windows.h>
	#include <intrin.h>
	#define FOR_INTEL
#elif __GNUC__
	#include <pthread.h>

	#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		#include "sse2neon-02-01-2022/sse2neon.h" 
		#define FOR_ARM
	#else // Most likely X86/X64
		#include <intrin.h>
		#define FOR_INTEL
	#endif
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

// Undef. to kill assertions.
// #define ASSERTIONS

// Undef. to enable streamed (non-temporal) writes
#if defined(FOR_INTEL)
	#define STREAM_WRITES
#endif

// Set to 0 to use index of letter 'U' instead of extra 4 bytes
#define USE_EXTRA_INDEX 0

// static thread_local unsigned s_iThread;       // Dep. for thread heaps.
#define GLOBAL_MEMORY_POOL_SIZE 1024*1024*2000   // Just allocate as much as we can in 1 go.
#include "custom-allocator.h"                    // Depends on Ned Flanders & co. :)

constexpr size_t kAlignTo = 16; // 128-bit
constexpr size_t kCacheLineSize = sizeof(size_t)*8;

// Undef. to kill prefetching (to do: read up on ARM/Silicon and prefetches)
// #define NO_PREFETCHES 

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

const size_t kNumConcurrrency = omp_get_max_threads();
const size_t kNumThreads = kNumConcurrrency+(kNumConcurrrency/2);

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
#if defined(__GNUC__)
	__builtin_prefetch(address, 1, 0);
#elif defined(_WIN32)
	_mm_prefetch(address, _MM_HINT_NTA);
#endif
}

#else

BOGGLE_INLINE_FORCE static void FarPrefetch(const char* address)  {}
BOGGLE_INLINE_FORCE static void NearPrefetch(const char* address) {}
BOGGLE_INLINE_FORCE static void ImmPrefetch(const char* address)  {}

#endif


// Dictionary word (score is best precalculated)
class Word
{
public:
	Word(unsigned score, const std::string& word) :
	score(score)
//,	word(word)
	{
		strcpy(this->word, word.c_str());
	}
	
	size_t score;
	char word[MAX_WORD_LEN+1];

//	std::string word;
//	uint8_t padding[2]; // Pad to 32 bytes
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

constexpr unsigned kAlphaRange = ('Z'-'A')+1;

// Cheap way to tag along the tiles (few bits left)
constexpr unsigned kTileVisitedBit = 1<<7;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
BOGGLE_INLINE_FORCE unsigned LetterToIndex(unsigned letter)
{
	return USE_EXTRA_INDEX + (letter - static_cast<unsigned>('A'));
}

constexpr auto kIndexU = 'U'-'A'; // LetterToIndex('U');

#if 0 == USE_EXTRA_INDEX
	constexpr auto kIndexParent = kIndexU;
#else
	constexpr auto kIndexParent = 0;
#endif

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
		for (auto* child : m_children)
			delete child;
	}

	// Only called from LoadDictionary().
	LoadDictionaryNode* AddChild(char letter, size_t iThread)
	{
		const unsigned index = LetterToIndex(letter);

		// Adding child en route to a new word, so up the ref. count
		const unsigned bit = 1 << index;
		if (m_indexBits & bit)
		{
			auto* child = m_children[index-USE_EXTRA_INDEX];
			return child;
		}

		++s_threadInfo[iThread].nodes;

		m_indexBits |= bit;

		return m_children[index-USE_EXTRA_INDEX] = new LoadDictionaryNode();
	}

	BOGGLE_INLINE_FORCE LoadDictionaryNode* GetChild(unsigned index)
	{
		Assert(nullptr != m_children[index-USE_EXTRA_INDEX]);
		return m_children[index-USE_EXTRA_INDEX];
	}

private:
	uint32_t m_indexBits = 0;
	int32_t m_wordIdx = -1; 
	LoadDictionaryNode* m_children[kAlphaRange] = { nullptr };
};

// Actual processing node.
class DictionaryNode
{
	friend class LoadDictionaryNode;

public:
	DictionaryNode() {}

	class ThreadCopy
	{
	public:
		ThreadCopy(unsigned iThread) 
		{
			// Allocate pool for all necessary nodes: why here, you glorious titwillow?
			// Well, it sits nice and snug on it's on (probably) page boundary aligning nicely with this thread's cache as opposed to allocating
			// one huge block at once.
			const auto size = s_threadInfo[iThread].nodes*sizeof(DictionaryNode);
			m_pool = static_cast<DictionaryNode*>(s_threadCustomAlloc[iThread].AllocateAlignedUnsafe(size, kAlignTo));

#if _DEBUG
			m_pool->m_children[kIndexParent] = 0xcdcdcdcd;
#endif

			// Recursively copy them.
			Copy(s_threadDicts[iThread]);
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
		BOGGLE_INLINE_FORCE uint32_t Copy(LoadDictionaryNode* parent)
		{
			DictionaryNode* node = m_pool + m_iAlloc++;

			unsigned indexBits = node->m_indexBits = parent->m_indexBits;
			node->m_wordIdx = parent->m_wordIdx;

			// Yes, you're seeing this correctly, we're chopping a 64-bit pointer in half.
			// Quite volatile, but usually works out fine.
			node->m_poolUpper32 = reinterpret_cast<uint64_t>(m_pool) & 0xffffffff00000000;

			const uint32_t nodeLower32 = reinterpret_cast<intptr_t>(node) & 0xffffffff;

//			if (indexBits > 0)
			{
#ifdef _WIN32
				unsigned long index;
				if (_BitScanForward(&index, indexBits))
				{
#elif defined(__GNUC__)
				int index = __builtin_ffs(int(indexBits));
				if (index--)
				{
#endif
					for (indexBits >>= index; index < kAlphaRange+USE_EXTRA_INDEX; ++index, indexBits >>= 1)
					{
						if (indexBits & 1)
							node->m_children[index] = Copy(parent->GetChild(index));
					}
				}
			}

			return nodeLower32;
		}

	private:
		DictionaryNode* m_pool;
		size_t m_iAlloc = 0;
	};

	// Destructor is not called when using ThreadCopy!
	~DictionaryNode() = delete;

public:
	BOGGLE_INLINE_FORCE unsigned HasChildren() const { 
		return m_indexBits;  
	}

#if 0
	BOGGLE_INLINE_FORCE void PruneReverse()
	{
		DictionaryNode* current = this;

		do
		{
			const uint32_t rootLower32 = current->m_children[kIndexParent];

			if (0 == current->m_wordRefCount-1)
				current->m_indexBits = 0;

#if defined(STREAM_WRITES)
			// This should make sense since we're not going to read this value for a while:
			_mm_stream_si32(&current->m_wordRefCount, current->m_wordRefCount-1);
#else
			--current->m_wordRefCount;
#endif

			current = reinterpret_cast<DictionaryNode*>(m_poolUpper32|rootLower32);
		}
		while (reinterpret_cast<intptr_t>(current) & 0xffffffff);
	}
#endif

	BOGGLE_INLINE_FORCE void RemoveChild(unsigned index)
	{
		Assert(index < kAlphaRange+USE_EXTRA_INDEX);
		Assert(HasChild(index));

		m_indexBits ^= 1 << index;
	}

	// Returns non-zero if true.
	BOGGLE_INLINE_FORCE unsigned HasChild(unsigned index) const
	{
		Assert(index < kAlphaRange+USE_EXTRA_INDEX);
		return m_indexBits & (1 << index);
	}

	BOGGLE_INLINE_FORCE DictionaryNode* GetChild(unsigned index) const
	{
#if 1 == USE_EXTRA_INDEX
		Assert(0 == index || HasChild(index));
#else
		Assert(HasChild(index));
#endif

		return reinterpret_cast<DictionaryNode*>(m_poolUpper32|m_children[index]);
	}

	// Returns NULL if no child.
	BOGGLE_INLINE_FORCE DictionaryNode* GetChildChecked(unsigned index) const
	{
		if (!HasChild(index))
			return nullptr;
		else
		{
			return reinterpret_cast<DictionaryNode*>(m_poolUpper32|m_children[index]); // This dirty trick courtesty of Alex B.
		}
	}

	// If not -1, it's pointing to a word.
	BOGGLE_INLINE_FORCE int32_t GetWordIndex() const
	{
		return m_wordIdx;
	}

	BOGGLE_INLINE_FORCE void OnWordFound()
	{
		m_wordIdx = -1;
	}

private:
	uint32_t m_indexBits;
	uint64_t m_poolUpper32;
	uint32_t m_children[kAlphaRange+USE_EXTRA_INDEX];
	int32_t m_wordIdx; // Sits on a 16-byte boundary
};

// Keep the above exactly 128 bytes, keep it that way!
static_assert(sizeof(DictionaryNode) == 128);

// We keep one dictionary at a time so it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;

// Counters, the latter being useful to reserve space.
static unsigned s_longestWord;
static size_t s_wordCount;

BOGGLE_INLINE static unsigned GetWordScore_Readable(size_t length) /* const */
{
	length -= 3;
	length = length > 5 ? 5 : length; // This nicely compiles to a conditional move

	constexpr unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
	return kLUT[length];
}

BOGGLE_INLINE static uint8_t GetWordScore_Albert(size_t length) /* const */
{
	length = length > 8 ? 8 : length;

	// Courtesy of Albert Sandberg, the bit magician.
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
	Assert(nullptr != node);

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
	s_words.emplace_back(Word(GetWordScore_Albert(length), word));

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
		for (auto iThread = 0; iThread < kNumThreads; ++iThread)
			s_threadDicts.push_back(new LoadDictionaryNode()); // Allocated in AddWordToDictionary() for (ever so slightly) better locality

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

		unsigned iThread = 0;
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
			count += info.load;

		if (count != s_wordCount)
			debug_print("Thread word count (load) %zu != total word count %zu!", count, s_wordCount);
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
		// Delete per-thread dictionary trees.
		for (auto* root : s_threadDicts)
			delete root;

		s_threadDicts.clear();

		// Reset thread information.
		s_threadInfo.resize(kNumThreads);

		// Reset counters.
		s_longestWord = 0;
		s_wordCount = 0;
	}
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.

class Query
{
public:
	Query(Results& results, const char* sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_sanitized(sanitized)
,		m_width(width)
,		m_height(height) {}

	~Query() {}

	void ExecuteThread(unsigned iThread, std::vector<unsigned>& wordsFound);

	void Execute()
	{
#ifdef NED_FLANDERS
		// Just in case another Execute() call is made on the same context: avoid leaking.
		FreeWords(m_results);

		// Bit of a step back from what it was, but as I'm picking words out of the global list now..
		DictionaryLock dictLock;
#endif
		{
			unsigned Score = 0;
			unsigned Count = 0;

			// I'll be copying pointers, plain and simple, but not the safest given the API.
			m_results.Words = static_cast<char**>(s_globalCustomAlloc.AllocateAlignedUnsafe(s_wordCount*sizeof(char*), kAlignTo));
			char** words_cstr = const_cast<char**>(m_results.Words); 

#if defined(NED_FLANDERS) // WIP!
			m_reqStrBufSize = 0;
#endif

			#pragma omp parallel for reduction(+:Count) reduction(+:Score) schedule(static, 1) num_threads(int(kNumThreads))
			for (int iThread = 0; iThread < kNumThreads; ++iThread)
			{
				std::vector<unsigned> wordsFound;
				wordsFound.reserve(s_threadInfo[iThread].load);
				ExecuteThread(iThread, wordsFound);
				std::sort(wordsFound.begin(), wordsFound.end());

				for (const auto wordIdx : wordsFound)
				{
					const auto& word = s_words[wordIdx];
					
					Count += 1;
					Score += unsigned(word.score);

#if defined(STREAM_WRITES)
					_mm_stream_si64((long long*) &(*words_cstr++), reinterpret_cast<long long>(word.word));
//					_mm_stream_si64((long long*) &(*words_cstr++), reinterpret_cast<long long>(word.word.c_str()));
#else
					*words_cstr++ = const_cast<char*>(word.word);
//					*words_cstr++ = const_cast<char*>(word.word.c_str());
#endif
				}
				debug_print("Thread %u completed with %zu words.\n", iThread, wordsFound.size());
			}

			m_results.Count = Count;
			m_results.Score = Score;
		}
	}

private:
#if defined(DEBUG_STATS)
	void BOGGLE_INLINE_FORCE TraverseCall(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY, uint8_t depth);
	void BOGGLE_INLINE TraverseBoard(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY, uint8_t depth);
#else
	void BOGGLE_INLINE_FORCE TraverseCall(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY);
	void BOGGLE_INLINE TraverseBoard(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY);
#endif

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;

#if defined(NED_FLANDERS)
	size_t m_reqStrBufSize;
#endif

#if defined(DEBUG_STATS)
	unsigned m_maxDepth;
#endif
};

void Query::ExecuteThread(unsigned iThread, std::vector<unsigned>& wordsFound)
{
	const unsigned width  = m_width;
	const unsigned height = m_height;

	// Create copy of dictionary tree for this thread
	const auto threadCopy = DictionaryNode::ThreadCopy(iThread);
	auto* root = threadCopy.Get();

#if defined(DEBUG_STATS)
	debug_print("Thread %u has a load of %zu words and %zu nodes.\n", iThread, s_threadInfo[iThread].load, s_threadInfo[iThread].nodes);
	m_maxDepth = 0;
#endif

	// Allocate & copy grid (no necessity to free it, thread heaps will be discarded as a whole afterwards)
	const auto gridSize = width*height;
	char* visited = static_cast<char*>(s_threadCustomAlloc[iThread].AllocateAlignedUnsafe(gridSize, kAlignTo));
	memcpy(visited, m_sanitized, gridSize);

	for (unsigned offsetY = 0; offsetY <= width*(height-1); offsetY += m_width) 
	{
		// Try to prefetch next horizontal line of board in advance
		FarPrefetch(visited + offsetY+width);

		for (int iX = 0; iX < int(width); ++iX) 
		{
			if (auto* child = root->GetChildChecked(visited[offsetY+iX]))
			{
#if defined(DEBUG_STATS)
				TraverseBoard(wordsFound, &visited[offsetY+iX], child, width, height, iX, offsetY, 1);
#else
				TraverseBoard(wordsFound, &visited[offsetY+iX], child, width, height, iX, offsetY);
#endif
			}

			// Try to prefetch next cache line
			NearPrefetch(visited + offsetY+iX+kCacheLineSize);
		}
	}

#if defined(NED_FLANDERS)
	for (unsigned wordIdx : wordsFound)
	{
		const size_t length = strlen(s_words[wordIdx].word);
//		const size_t length = s_words[wordIdx].word.length(); 
		m_reqStrBufSize += length + 1; // Plus one for zero terminator
	}
#endif

#if defined(DEBUG_STATS)
	if (s_threadInfo[iThread].load > 0)
	{
		const float hitPct = float(wordsFound.size())/s_threadInfo[iThread].load;
		debug_print("Thread %u has max. traversal depth %u (max. %u), hit rate %.2f\n", iThread, m_maxDepth, s_longestWord, hitPct); 
	}
#endif
}

#if defined(DEBUG_STATS)
BOGGLE_INLINE_FORCE void Query::TraverseCall(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY, uint8_t depth)
#else
BOGGLE_INLINE_FORCE void Query::TraverseCall(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY)
#endif
{
	if (!(*visited & kTileVisitedBit)) // Not visited?
	{
		if (auto* child = node->GetChildChecked(*visited)) // With child?
		{
#if defined(DEBUG_STATS)
			TraverseBoard(wordsFound, visited, child, width, height, iX, offsetY, depth);
#else
			TraverseBoard(wordsFound, visited, child, width, height, iX, offsetY);
#endif

			if (!child->HasChildren())
				node->RemoveChild(*visited);
		}
	}
}

#if defined(DEBUG_STATS)
void BOGGLE_INLINE Query::TraverseBoard(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY, uint8_t depth)
#else
void BOGGLE_INLINE Query::TraverseBoard(std::vector<unsigned>& wordsFound, char* visited, DictionaryNode* node, unsigned width, unsigned height, unsigned iX, unsigned offsetY)
#endif
{
	Assert(nullptr != node);

	const auto wordIdx = node->GetWordIndex();

#if defined(DEBUG_STATS)
	++depth;
	Assert(depth <= s_longestWord);
	m_maxDepth = std::max<unsigned>(m_maxDepth, depth);
#endif

	// Flag tile as visited while we traverse in search of a word (the branch predictor does a good enough job below).
	*visited |= kTileVisitedBit;

#if defined(DEBUG_STATS)
	if (offsetY >= width) 
	{
		if (iX < width-1) 
			TraverseCall(wordsFound, (visited - width) + 1, node, width, height, iX+1, offsetY-width, depth);

		TraverseCall(wordsFound, visited - width, node, width, height, iX, offsetY-width, depth);
		
		if (iX > 0) 
			TraverseCall(wordsFound, (visited - width) - 1, node, width, height, iX-1, offsetY-width, depth);
	}

	if (iX > 0)
		TraverseCall(wordsFound, visited-1, node, width, height, iX-1, offsetY, depth);

	if (iX < width-1) 
		TraverseCall(wordsFound, visited+1, node, width, height, iX+1, offsetY, depth);

	if (offsetY < width*(height-1))
	{
		if (iX < width-1) 
			TraverseCall(wordsFound, (visited + width) + 1, node, width, height, iX+1, offsetY+width, depth);

		TraverseCall(wordsFound, visited + width, node, width, height, iX, offsetY+width, depth);

		if (iX > 0) 
			TraverseCall(wordsFound, (visited + width) - 1, node, width, height, iX-1, offsetY+width, depth);
	}
#else
	if (offsetY >= width) 
	{
		if (iX < width-1) 
			TraverseCall(wordsFound, (visited - width) + 1, node, width, height, iX+1, offsetY-width);

		TraverseCall(wordsFound, visited - width, node, width, height, iX, offsetY-width);

		if (iX > 0) 
			TraverseCall(wordsFound, (visited - width) - 1, node, width, height, iX-1, offsetY-width);
	}

	if (iX > 0)
		TraverseCall(wordsFound, visited-1, node, width, height, iX-1, offsetY);

	if (iX < width-1) 
		TraverseCall(wordsFound, visited+1, node, width, height, iX+1, offsetY);

	if (offsetY < width*(height-1))
	{
		if (iX < width-1) 
			TraverseCall(wordsFound, (visited + width) + 1, node, width, height, iX+1, offsetY+width);

		TraverseCall(wordsFound, visited + width, node, width, height, iX, offsetY+width);

		if (iX > 0) 
			TraverseCall(wordsFound, (visited + width) - 1, node, width, height, iX-1, offsetY+width);
	}
#endif

	// Done!
	*visited ^= kTileVisitedBit;

	// Word found?
	if (wordIdx >= 0) 
	{
		node->OnWordFound();
		wordsFound.emplace_back(wordIdx);
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
		char* sanitized = static_cast<char*>(s_globalCustomAlloc.AllocateAligned(gridSize*sizeof(char), kAlignTo));

		bool invalidBoard = false;

		// Sanitize that checks for illegal input and uppercases.
		for (unsigned iY = 0; iY < height; ++iY)
		{
			#pragma omp parallel for num_threads(4)
			for (int iX = 0; iX < int(width); ++iX)
			{
				const unsigned index = iY*width + iX;

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
					invalidBoard = true;
					break;				}
			}
		}

		if (true == invalidBoard)
			return results; // Skip query: no results.
#else
		char* sanitized = static_cast<char*>(s_globalCustomAlloc.AllocateAlignedUnsafe(gridSize, kAlignTo));

		// Sanitize that just reorders and expects uppercase.
		for (unsigned index = 0; index < gridSize; ++index)
		{
			const char letter = *board++;
			const unsigned sanity = LetterToIndex(letter); // LetterToIndex(toupper(letter));
			sanitized[index] = sanity;
		}
#endif

		// Allocate for per-thread allocators
		const size_t overhead = tlsf_alloc_overhead();
		s_threadCustomAlloc.reserve(kNumThreads);
		for (auto iThread = 0; iThread < kNumThreads; ++iThread)
		{
			const size_t threadHeapSize = 
				gridSize + overhead +                                           // Visited grid
				s_threadInfo[iThread].nodes*sizeof(DictionaryNode) + overhead + // Dictionary nodes
				1024*1024*1;                                                    // For overhead and alignment
	
#ifdef NED_FLANDERS			
			s_threadCustomAlloc.emplace_back(CustomAlloc(static_cast<char*>(s_globalCustomAlloc.AllocateAligned(threadHeapSize, kPageSize)), threadHeapSize));
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
			
			// Not necessary to delete 'allocator', because:
			// - Allocator context itself is contained within pool just released, 100%
			// - Thus, the CustomAllocator destructor has zero work to do
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
	if (nullptr != results.Words)
		s_globalCustomAlloc.FreeUnsafe((void*) results.Words);

	results.Words = nullptr;
	results.Count = results.Score = 0;
}
