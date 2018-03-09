/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	
	Updated late.
	** Now in leaky state, but shit fast. **
	
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

	To do:
		- Less use of iThread, something's off anyway.
		- Use smaller (bit) grids to flag traversed tiles.
		- Look at cache coherency a bit more.
		- Fix non-power-of-2 grids.
		- Make detection of dead ends more efficient, even though it's a really low percentage we're dealing with.
		- I'm not using SIMD (by choice, for now).
		- Detect leaks (and fix the ones you obviously know you have in the nodes) using Valgrind.

	To do (low priority):
		- Building (or loading) my dictionary is slow, I'm fine with that as I focus on the solver.
		- Test on Ubuntu & Windows.
		- Fix class members (notation).
		- Use more references where applicable.


	There's this idea floating that if you have a hash and eliminate a part of the dict. tree that way
	by special-casing the first 3 characters you're golden, but you're not if you do it right really.
	Or it at the very least hardly helps.

	Notes:
		- Compile with full optimization (-O3 for ex.) for best performance.
		- I could not assume anything about the test harness, so I did not; if you want debug output check debug_print().
		- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
		- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
		  is guarded by a mutex and no globals are used.
		- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.
		- My class design isn't really tight (functions and public member values galore), but for now that's fine.
	
	I've done leak testing using Valgrind in OSX and I seem to be in the clear; there are some inconclusive and (hopefully) irrelevant
	ones reported in the runtime library, but you shouldn't run into killer pileups.

	Lesson learned, that I knew all along: branching is terrible.
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

#include "api.h"

// 32-bit Morton ordering routines, they're good enough for the 64-bit build too and it saves some stack.
// Long before 32 bits become too little I'll have problems.
#include "MZC2D32.h"
typedef uint32_t morton_t;

// Undef. to skip dead end percentages and all prints and such.
// #define DEBUG_STATS

#if defined(DEBUG_STATS)
	#define debug_print printf
#else
	inline void debug_print(const char* format, ...) {}
#endif

const unsigned kNumThreads = std::thread::hardware_concurrency();
const unsigned kAlphaRange = ('Z'-'A')+1;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
inline unsigned LetterToIndex(char letter)
{
	return letter - 'A';
}

// FIXME
class DictionaryNode;
inline unsigned AllocNode(unsigned iThread);
inline DictionaryNode* GetNode(unsigned index, unsigned iThread);
inline void ClearNodes(unsigned iThread);

// FIXME: less pointers, less iThread!
class DictionaryNode
{
public:
	DictionaryNode() : alphaBits(0), wordIdx(-1) {}

/*
	DictionaryNode(unsigned alphaBits = 0, unsigned wordIdx = -1) :
		alphaBits(alphaBits), wordIdx(wordIdx)
	{
	}
*/

public:
	static DictionaryNode DeepCopy(const DictionaryNode& parent, unsigned iThread)
	{
		DictionaryNode node;
		node.alphaBits = parent.alphaBits;
		node.wordIdx   = parent.wordIdx;

		for (unsigned index = 0; index < kAlphaRange; ++index)
		{
			if (true == parent.HasChild(index))
			{
				auto iNode = AllocNode(iThread);
				*GetNode(iNode, iThread) = DeepCopy(*parent.GetChild(index, -1), iThread);
				node.children[index] = iNode;
			}
		}

		return node;
	}

	~DictionaryNode() {}

	// Only to be called during dictionary load, from the main thread.
	DictionaryNode* AddChild(char letter)
	{
		const unsigned index = LetterToIndex(letter);

		if (false == HasChild(index))
		{
			children[index] = AllocNode(-1);
			const unsigned bit = 1 << index;
			alphaBits |= bit;
		}

		return GetNode(children[index], -1);
	}

	inline bool IsWord() const { return -1 != wordIdx;  }
	inline bool IsVoid() const { return 0 == alphaBits && false == IsWord(); }

	// Return value indicates if node is now a dead end.
	inline void RemoveChild(unsigned index)
	{
		// Clear the according bit; this operation is performed on a copy so there's no deletion necessary.
		const unsigned bit = 1 << index;
		alphaBits &= ~bit;
	}

	// Always use this prior to GetChild(), it's decisively faster.
	inline bool HasChild(unsigned index) const
	{
		assert(index < kAlphaRange);
		const unsigned bit = 1 << index;
		return 0 != (alphaBits & bit);
	}

	inline DictionaryNode* GetChild(unsigned index, unsigned iThread) const
	{
		assert(true == HasChild(index));
		return GetNode(children[index], iThread);
	}

	// FIXME: better func. name
	inline void ClearWord()
	{
		assert(true == IsWord());
		wordIdx = -1;
	}

	// Dirty as it is, I'm touching these here and there.
	// FIXME: wrap in operation methods on object.
	unsigned wordIdx;
	unsigned alphaBits;

private:
	std::array<unsigned, kAlphaRange> children;
};

// We keep one dictionary at a time, but it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;

// Nodes are allocated sequentially.
static std::vector<std::vector<DictionaryNode>> s_dictNodes;
inline unsigned AllocNode(unsigned iThread) {
//	assert(iThread+1 < s_dictNodes.size());
	auto& vector = s_dictNodes[iThread+1];
	vector.emplace_back(DictionaryNode());
	return vector.size()-1;
}

inline DictionaryNode* GetNode(unsigned index, unsigned iThread) {
//	assert(iThread+1 < s_dictNodes.size());
	auto& vector = s_dictNodes[iThread+1];
	return &vector[index];
}

inline void PrepareNodes(unsigned iThread) {
//	assert(iThread > 0 && iThread <= kNumThreads);
	auto& vector = s_dictNodes[iThread+1];
//	vector.resize(s_dictNodes[0].size());
//	vector.clear();
}

// A root per thread (balancing it by word load doesn't necessarily work out due to mem. coherency).
static std::vector<DictionaryNode> s_dictTrees;

// Sequential dictionary of all full words (FIXME: yet unsorted).
static std::vector<std::string> s_dictionary;

// Counters, the latter being useful to reserve space.
static size_t s_longestWord;
static size_t s_wordCount;

class ThreadInfo
{
public:
	ThreadInfo() : 
		load(0) {}

	~ThreadInfo() {}

	size_t load;
};

static std::vector<ThreadInfo> s_threadInfo;

// Scoped lock for all dictionary globals.
class DictionaryLock
{
public:
	DictionaryLock() :
		m_lock(s_dictMutex) {}

private:
	std::lock_guard<std::mutex> m_lock;
};

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
	if (std::string::npos != iQ)
	{
		auto next = iQ+1;
		if (next == length || word[next] != 'U')
		{
			debug_print("Invalid word due to 'Qu' rule: %s\n", word.c_str());
			return false;
		}

		iQ = word.substr(iQ).find_first_of('Q');
	}

	return true;
}

// Input word must be uppercase!
static void AddWordToDictionary(const std::string& word)
{
	// Word of any use given the Boggle rules?
	if (false == IsWordValid(word))
		return;

	const size_t length = word.length();
	if (length > s_longestWord)
	{
		s_longestWord = length;
	}

	// As a first strategy we'll split at the root.
	const char firstLetter = word[0];

	unsigned iDestThread = LetterToIndex(firstLetter)%kNumThreads;

/*
	// Crappy load balancing.
	unsigned iDestThread = 0;
	size_t maxLoad = 0;
	for (unsigned iThread = 0; iThread < kNumThreads; ++iThread)
	{
		const size_t load = s_threadInfo[iThread].load;
		if (load <= maxLoad)
		{
			iDestThread = iThread;
		}
		else
		{
			maxLoad = load;
		}
	}
*/

	DictionaryNode* current = &s_dictTrees[iDestThread];

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;

		// Get or create child node.
		current = current->AddChild(letter);

		// Handle 'Qu' rule.
		if ('Q' == letter)
		{
			// Verified to be 'Qu' by IsWordValid().
			// Skip over 'U'.
			++iLetter;
		}
	}

	s_dictionary.emplace_back(word);

	current->wordIdx = s_wordCount;

	++s_threadInfo[iDestThread].load;
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

		// Sort the original word list, as we'll be attempting access it in a sorted manner as well.
		std::sort(s_dictionary.begin(), s_dictionary.end());

		// Check thread load total.
		size_t count = 0;
		for (auto& info : s_threadInfo)
		{
			count += info.load;
		}
		
		if (count != s_wordCount)
			debug_print("Thread word count %zu != total word count %zu!", count, s_wordCount);
	}

	debug_print("Dictionary loaded. %zu words, longest being %zu characters\n", s_wordCount, s_longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;
	{
		// FIXME
		s_dictNodes.resize(kNumThreads+1);

		s_dictTrees.resize(kNumThreads, DictionaryNode());
		s_dictionary.clear();

		s_longestWord = 0;
		s_wordCount = 0;
		s_threadInfo.resize(kNumThreads, ThreadInfo());
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
		iThread(iThread)
,		instance(instance)
,		visited(nullptr)
,		sanitized(instance->m_sanitized)
,		score(0)
,		reqStrBufLen(0)
		{
			// Minimal initialization, handle rest in OnThreadStart().
			assert(nullptr != instance);
		}

		// To be called when entering thread.
		void OnThreadStart()
		{
			visited = std::unique_ptr<char[]>(new char[instance->m_gridSize]);
			memset(visited.get(), 0, instance->m_gridSize*sizeof(char));

			// No intermediate allocations please.
			wordsFound.reserve(s_threadInfo[iThread].load); 
		}

		~ThreadContext() {}

		// In-put
		const unsigned iThread;
		const Query* instance;
		std::unique_ptr<char[]> visited; // FIXME: optimize, stuff bits.
		const char* sanitized;

		// Out-put
		std::vector<unsigned> wordsFound;
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
			const unsigned numThreads = kNumThreads;

			std::vector<std::thread> threads;
			std::vector<std::unique_ptr<ThreadContext>> contexts; // FIXME: can go into TLS?

			debug_print("Kicking off %u threads.\n", numThreads);

			for (unsigned iThread = 0; iThread < numThreads; ++iThread)
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
				strBufLen += context->reqStrBufLen + numWords; // Add numWords for terminators.

				debug_print("Thread %u joined with %u words (scoring %u).\n", context->iThread, numWords, score);
			}

			// Copy words to Results structure.
			// I'd rather set pointers into the dictionary, but that would break the results as soon as a new dictionary is loaded.

			m_results.Words = new char*[m_results.Count];
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
	}

private:
	static void ExecuteThread(ThreadContext* context)
	{
		context->OnThreadStart();

		auto& query = *context->instance;
		const unsigned iThread = context->iThread;

		// Pull a deep copy.
		PrepareNodes(iThread);
		DictionaryNode subDict;
		subDict = DictionaryNode::DeepCopy(s_dictTrees[iThread], iThread);

		const unsigned width = query.m_width;
		const unsigned height = query.m_height;

#if defined(DEBUG_STATS)
		debug_print("Thread %u has a load of %zu words.\n", iThread, s_threadInfo[iThread].load);

		context->maxDepth = 0;
		unsigned deadEnds = 0;
#endif

		if (false == subDict.IsVoid())
		{
			morton_t mortonX = ulMC2Dencode(0, 0);
			for (unsigned iX = 0; iX < width; ++iX)
			{
				morton_t morton2D = mortonX;
				for (unsigned iY = 0; iY < height; ++iY)
				{
#if defined(DEBUG_STATS)
					context->isDeadEnd = 1;
#endif

					const unsigned index = query.m_sanitized[morton2D];
					const bool hasChild = subDict.HasChild(index);
					if (true == hasChild)
					{
						unsigned depth = 0;
						TraverseBoard(*context, morton2D, /* child */ subDict.GetChild(index, iThread), depth);

/* 
						This doesn't help as it pollutes the loop with a costly branch.
						if (true == child->IsVoid())
						{
							subDict.RemoveChild(index);
							if (true == subDict.IsVoid())
								break;
						}
*/
					}

#if defined(DEBUG_STATS)
					deadEnds += !context->isDeadEnd;
#endif

					morton2D = ulMC2Dyplusv(morton2D, 1);
				}

				mortonX = ulMC2Dxplusv(mortonX, 1);
			}
		}

		// Sorting the indices into the full word list improves execution time a little.
		auto& wordsFound = context->wordsFound;
		std::sort(wordsFound.begin(), wordsFound.end());
		
#if defined(DEBUG_STATS)
		const float deadPct = ((float)deadEnds/query.m_gridSize)*100.f;
		debug_print("Thread %u has max. traversal depth %u (longest %zu), %u dead ends (%.2f percent).\n", iThread, context->maxDepth, s_longestWord, deadEnds, deadPct);
#endif
	}

private:
	inline static unsigned GetWordScore(size_t length) /* const */
	{
		const unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return kLUT[length-3];
	}

	inline static void TraverseBoard(ThreadContext& context, morton_t mortonCode, DictionaryNode* child, unsigned& depth)
	{
		assert(nullptr != child);
		assert(depth < s_longestWord);

#if defined(DEBUG_STATS)
		context.maxDepth = std::max(context.maxDepth, depth);
#endif

		// Note: checking if the depth is sufficient beforehand is costlier than just checking if the index is valid.
		unsigned wordIdx = child->wordIdx;
		if (-1 != wordIdx)
		{
			// Found a word.
			const unsigned wordIdx = child->wordIdx;
			context.wordsFound.emplace_back(wordIdx);
			const size_t length = s_dictionary[wordIdx].length();
			context.score += GetWordScore(length);
			context.reqStrBufLen += length;

			// FIXME: name et cetera
			child->ClearWord(); 

#if defined(DEBUG_STATS)
			context.isDeadEnd = 0;
#endif

			if (child->IsVoid()) 
			{
				return;
			}
		}

		++depth;

		const size_t gridSize = context.instance->m_gridSize;

		// Recurse, as we've got a node that might be going somewhewre.
		// Before recursion, mark this board position as evaluated.
		auto& visited = context.visited;
		visited[mortonCode] = 1; // FIXME: stuff it!

		// FIXME: a lot of these calculations are needless as we're movingn a window, but it seems fine and we're not
		// pushing the ALU so much that it'd warrant any branching or memory hits.
		morton_t mortonCodes[8];
		mortonCodes[0] = ulMC2Dxminusv(mortonCode, 1);
		mortonCodes[1] = ulMC2Dxplusv(mortonCode, 1);
		mortonCodes[2] = ulMC2Dyminusv(mortonCodes[0], 1);
		mortonCodes[3] = ulMC2Dyminusv(mortonCode, 1);
		mortonCodes[4] = ulMC2Dyminusv(mortonCodes[1], 1);
		mortonCodes[5] = ulMC2Dyplusv(mortonCodes[0], 1);
		mortonCodes[6] = ulMC2Dyplusv(mortonCode, 1);
		mortonCodes[7] = ulMC2Dyplusv(mortonCodes[1], 1);

		for (unsigned iN = 0; iN < 8; ++iN)
		{
			const morton_t newMorton = mortonCodes[iN];
			if (newMorton < gridSize)
			{
				const bool skipTile = visited[newMorton];

				if (false == skipTile)
				{
					auto* board = context.sanitized;
					const unsigned nbIndex = board[newMorton];
					if (true == child->HasChild(nbIndex))
					{
						// Traverse, and if we hit the wall go see if what we're left with his void.
						auto* nbChild = child->GetChild(nbIndex, context.iThread);
						TraverseBoard(context, newMorton, nbChild, depth);
						if (true == nbChild->IsVoid())
						{
							child->RemoveChild(nbIndex);

							if (true == child->IsVoid())
								// Stop recursing.
								break;
						}
					}
				}
			}
		}

		--depth;

		// Open up this position on the board again.
		visited[mortonCode] = 0;
	}

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;
	const size_t m_gridSize;
};

Results FindWords(const char* board, unsigned width, unsigned height)
{
	debug_print("Using debug prints, takes a little off the performance.\n");
	
	Results results;
	results.Words = nullptr;
	results.Count = 0;
	results.Score = 0;
	results.UserData = nullptr; // Didn't need it in this implementation.

	// FIXME: check if board fits in 32 bits.

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		// Yes: sanitize it (check for illegal input and force all to uppercase).
		const unsigned gridSize = width*height;
		std::unique_ptr<char[]> sanitized(new char[gridSize]);

		morton_t mortonX = ulMC2Dencode(0, 0);
		for (unsigned iX = 0; iX < width; ++iX)
		{
			morton_t morton2D = mortonX;
			for (unsigned iY = 0; iY < height; ++iY)
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

				morton2D = ulMC2Dyplusv(morton2D, 1);
			}

			mortonX = ulMC2Dxplusv(mortonX, 1);
		}

		Query query(results, sanitized.get(), width, height);
		query.Execute();
	}

	return results;
}

void FreeWords(Results results)
{
	if (0 != results.Count && nullptr != results.Words)
	{
		// Allocated a single buffer.
		delete[] results.Words[0];
	}

	delete[] results.Words;
	results.Words = nullptr;

	results.Count = results.Score = 0;
	results.UserData = nullptr;
}
