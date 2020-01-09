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
		- Profile and optimize:
		  - Different 'visited' strategy.
		  - Bigger free()-granularity (sort of fixed).
		  - Cache issues & dictionary node optimization.
		  - Scoring seems one off with the submitted version; weird.
		- Always check for leaks (Windows debug build does it automatically).
		- FIXMEs.

	To do (low priority):
		- Check compile & run status on Linux.
		- Building (or loading) my dictionary is slow(ish), I'm fine with that as I focus on the solver.
		- Fix class members (notation).

	There's this idea floating that if you have a hash and eliminate a part of the dict. tree that way
	by special-casing the first 3 characters you're golden, but you're not if you do it right really.
	Or it at the very least hardly helps.

	Notes:
		- ** Currently only tested on Windows 10, VS2017 + OSX **
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
*/

// Make VC++ 2015 shut up and walk in line.
#define _CRT_SECURE_NO_WARNINGS 

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
#include <unordered_set>
#include <vector>
#include <array>
#include <algorithm>

#include <cassert>
	
#include "api.h"
#include "random.h"
// #include "alloc-aligned.h"
#include "bit-tricks.h"
#include "simple-tlsf.h"

// 32-bit Morton ordering routines, they're good enough for the 64-bit build too and it saves some stack.
// Long before 32 bits become too little I'll have problems of another nature.
#include "MZC2D32.h"
typedef uint32_t morton_t;

/*
#include "MZC2D64.h"
typedef uint64_t morton_t;
#define ulMC2Dencode ullMC2Dencode
#define ulMC2Dxplusv ullMC2Dxplusv
#define ulMC2Dyplusv ullMC2Dyplusv
#define ulMC2Dxminusv ullMC2Dxminusv
#define ulMC2Dyminusv ullMC2Dyminusv
*/

// Undef. to skip dead end percentages and all prints and such.
// #define DEBUG_STATS

// Undef. to enable all the work I put in to please a, as it turns out, very forgiving test harness.
// But basically the only gaurantee here is that this works with my own test!
// #define NED_FLANDERS

// Undef. to use only 1 thread.
// #define SINGLE_THREAD

// Undef. to kill assertions.
// #define ASSERTIONS

#if defined(ASSERTIONS)
	#define Assert assert
#else
	inline void Assert(bool condition) {}
#endif

#if defined(DEBUG_STATS)
	#define debug_print printf
#else
	inline void debug_print(const char* format, ...) {}
#endif

const size_t kCacheLine = sizeof(size_t)<<3;

const unsigned kAlphaRange  = ('Z'-'A');
const unsigned kPaddingTile = 0xff;

#if defined(SINGLE_THREAD)
	constexpr size_t kNumThreads = 1;
#else
	const size_t kNumCores = std::thread::hardware_concurrency();
	const size_t kNumThreads = kNumCores*2;
#endif

// Number of words per thread
class ThreadInfo
{
public:
	ThreadInfo() : 
		load(0)
	{
	}

	size_t load;
};

static std::vector<ThreadInfo> s_threadInfo;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
__inline unsigned LetterToIndex(char letter)
{
	auto index = letter - 'A';
	return index;
}

// FWD.
static void AddWordToDictionary(const std::string& word);

// FIXME: under construction (hacky) to see if my DeepCopy() idea works out
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

	static __inline DictionaryNode* DeepCopy(DictionaryNode* parent)
	{
		DictionaryNode* node = new DictionaryNode();
		node->m_wordIdx = parent->m_wordIdx;
		unsigned indexBits = node->m_indexBits = parent->m_indexBits;

		unsigned index = 0;
		while (indexBits)
		{
			if (indexBits & 1)
			{
				auto *child = parent->GetChild(index);
				node->m_children[index] = DeepCopy(child);
			}

			indexBits >>= 1;
			++index;
		}

		return node;
	}

	~DictionaryNode()
	{
		// FIXME: C++11-ify
//		for (unsigned iChild = 0; iChild < kAlphaRange; ++iChild)
//		{
//			auto* child = m_children[iChild];
//			delete child;
//		}

//		delete[] m_children;

		// FIXME: since I disabled freeing pointers in the custom allocator, the above is not necessary
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
	__inline bool HasChildren() const { return 0 != m_indexBits; } // Non-zero.
	__inline bool IsWord() const { return -1 != m_wordIdx; }
	__inline int IsVoid() const { return int(m_indexBits+m_wordIdx)>>31; } // Is 1 (sign bit) if zero children (0) and no word (-1).

	// Returns zero if node is now a dead end.
	__inline unsigned RemoveChild(unsigned index)
	{
		Assert(index < kAlphaRange);
		const unsigned bit = 1 << index;
		m_indexBits &= ~bit;
		return 0 != m_indexBits || false == IsWord();
	}

	// Returns non-zero if true.
	__inline unsigned HasChild(unsigned index)
	{
		Assert(index < kAlphaRange || index == kPaddingTile /* Saves checking for it since it'll just result in zilch. */);
		const unsigned bit = 1 << index;
		return m_indexBits & bit;
	}

	__inline DictionaryNode* GetChild(unsigned index)
	{
		Assert(HasChild(index));
		return m_children[index];
	}

	__inline size_t GetWordIndex() // const
	{
		// The idea here is that if a word index is not -1, it is found and thus should be eliminated directly
		auto index = m_wordIdx;
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

	unsigned iThread;
	if (word[0] == 'Q' && word[1] == 'U')
	{
		// A super hacky way to divide the 'Qu' cases between two threads allocated for both letters (FIXME)
		iThread = (word.size() & 1) ? 20 : 16;
	}
	else
	{
		const char letter = word[0];
		iThread = LetterToIndex(letter)%kNumThreads;
	}

	// This performs extremely poorly:
	// const unsigned iThread = mt_randu32()%kNumThreads;
	
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
		for (unsigned iThread = 0; iThread < kAlphaRange; ++iThread)
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

		// Reset thread information
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

private:
	// FIXME: slim this down, fix notation.
	class ThreadContext
	{
	public:
		CUSTOM_NEW
		CUSTOM_DELETE

		ThreadContext(unsigned iThread, const Query* instance) :
		instance(instance)
,		iThread(iThread)
,		gridSize(instance->m_gridSize)
,		sanitized(instance->m_sanitized)
,		visited(static_cast<bool*>(s_customAlloc.Allocate(gridSize*sizeof(bool), kCacheLine)))
//,		visited(static_cast<bool*>(mallocAligned(gridSize*sizeof(bool), kCacheLine)))
,		score(0)
,		reqStrBufLen(0)
		{
			assert(nullptr != instance);

//			memset(visited, 0, gridSize*sizeof(bool));

			size_t numStreams = gridSize*sizeof(bool);
			numStreams >>= 2;
			int* pWrite = reinterpret_cast<int*>(visited);
			while (numStreams--)
				_mm_stream_si32(pWrite++, 0);

			wordsFound.reserve(s_threadInfo[iThread].load); // FIXME: if the grid size is small this is obviously too much, but in the "worst" case we won't re-allocate.
		}

		~ThreadContext()
		{
			s_customAlloc.Free(visited);
//			freeAligned(visited);
		}

		// Input/Temp
		const Query* instance;
		const unsigned iThread;
		const size_t gridSize;
		const char* sanitized;
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
	static void ExecuteThread(ThreadContext* context)
	{
		auto& query = *context->instance;
		const unsigned iThread = context->iThread;
		auto* sanitized = context->sanitized;
		auto* visited = context->visited;

		// A necessary evil to support multiple runs
		std::unique_ptr<DictionaryNode> root(DictionaryNode::DeepCopy(s_dictRoots[iThread]));

		// auto *root = s_dictRoots[iThread];
			
		const unsigned width = query.m_width;
		const unsigned height = query.m_height;

#if defined(DEBUG_STATS)
		debug_print("Thread %u has a load of %zu words.\n", iThread, s_threadInfo[iThread].load);

		context->maxDepth = 0;
#endif
		morton_t mortonY = ulMC2Dencode(0, 0);
		for (unsigned iY = 0; iY < height; ++iY)
		{
			morton_t morton2D = ulMC2Dencode(0, iY); // mortonY;
			for (unsigned iX = 0; iX < width; ++iX)
			{
				const unsigned index = sanitized[morton2D];

				// FIXME: this only works as long as the children array is initialized with null pointers.
				if (root->HasChild(index))
				{
					// Flag tile as visited.
					visited[morton2D] = true;

					DictionaryNode* child = root->GetChild(index);
#if defined(DEBUG_STATS)
					unsigned depth = 0;
					TraverseBoard(*context, morton2D, child, depth);
#else
					TraverseBoard(*context, morton2D, child);
#endif

					// Remove visit flag.
					visited[morton2D] = false;
				}

				morton2D = ulMC2Dxplusv(morton2D, 1);
			}

			// This stabilizes thread load a little.
			std::this_thread::yield();
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
		const float ratio = 100.f*float(wordsFound.size())/s_threadInfo[iThread].load;
		debug_print("Thread %u has max. traversal depth %u (longest %u), success rate %.2f percent.\n", iThread, context->maxDepth, s_longestWord, ratio);
#endif
	}

private:
	__inline static unsigned GetWordScore(size_t length) /* const */
	{
		const unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return kLUT[length-3];
	}

#if defined(DEBUG_STATS)
	static bool TraverseBoard(ThreadContext& context, morton_t mortonCode, DictionaryNode* node, unsigned& depth)
#else
	static bool TraverseBoard(ThreadContext& context, morton_t mortonCode, DictionaryNode* node)
#endif
	{
		Assert(nullptr != node);

		// This branch is expensive
		// if (false == node->HasChildren())
		//	return;

		// So we use this as the recursion loop counter below instead.
		// const unsigned count = IsNotZero(node->HasChildren()) << 3;

		constexpr unsigned count = 8;

#if defined(DEBUG_STATS)
		Assert(depth < s_longestWord);
		context.maxDepth = std::max(context.maxDepth, depth);
		++depth;
#endif

		morton_t mortonCodes[count];

		// Left, Right
		const morton_t left  = ulMC2Dxminusv(mortonCode, 1);
		const morton_t right = ulMC2Dxplusv(mortonCode, 1);

		mortonCodes[0] = left;
		mortonCodes[1] = right;

		mortonCodes[2] = ulMC2Dyminusv(left, 1); // UL
		mortonCodes[3] = ulMC2Dyminusv(mortonCode, 1); // U
		mortonCodes[4] = ulMC2Dyminusv(right, 1); // UR

		mortonCodes[5] = ulMC2Dyplusv(right, 1); // LR
		mortonCodes[6] = ulMC2Dyplusv(mortonCode, 1); // D
		mortonCodes[7] = ulMC2Dyplusv(left, 1); // LL

		// Recurse, as we've got a node that might be going somewhewre.

		auto* board = context.sanitized;
		auto* visited = context.visited;
		const size_t gridSize = context.gridSize;

		for (unsigned iDir = 0; iDir < 8; ++iDir)
		{
			const morton_t nbMorton = mortonCodes[iDir];
			if (nbMorton >= gridSize)
				continue;

			const unsigned nbIndex = board[nbMorton];
			if (!node->HasChild(nbIndex))
				continue;

			if (true == visited[nbMorton])
				continue;

			// Flag new tile as visited.
			visited[nbMorton] = true;

			auto* child = node->GetChild(nbIndex);

#if defined(DEBUG_STATS)
			TraverseBoard(context, nbMorton, child, depth);
#else
			TraverseBoard(context, nbMorton, child);
#endif

			// Remove visit flag;
			visited[nbMorton] = false;

			// Child node exhausted?
			if (child->IsVoid())
			{
				const unsigned isZero = IsZero(node->RemoveChild(nbIndex));
				iDir = isZero<<3; // Break out of loop without extra branch.
			}
		}

#if defined(DEBUG_STATS)
		--depth;
#endif

		const size_t wordIdx = node->GetWordIndex();
		if (-1 == wordIdx)
			return false;

		// Found a word!
		context.wordsFound.emplace_back(wordIdx);
		return true;
	}

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;
	const size_t m_gridSize;
};

Results FindWords(const char* board, unsigned width, unsigned height)
{
	debug_print("Using debug prints, takes a little off the performance.\n");

#if !defined(NED_FLANDERS)
	static bool warned = false;
	if (false == warned)
	{
		debug_print("Built without the NED_FLANDERS define, so the safety measures are largely off.\n");
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

	// FIXME: check if board fits in 32 bits.
	// FIXME: this is a cheap fix, but keeping the Morton-arithmetic light during traversal is worth something.
	const unsigned pow2Width = RoundPow2_32(width);
	const unsigned pow2Height = RoundPow2_32(height);
	
	if (pow2Width != width || pow2Height != height)
		debug_print("Rounding board dimensions to %u*%u.\n", pow2Width, pow2Height);

	const unsigned xPadding = pow2Width-width;
	const unsigned yPadding = pow2Height-height;

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		const unsigned gridSize = pow2Width*pow2Height;
//		char* sanitized = static_cast<char*>(mallocAligned(gridSize*sizeof(char), kCacheLine));
		char* sanitized = static_cast<char*>(s_customAlloc.Allocate(gridSize*sizeof(char), kCacheLine));

		// FIXME: easy way to set all padding tiles, won't notice it with boards that are large, but it'd be at least
		//        better to do this with a write-combined memset().
		if (xPadding+yPadding > 0)
			memset(sanitized, kPaddingTile, gridSize*sizeof(char));

#ifdef NED_FLANDERS
		// Sanitize that checks for illegal input and uppercases.
		for (unsigned iY = 0; iY < height; ++iY)
		{
			morton_t morton2D = ulMC2Dencode(0, iY);
			for (unsigned iX = 0; iX < width; ++iX)
			{
				// FIXME: does not check for 'U'!
				const char letter = *board++;

				if (0 != isalpha((unsigned char) letter))
				{
					const unsigned sanity = LetterToIndex(toupper(letter));
					sanitized[morton2D] = sanity;
				}
				else
				{
					// Invalid character: skip query.
					return results;
				}

				morton2D = ulMC2Dxplusv(morton2D, 1);
			}
		}
#else
		// Sanitize that just reorders and expects uppercase.
		for (unsigned iY = 0; iY < height; ++iY)
		{
			morton_t morton2D = ulMC2Dencode(0, iY);
			for (unsigned iX = 0; iX < width; ++iX)
			{
				const char letter = *board++;
				const unsigned sanity = LetterToIndex(letter); // LetterToIndex(toupper(letter));
				sanitized[morton2D] = sanity;

				morton2D = ulMC2Dxplusv(morton2D, 1);
			}
		}
#endif

		Query query(results, sanitized, pow2Width, pow2Height);
		query.Execute();

//		freeAligned(sanitized);
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
