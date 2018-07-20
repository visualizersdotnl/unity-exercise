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
		- Maybe it'd be much faster if you allocate nodes sequentially that are on the same depth level?
		- Optimize 'visited' array.
		- Integrate TLSF allocator.
		- Smaller nodes.
		- FIXMEs.
		- Profile some more!

	To do (low priority):
		- Check compile & run status on Linux.
		- Building (or loading) my dictionary is slow(ish), I'm fine with that as I focus on the solver.
		- Fix class members (notation).
		- Use more references where applicable.


	There's this idea floating that if you have a hash and eliminate a part of the dict. tree that way
	by special-casing the first 3 characters you're golden, but you're not if you do it right really.
	Or it at the very least hardly helps.

	Notes:
		- ** Currently only tested on Windows 10, VS2017 **
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
#include <vector>
#include <string>
#include <iostream>
// #include <map>
// #include <unordered_map>
// #include <set>
// #include <list>
#include <mutex>
#include <thread>
// #include <atomic>
#include <array>
// #include <deque>
#include <cassert>
#include <algorithm>
// #include <bitset>
#include <chrono>

#include "api.h"
#include "random.h"

// 32-bit Morton ordering routines, they're good enough for the 64-bit build too and it saves some stack.
// Long before 32 bits become too little I'll have problems of another nature.
#include "MZC2D32.h"
typedef uint32_t morton_t;

// Stupid simple sequential block allocator.
#include "sequential.h"

// Undef. to skip dead end percentages and all prints and such.
// #define DEBUG_STATS

// Undef. to enable all the work I put in to please a, as it turns out, very forgiving test harness.
// #define NED_FLANDERS

// Undef. to use only 1 thread.
// #define SINGLE_THREAD

// Undef. to kill assertions.
// #define ASSERTIONS

#if defined(_DEBUG) || defined(ASSERTIONS)
	inline void Assert(bool condition) { assert(condition); }
#else
	inline void Assert(bool condition) {}
#endif

#if defined(DEBUG_STATS)
	#define debug_print printf
#else
	inline void debug_print(const char* format, ...) {}
#endif

inline unsigned RoundPow2_32(unsigned value)
{
	--value;
	value |= value >> 1;
	value |= value >> 2;
	value |= value >> 4;
	value |= value >> 8;
	value |= value >> 16;
	return value+1;
}

#if defined(SINGLE_THREAD)
	constexpr size_t kNumThreads = 1;
#else
	const size_t kNumCores = std::thread::hardware_concurrency();
	const size_t kNumThreads = kNumCores*2; // FIXE: this indicates that I'm not using the cores efficiently, and that might be that damned 'visited' array.
#endif

const unsigned kAlphaRange = ('Z'-'A')+1;
const unsigned kPaddingTile = 0xff;

// FIXME: describe
class ThreadInfo
{
public:
	ThreadInfo() : 
		load(0)
,		nodeCount(1) // Each thread has at least 1 root node.
	{}

	size_t load;
	size_t nodeCount;
};

static std::vector<ThreadInfo> s_threadInfo;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
inline unsigned LetterToIndex(char letter)
{
	return letter - 'A';
}

class DictionaryNode
{
	friend void AddWordToDictionary(const std::string& word);
	friend void FreeDictionary();

public:

	static inline DictionaryNode* ThreadCopy(const DictionaryNode* parent, SeqAlloc<DictionaryNode>& allocator)
	{
		Assert(nullptr != parent);

		DictionaryNode* node = allocator.New();
		unsigned alphaBits = node->alphaBits = parent->alphaBits;
		node->wordIdx = parent->wordIdx;

		for (unsigned index = 0; index < kAlphaRange; ++index)
		{
			const unsigned bit = 1 << index;
			if (alphaBits & bit)
			{
				node->children[index] = ThreadCopy(parent->GetChild(index), allocator);
			}
		}

		return node;

	}

	DictionaryNode() : alphaBits(0), wordIdx(-1) {}

private:
	// Must only be called from FreeDictionary();
	~DictionaryNode() 
	{
		for (size_t index = 0; index < kAlphaRange; ++index)
		{
			if (0 != HasChild(index))
			{
				delete children[index];
			}
		}
	}

	// Must only be called from LoadDictionary().	
	DictionaryNode* AddChild(char letter, unsigned iThread)
	{
		const unsigned index = LetterToIndex(letter);

		if (0 == HasChild(index))
		{
			children[index] = new DictionaryNode();
			++s_threadInfo[iThread].nodeCount;

			const unsigned bit = 1 << index;
			alphaBits |= bit;
		}

		return children[index];
	}

public:
	inline unsigned HasChildren() const { return alphaBits; } // Non-zero: has children.
	inline bool IsWord() const { return -1 != wordIdx;  }
	inline bool IsVoid() const { return 0 == alphaBits && -1 == wordIdx; }

	// Zeo return value indicates if node is now a dead end.
	inline unsigned RemoveChild(size_t index)
	{
		Assert(0 != HasChild(index));

		const unsigned bit = 1 << index;
		alphaBits ^= bit;

		// No need to delete since RemoveChild() will only be called from a thread copy, and those
		// use a custom block allocator.

		return HasChildren();
	}

	// Returns non-zero if true.
	inline unsigned HasChild(size_t index) const
	{
		Assert(index < kAlphaRange);
		const unsigned bit = 1 << index;
		return alphaBits & bit;
	}

	inline DictionaryNode* GetChild(size_t index) const
	{
		Assert(0 != HasChild(index));
		return children[index];
	}

	inline void OnWordFound()
	{
		Assert(true == IsWord());
		wordIdx = -1;
	}

	// Dirty as it is, I'm touching these here and there.
	// FIXME: wrap in operation methods on object.
	// FIXME: shove alphaBits in wordIdx?
	size_t wordIdx;
	unsigned alphaBits;

private:
	std::array<DictionaryNode*, kAlphaRange> children;
};

// We keep one dictionary at a time, but it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;

// A root per thread (balancing it by word load doesn't necessarily work out due to mem. coherency).
static std::vector<DictionaryNode*> s_threadDicts;

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
inline bool IsWordValid(const std::string& word)
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
/* static */ void AddWordToDictionary(const std::string& word)
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
	const unsigned iThread = LetterToIndex(firstLetter)%kNumThreads;

	// Gives perfect distribution, but that's apparently *not* what we're looking for.
	// const unsigned iThread = mt_randu32()%kNumThreads;

	DictionaryNode* parent = s_threadDicts[iThread];
	Assert(nullptr != parent);

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;

		// Get or create child node.
		parent = parent->AddChild(letter, iThread);

		// Handle 'Qu' rule.
		if ('Q' == letter)
		{
			// Verified to be 'Qu' by IsWordValid().
			// Skip over 'U'.
			++iLetter;
		}
	}

	s_dictionary.emplace_back(word);

	parent->wordIdx = s_wordCount;

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
		// Create root node per thread.
		for (auto& threadDict : s_threadDicts)
		{
			Assert(nullptr == threadDict);
			threadDict = new DictionaryNode();
		}

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
		// Delete thread roots.
		for (auto* threadDict : s_threadDicts)
		{
			delete threadDict;
		}

		// Clean up arrays.
		s_threadDicts.resize(kNumThreads, nullptr);
		s_threadInfo.resize(kNumThreads, ThreadInfo());

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
,		m_height(height)
,		m_gridSize(width*height) 
	{
	}

	~Query() {}

private:
	// FIXME: slim this down.
	class ThreadContext
	{
	public:
		ThreadContext(unsigned iThread, const Query* instance) :
		instance(instance)
,		iThread(iThread)
,		gridSize(instance->m_gridSize)
,		visited(new bool[gridSize])
,		sanitized(instance->m_sanitized)
,		score(0)
,		reqStrBufLen(0)
		{
			// Minimal initialization, handle rest in OnThreadStart().
			assert(nullptr != instance);
			memset(visited.get(), 0, gridSize*sizeof(bool));

			// FIXME: this is obviously too much for big dictionary VS. small grid or vice versa.
			wordsFound.reserve(s_threadInfo[iThread].load); 
		}

		// In-put
		const Query* instance;
		const unsigned iThread;
		const size_t gridSize;
		std::unique_ptr<bool[]> visited; // FIXME: optimize.
		const char* sanitized;

		// Out-put
		std::vector<size_t> wordsFound;
		unsigned score;
		size_t reqStrBufLen;

#if defined(DEBUG_STATS)
		unsigned maxDepth;
		unsigned isDeadEnd;
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

			for (auto& thread : threads)
			{
				thread.join();
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

		// Pull a deep copy; since everything is allocated with the sequential allocator we don't need to delete the root.
		SeqAlloc<DictionaryNode> nodeAlloc(s_threadInfo[iThread].nodeCount);
		DictionaryNode* subDict = DictionaryNode::ThreadCopy(s_threadDicts[iThread], nodeAlloc);
			
		const unsigned width = query.m_width;
		const unsigned height = query.m_height;

#if defined(DEBUG_STATS)
		debug_print("Thread %u has a load of %zu words.\n", iThread, s_threadInfo[iThread].load);

		context->maxDepth = 0;
		unsigned deadEnds = 0;
#endif

		if (false == subDict->IsVoid())
		{
			morton_t mortonY = ulMC2Dencode(0, 0);
			for (unsigned iY = 0; iY < height; ++iY)
			{
				morton_t morton2D = mortonY;
				for (unsigned iX = 0; iX < width; ++iX)
				{
#if defined(DEBUG_STATS)
					context->isDeadEnd = 1;
#endif

					const unsigned index = query.m_sanitized[morton2D];
					if (0 != subDict->HasChild(index))
					{
						DictionaryNode* child = subDict->GetChild(index);

#if defined(DEBUG_STATS)
						unsigned depth = 0;
						TraverseBoard(*context, morton2D, child, depth);
#else
						TraverseBoard(*context, morton2D, child);
#endif

						// ->> Testing if subDict is empty here doesn't give any gain. <<-

#if defined(DEBUG_STATS)
						deadEnds += !context->isDeadEnd;
#endif
					}

					morton2D = ulMC2Dxplusv(morton2D, 1);
				}

				mortonY = ulMC2Dyplusv(mortonY, 1);
			}
		}

	done:
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
		const float deadPct = ((float)deadEnds/query.m_gridSize)*100.f;
		debug_print("Thread %u has max. traversal depth %u (longest %u), %u dead ends (%.2f percent).\n", iThread, context->maxDepth, s_longestWord, deadEnds, deadPct);
#endif
	}

private:
	inline static unsigned GetWordScore(size_t length) /* const */
	{
		const unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return kLUT[length-3];
	}

	// #pragma inline_recursion(on)

#if defined(DEBUG_STATS)
	static void TraverseBoard(ThreadContext& context, morton_t mortonCode, DictionaryNode* child, unsigned& depth)
#else
	static void TraverseBoard(ThreadContext& context, morton_t mortonCode, DictionaryNode* child)
#endif
	{
		Assert(nullptr != child);

		// Profiling indicates that moving this less likely case to the bottom reduces execution time.
		const size_t wordIdx = child->wordIdx;
		if (-1 != wordIdx)
		{
			// Found a word.
			context.wordsFound.emplace_back(wordIdx);
			child->OnWordFound(); 

#if defined(DEBUG_STATS)
			context.isDeadEnd = 0;
#endif
		}

		// Early out?
		if (!child->HasChildren())
			return;

#if defined(DEBUG_STATS)
		Assert(depth < s_longestWord);
		context.maxDepth = std::max(context.maxDepth, depth);
		++depth;
#endif

		// Before recursion, mark this board position as evaluated.
		auto& visited = context.visited;
		visited[mortonCode] = true;

		const size_t gridSize = context.gridSize;
		auto* board = context.sanitized;

		// FIXME: this can be done much smarter using a sliding window, but in the full picture it doesn't look to be worth it.
		morton_t mortonCodes[8];

		// Left, Right
		mortonCodes[0] = ulMC2Dxminusv(mortonCode, 1);
		mortonCodes[1] = ulMC2Dxplusv(mortonCode, 1);
	
		// Lower left, Upper right
		mortonCodes[2] = ulMC2Dyminusv(mortonCodes[0], 1);
		mortonCodes[3] = ulMC2Dyplusv(mortonCodes[1], 1);

		// Lower right, Upper left		
		mortonCodes[4] = ulMC2Dyminusv(mortonCodes[1], 1);
		mortonCodes[5] = ulMC2Dyplusv(mortonCodes[0], 1);

		// Up, Down		
		mortonCodes[6] = ulMC2Dyplusv(mortonCode, 1);
		mortonCodes[7] = ulMC2Dyminusv(mortonCode, 1);

		// Recurse, as we've got a node that might be going somewhewre.
		for (int iDir = 0; iDir < 8; ++iDir)
		{
			const morton_t newMorton = mortonCodes[iDir];
			if (newMorton >= gridSize)
				continue;

			const unsigned nbIndex = board[newMorton];
			if (kPaddingTile != nbIndex && child->HasChild(nbIndex))
			{
				if (false == visited[newMorton])
				{
					// Traverse, and if we hit the wall go see if what we're left with his void.
					auto* nbChild = child->GetChild(nbIndex);

#if defined(DEBUG_STATS)
					TraverseBoard(context, newMorton, nbChild, depth);
#else
					TraverseBoard(context, newMorton, nbChild);
#endif

					if (true == nbChild->IsVoid())
					{
						if (0 == child->RemoveChild(nbIndex))
						{
							// Stop recursing, but still check for word below.
							break;
						}
					}
				}
			}
		}

#if defined(DEBUG_STATS)
		--depth;
#endif

		// Open up this position on the board again.
		visited[mortonCode] = false;
	}

	// #pragma inline_recursion(off)

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
		printf("Built without the NED_FLANDERS define, so the safety measures are largely off.\n");
		warned = true;
	}
#endif
	
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

//	const unsigned xPadding = pow2Width-width;
//	const unsigned yPadding = pow2Height-height;

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		const unsigned gridSize = pow2Width*pow2Height;
		std::unique_ptr<char[]> sanitized(new char[gridSize]);

		// FIXME: easy way to set all padding tiles, won't notice it with boards that are large, but it'd be at least
		//        better to do this with a write-combined memset().
		memset(sanitized.get(), kPaddingTile, gridSize*sizeof(char));

#ifdef NED_FLANDERS
		// Sanitize that checks for illegal input and uppercases.
		morton_t mortonY = ulMC2Dencode(0, 0);
		for (unsigned iY = 0; iY < height; ++iY)
		{
			morton_t morton2D = mortonY;
			for (unsigned iX = 0; iX < width; ++iX)
			{
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

			mortonY = ulMC2Dyplusv(mortonY, 1);
		}
#else
		// Sanitize that just uppercases.
		morton_t mortonY = ulMC2Dencode(0, 0);
		for (unsigned iY = 0; iY < height; ++iY)
		{
			morton_t morton2D = mortonY;
			for (unsigned iX = 0; iX < width; ++iX)
			{
				const char letter = *board++;
				const unsigned sanity = LetterToIndex(toupper(letter));
				sanitized[morton2D] = sanity;

				morton2D = ulMC2Dxplusv(morton2D, 1);
			}

			mortonY = ulMC2Dyplusv(mortonY, 1);
		}
#endif

		Query query(results, sanitized.get(), pow2Width, pow2Height);
		query.Execute();
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
