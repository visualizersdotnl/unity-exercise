/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	Then improved more than a few times later.
	
	Please take a minute to read this piece of text.

	Rules:
		- Only use the same word once.
		- Can't reuse a letter in the same word.
		- There's no 'Q', but there is 'Qu'.
		- Words must be at least 3 letters long.

	Scoring (original):
		- 3, 4 = 1
		- 5 = 2
		- 6 = 3
		- 7 = 5
		- >7 = 11

	"For the purposes of scoring Qu counts as two letters: squid would score two points (for a five-letter word) despite being formed from a 
	chain of only four cubes."

	Rules and scoring taken from Wikipedia.

	To do (high priority):
		- Optimization & clean up.
		- Always check for leaks (Windows debug build does it automatically).
		- FIXMEs.

	To do (low priority):
		- Check compile & run status on Linux and OSX: @Albert!
		- Building (or loading) my dictionary is slow(ish), I'm fine with that as I focus on the solver.
		- Fix class members (notation).

	There's this idea floating that if you have a hash and eliminate a part of the dict. tree that way
	by special-casing the first 3 characters you're golden, but it did not really help at all plus it 
	makes the algorithm less flexible. What if someone decides to change the rules? ;)

	Notes:
		- ** Currently only tested on Windows 10, VS2017 + Linux & OSX **
		- It's currently faster on a proper multi-core CPU than the Core M, probably due to those allocator locks.
		- Compile with full optimization (-O3 for ex.) for best performance.
		  Disabling C++ exceptions helps too, as they hinder inlining and are not used.
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

	- This is a non-Morton version that now handily outperforms the Morton version.
	- I must try a pool of sequential nodes instead of loose allocations, with a dictionary instance as their parent, which causes cache misses.
	- I've used "__forceinline" to manhandle MSVC, which obviously won't fly on OSX/Linux.
	- There's quite a bit of branching going on but trying to be smart doesn't always please the predictor/pipeline the best way, rather it's quite fast
	  in critical places because the predictor can do it's work very well (i.e. the cases lean 99% towards one side).
	- Just loosely using per-thread instances of CustomAlloc doesn't improve performance, I still think it could work because it enhances
	  locality and eliminates the need for those mutex locks.
	- What *would* help is getting rid of all those copied DictionaryNode instances in one go instead of that horrendous delete chain.
*/

// Make VC++ 2015 shut up and walk in line.
#define _CRT_SECURE_NO_WARNINGS 

/*
	Albert: at this point detect that you're using GNU C or whatever, but don't just kill __forceinline like you did in the
	        simple allocator header because that doesn't work.
*/

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>
#include <iostream>
#include <mutex>
#include <thread>
// #include <atomic>

// #include <unordered_map>
// #include <unordered_set>
// #include <deque>
// #include <list>
// #include <set>
// #include <stack>
// #include <unordered_set>
#include <vector>
// #include <array>
#include <algorithm>

#include <cassert>
	
#include "api.h"
#include "random.h"
// #include "alloc-aligned.h"
#include "bit-tricks.h"
#include "simple-tlsf.h"

// Undef. to skip dead end percentages and all prints and such.
// #define DEBUG_STATS

// Undef. to enable all the work I put in to please a, as it turns out, very forgiving test harness.
// But basically the only gaurantee here is that this works with my own test!
// #define NED_FLANDERS

// Undef. to use only 1 thread.
// #define SINGLE_THREAD

// Undef. to kill assertions.
// #define ASSERTIONS

#if defined(_DEBUG) || defined(ASSERTIONS)
	#define Assert assert
#else
	inline void Assert(bool condition) {}
#endif

#if defined(DEBUG_STATS)
	#define debug_print printf
#else
	inline void debug_print(const char* format, ...) {}
#endif

#if defined(SINGLE_THREAD)
	constexpr size_t kNumThreads = 1;
#else
	const size_t kNumCores = std::thread::hardware_concurrency();
	const size_t kNumThreads = kNumCores*2; // FIXME: this speeds things up on my Intel I7
#endif

constexpr size_t kCacheLine = sizeof(size_t)*8;

constexpr unsigned kAlphaRange = ('Z'-'A')+1;

// Number of words and number of nodes (1 for root) per thread.
class ThreadInfo
{
public:
	ThreadInfo() : 
		load(0) {}

	size_t load;
};

static std::vector<ThreadInfo> s_threadInfo;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
__forceinline unsigned LetterToIndex(char letter)
{
	return letter - 'A';
}

// FWD.
static void AddWordToDictionary(const std::string& word);

class DictionaryNode
{
	friend void AddWordToDictionary(const std::string& word);
	friend void FreeDictionary();

public:
	CUSTOM_NEW
	CUSTOM_DELETE

	DictionaryNode() : 
		m_wordIdx(-1)
,		m_indexBits(0)
	{
		memset(m_children, 0, sizeof(DictionaryNode*)*kAlphaRange);
	}

	DictionaryNode(size_t wordIdx, unsigned indexBits) :
		m_wordIdx(wordIdx)
,		m_indexBits(indexBits)
	{
		memset(m_children, 0, sizeof(DictionaryNode*)*kAlphaRange);
	}

	// This copy function uses the global (custom) allocator
	static DictionaryNode* DeepCopy(DictionaryNode* parent)
	{
		Assert(nullptr != parent);

		DictionaryNode* node = new DictionaryNode(parent->m_wordIdx, parent->m_indexBits);

		unsigned indexBits = node->m_indexBits;
		unsigned index = 0;
		while (indexBits)
		{
			if (indexBits & 1)
			{
				node->m_children[index] = DeepCopy(parent->GetChild(index));
			}

			indexBits >>= 1;
			++index;
		}

		return node;
	}

	~DictionaryNode()
	{
		// FIXME
		// This just looks awful and performs likewise; I need to use a separate allocator
		// to allocate all children so I can discard them in 1 go.
		// Just ignoring this because the allocator cleans up after itself anyway don't work
		// either as it's performance drops due to all the abandoned allocations.
		for (unsigned iChar = 0; iChar < kAlphaRange; ++iChar)
			delete m_children[iChar];
	}

private:
	// Only called from LoadDictionary().
	DictionaryNode* AddChild(char letter)
	{
		const unsigned index = LetterToIndex(letter);

		const unsigned bit = 1 << index;
		if (m_indexBits & bit)
			return m_children[index];

		m_indexBits |= bit;
		return m_children[index] = new DictionaryNode();
	}

public:
	__inline unsigned HasChildren() const { return m_indexBits;                    } // Non-zero.
	__inline bool     IsWord()      const { return -1 != m_wordIdx;                }
	__inline int      IsVoid()      const { return int(m_indexBits+m_wordIdx)>>31; } // Is 1 (sign bit) if zero children (0) and no word (-1).

	// Returns zero if node is now a dead end.
	__inline unsigned RemoveChild(unsigned index)
	{
		Assert(index < kAlphaRange);
		const unsigned bit = 1 << index;
		m_indexBits ^= bit;
		return m_indexBits;
	}

	// Returns non-zero if true.
	__inline unsigned HasChild(unsigned index)
	{
		Assert(index < kAlphaRange);
		const unsigned bit = 1 << index;
		return m_indexBits & bit;
	}

	__inline DictionaryNode* GetChild(unsigned index)
	{
		Assert(HasChild(index));
		return m_children[index];
	}

	// Returns index and wipes it (eliminating need to do so yourself whilst not changing a negative outcome)
	__inline size_t GetWordIndex() /*  const */
	{
		const auto index = m_wordIdx;
		m_wordIdx = -1;
		return index;
	}

private:
	size_t m_wordIdx;
	unsigned m_indexBits;
	DictionaryNode* m_children[kAlphaRange];
};

// We keep one dictionary at a time so it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;

// A root per thread.
static std::vector<DictionaryNode*> s_dictRoots;

// Sequential dictionary of all full words (FIXME: yet unsorted).
static std::vector<std::string> s_dictionary;

// Counters, the latter being useful to reserve space.
static unsigned s_longestWord;
static size_t s_wordCount;

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
#else
class DictionaryLock {};
#endif // NED_FLANDERS

// Tells us if a word adheres to the rules.
static bool IsWordValid(const std::string& word)
{
	const size_t length = word.length();

	// Word not too short?
	if (length < 3)
	{
		debug_print("Invalid word because it's got less than 3 letters: %s\n", word.c_str());
		return false;
	}

	// Check if it violates the 'Qu' rule.
	auto iQ = word.find_first_of('Q');
	while (std::string::npos != iQ)
	{
		auto next = iQ+1;
		if (next == length || word[next] != 'U')
		{
			debug_print("Invalid word due to 'Qu' rule: %s\n", word.c_str());
			return false;
		}

		iQ = word.substr(next).find_first_of('Q');
	}

	return true;
}

// Input word must be uppercase!
static void AddWordToDictionary(const std::string& word)
{
	// Word of any use given the Boggle rules?	
	if (false == IsWordValid(word))
		return;
	
	const unsigned length = unsigned(word.length());
	if (length > s_longestWord)
	{
		s_longestWord = length;
	}

	// As a first strategy we'll split at the root.
	// As it turns out this simple way of dividing the load works fairly well, but it's up for review (FIXME).
	const char firstLetter = word[0];
	unsigned iThread = 0;
	if ('Q' == firstLetter)
	{
		// Otherwise this thread, for 'U', would remain unused.
		if (kNumThreads > 20)
			iThread = (rand() > 16384) ? 16 : 20;
	}
	else
		iThread = LetterToIndex(firstLetter)%kNumThreads;
	
	DictionaryNode* node = s_dictRoots[iThread];

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;

		// Get or create child node.
		node = node->AddChild(letter);

		// Handle 'Qu' rule.
		if ('Q' == letter)
		{
			// Verified to be 'Qu' by IsWordValid().
			// Skip over 'U'.
			++iLetter;
		}
	}

	// Store.
	s_dictionary.emplace_back(word);
	node->m_wordIdx = s_wordCount;

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

	DictionaryLock lock;
	{
		size_t iThread = kNumThreads;
		while (iThread-- > 0)
			s_dictRoots.emplace_back(new DictionaryNode());

		int character;
		std::string word;

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
					AddWordToDictionary(word);
					word.clear();
				}
			}
		}
		while (EOF != character);

		fclose(file);

		// Check thread load total.
		size_t count = 0;
		for (auto& info : s_threadInfo)
		{
			count += info.load;
		}
		
		if (count != s_wordCount)
			debug_print("Thread word count %zu != total word count %zu!", count, s_wordCount);
	}

	debug_print("Dictionary loaded. %zu words, longest being %u characters\n", s_wordCount, s_longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;
	{
		// Delete roots;
		for (auto* root : s_dictRoots) 
			delete root;

		s_dictRoots.clear();

		// Reset thread information.		
		s_threadInfo.resize(kNumThreads, ThreadInfo());

		// Reset counters.
		s_longestWord = 0;
		s_wordCount = 0;
	}
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.

#include <emmintrin.h>

class Query
{
public:
	CUSTOM_NEW
	CUSTOM_DELETE

	Query(Results& results, const char* sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_sanitized(sanitized)
,		m_width(width)
,		m_height(height)
,		m_gridSize(width*height) 
	{
	}

	~Query() {}

public:
	// FIXME: slim this down, fix notation! 
	class ThreadContext
	{
	public:
		CUSTOM_NEW
		CUSTOM_DELETE

		ThreadContext(unsigned iThread, const Query* instance) :
		iThread(iThread)
,		gridSize(instance->m_gridSize)
,		sanitized(instance->m_sanitized)
,		width(instance->m_width)
,		height(instance->m_height)
,		visited(static_cast<bool*>(s_customAlloc.Allocate(gridSize*sizeof(bool), kCacheLine)))
,		score(0)
,		reqStrBufLen(0)
		{
			assert(nullptr != instance);

			// I've done some testing and both allocating and clearing memory in the constructor (so in the main thread)
			// is actually faster than doing it upon entering the thread.

			wordsFound.reserve(s_threadInfo[iThread].load /* If a thread finds 50% of the load that's pretty good! */); 

			if (gridSize >= 32)
			{
				// This has proven to be a little faster than memset().
				size_t numStreams = gridSize*sizeof(bool) / sizeof(int);
				int* pWrite = reinterpret_cast<int*>(visited);
				while (numStreams--)
					_mm_stream_si32(pWrite++, 0);
			}
			else
				memset(visited, 0, gridSize*sizeof(bool));
		}

		~ThreadContext()
		{
			s_customAlloc.Free(visited);
		}

		// Input
		const unsigned iThread;
		const size_t gridSize;
		const char* sanitized;
		const unsigned width, height;
		
		// Grid to flag visited tiles.
		bool* visited;

		// Output
		std::vector<size_t> wordsFound;
		unsigned score;
		size_t reqStrBufLen;

#if defined(DEBUG_STATS)
		unsigned maxDepth;
#endif
	}; 

public:
	void Execute()
	{
		// Just in case another Execute() call is made on the same context: avoid leaking.
		FreeWords(m_results);

		// Bit of a step back from what it was, but as I'm picking words out of the global list now..
		DictionaryLock dictLock;
		{
			// Kick off threads.

			std::vector<std::thread> threads;
			std::vector<std::unique_ptr<ThreadContext>> contexts;

			debug_print("Kicking off %zu threads.\n", kNumThreads);

			for (unsigned iThread = 0; iThread < kNumThreads; ++iThread)
			{
				contexts.emplace_back(std::unique_ptr<ThreadContext>(new ThreadContext(iThread, this)));
				threads.emplace_back(std::thread(ExecuteThread, contexts[iThread].get()));
			}
			
			size_t busy = kNumThreads;
			while (busy)
			{
				for (auto& thread : threads)
				{
					if (thread.joinable())
					{
						thread.join();
						--busy;
					}

					std::this_thread::yield();
				}
			}

			m_results.Count  = 0;
			m_results.Score  = 0;
			size_t strBufLen = 0;

			for (auto& context : contexts)
			{
				const unsigned numWords = (unsigned) context->wordsFound.size();
				const unsigned score = context->score;
				m_results.Count += numWords;
				m_results.Score += score;
				strBufLen += context->reqStrBufLen + numWords; // Add numWords for 0-string-terminator for each.

				debug_print("Thread %u joined with %u words (scoring %u).\n", context->iThread, numWords, score);
			}

			m_results.Words = new char*[m_results.Count];

#ifdef NED_FLANDERS
			// Copy words to Results structure.
			// I'd rather set pointers into the dictionary, but that would break the results as soon as new dictionary is loaded.

			char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.
			char* resBuf = new char[strBufLen]; // Allocate sequential buffer.

			for (auto& context : contexts)
			{
				for (auto wordIdx : context->wordsFound)
				{
					*words_cstr = resBuf;
					auto& word = s_dictionary[wordIdx];
					strcpy(*words_cstr++, word.c_str());
					const size_t length = word.length();
					resBuf += length+1;
				}
			}
		}
#else
			// Copy words to Results structure.
			// The dirty way: set pointers.

			char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.

			for (auto& context : contexts)
			{
				for (auto wordIdx : context->wordsFound)
				{
					*words_cstr++ = const_cast<char*>(s_dictionary[wordIdx].c_str());
				}
			}
		}
#endif
	}

private:
	static void ExecuteThread(ThreadContext* context);

private:
	__forceinline static unsigned GetWordScore(size_t length) /* const */
	{
		const unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return kLUT[length-3];
	}

#if defined(DEBUG_STATS)
	static void __forceinline TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node, unsigned& depth);
	static void __forceinline TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node, unsigned& depth);
#else
	static void __forceinline TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node);
	static void __forceinline TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node);
#endif

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;
	const size_t m_gridSize;
};

/* static */ void Query::ExecuteThread(ThreadContext* context)
{
	const unsigned iThread = context->iThread;
	auto* sanitized = context->sanitized;

/*
	auto* visited = context->visited;
	unsigned gridSize = context->gridSize;

	if (gridSize >= 128)
	{
		// This has proven to be a little faster than memset().
		size_t numStreams = gridSize*sizeof(bool) / sizeof(__m128i);
		__m128i* pWrite = reinterpret_cast<__m128i*>(visited);
		const __m128i zero = _mm_setzero_si128();
		while (numStreams--)
			_mm_stream_si128(pWrite++, zero);
	}
	else
		memset(visited, 0, gridSize*sizeof(bool));
*/

	std::unique_ptr<DictionaryNode> root(DictionaryNode::DeepCopy(s_dictRoots[iThread]));
	// DictionaryNode* root = s_dictRoots[iThread];
			
	const unsigned width  = context->width;
	const unsigned height = context->height;

#if defined(DEBUG_STATS)
	debug_print("Thread %u has a load of %zu words.\n", iThread, s_threadInfo[iThread].load);

	context->maxDepth = 0;
#endif

	for (unsigned iY = 0; iY < height; ++iY)
	{
		for (unsigned iX = 0; iX < width; ++iX)
		{
			const size_t boardIdx = iY*width + iX;
			const unsigned index = sanitized[boardIdx];

			if (root->HasChild(index))
			{
				DictionaryNode* child = root->GetChild(index);

#if defined(DEBUG_STATS)
				unsigned depth = 0;
				TraverseBoard(*context, iX, iY, child, depth);
#else
				TraverseBoard(*context, iX, iY, child);
#endif
			}
		}
	}

	// Sorting the indices into the full word list improves execution time a little.
	auto& wordsFound = context->wordsFound;
	std::sort(wordsFound.begin(), wordsFound.end());

	// Tally up the score and required buffer length.
	for (auto wordIdx : wordsFound)
	{
		const size_t length = s_dictionary[wordIdx].length();
		context->score += GetWordScore(length);
		context->reqStrBufLen += length;
	}
		
#if defined(DEBUG_STATS)
	const float missesPct = ((float)wordsFound.size()/s_threadInfo[iThread].load)*100.f;
	debug_print("Thread %u has max. traversal depth %u (max. %u), misses: %.2f percent of load.\n", iThread, context->maxDepth, s_longestWord, missesPct);
#endif
}

#if defined(DEBUG_STATS)
/* static */ __forceinline void Query::TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node, unsigned& depth)
#else
/* static */ __forceinline void Query::TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node)
#endif
{
	const auto width = context.width;
	const unsigned nbBoardIdx = iY*width + iX;

	auto* board = context.sanitized;
	const unsigned nbIndex = board[nbBoardIdx];

	if (0 != node->HasChild(nbIndex))
	{
		auto* child = node->GetChild(nbIndex);
#if defined(DEBUG_STATS)
		TraverseBoard(context, iX, iY, child, depth);
#else
		TraverseBoard(context, iX, iY, child);
#endif

		// Child node exhausted?
		if (child->IsVoid())
		{
			node->RemoveChild(nbIndex);
			// FIXME: find a way to bail out of recursion
		}
	}
}

#if defined(DEBUG_STATS)
/* static */ void __forceinline Query::TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node, unsigned& depth)
#else
/* static */ void __forceinline Query::TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node)
#endif
{
	Assert(nullptr != node);

	const auto width = context.width;
	const auto height = context.height;

	const unsigned boardIdx = iY*width + iX;

	auto* visited = context.visited;
	if (true == visited[boardIdx])
		return;

	// Why does this eat such a vile amount of cycles?
//	if (0 == node->HasChildren())
//		return;

#if defined(DEBUG_STATS)
	Assert(depth < s_longestWord);
	context.maxDepth = std::max(context.maxDepth, depth);
	++depth;
#endif

	// Recurse, as we've got a node that might be going somewhewre.
	
	visited[boardIdx] = true;

	const bool xSafe = iX < width-1;

#if defined(DEBUG_STATS)
	if (iY < height-1)
	{
		TraverseCall(context, iX, iY+1, node, depth);
		
		if (xSafe) 
			TraverseCall(context, iX+1, iY+1, node, depth);
		if (iX > 0)
			TraverseCall(context, iX-1, iY+1, node, depth);
	}

	if (iY > 0)
	{
		TraverseCall(context, iX, iY-1, node, depth);
		
		if (xSafe)
			TraverseCall(context, iX+1, iY-1, node, depth);
		if (iX > 0) 
			TraverseCall(context, iX-1, iY-1, node, depth);
	}

	if (iX > 0)
		TraverseCall(context, iX-1, iY, node, depth);

	if (xSafe)
		TraverseCall(context, iX+1, iY, node, depth);

		--depth;
#else
	// This has been ordered specifically to be as cache friendly as possible,
	// plus due to the enormous advantage that the branching goes 1 way (everywhere but on the edges)
	// the predictor does it's job and the branches aren't expensive at all.

	if (iY < height-1)
	{
		TraverseCall(context, iX, iY+1, node);

		if (xSafe) 
			TraverseCall(context, iX+1, iY+1, node);
		if (iX > 0)
			TraverseCall(context, iX-1, iY+1, node);
	}

	if (iY > 0) {
		TraverseCall(context, iX, iY-1, node);

		if (xSafe)
			TraverseCall(context, iX+1, iY-1, node);
		if (iX > 0) 
			TraverseCall(context, iX-1, iY-1, node);
	}

	if (iX > 0)
		TraverseCall(context, iX-1, iY, node);

	if (xSafe)
		TraverseCall(context, iX+1, iY, node);
#endif

	const size_t wordIdx = node->GetWordIndex();
	if (-1 != wordIdx)
		context.wordsFound.emplace_back(wordIdx);
		
	visited[boardIdx] = false;
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

	const size_t nodeSize = sizeof(DictionaryNode);
	debug_print("Node size: %zu\n", nodeSize);
	
	Results results;
	results.Words = nullptr;
	results.Count = 0;
	results.Score = 0;
	results.UserData = nullptr; // Didn't need it in this implementation.

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		const unsigned gridSize = width*height;
		char* sanitized = static_cast<char*>(s_customAlloc.AllocateUnsafe(gridSize*sizeof(char), kCacheLine));

#ifdef NED_FLANDERS
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
		// Sanitize that just reorders and expects uppercase.
		for (unsigned index = 0; index < gridSize; ++index)
		{
			const char letter = *board++;
			const unsigned sanity = LetterToIndex(letter); // LetterToIndex(toupper(letter));
			sanitized[index] = sanity;
		}
#endif

		Query query(results, (nullptr == sanitized) ? board : sanitized, width, height);
		query.Execute();

		s_customAlloc.Free(sanitized);
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
#endif

	delete[] results.Words;
	results.Words = nullptr;

	results.Count = results.Score = 0;
	results.UserData = nullptr;
}
