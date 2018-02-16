/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	
	Updated thereafter :-)
	Now in broken state, but shit fast.
	
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
		- Fix tree (leaks, reuse).
		- Kill recursion of dead ends.
		- Fix everything non-power-of-2 grids: Morton shit really worth it?
		- Fix 32-bit.
		- Is it still C++11?
		- Fix class member (notation).
		- Use more references where applicable.
		- Assertions!

	Notes:
		- Compile with full optimization (-O3 for ex.) for best performance.
		- I could not assume anything about the test harness, so I did not; if you want debug output check debug_print().
		- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
		- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
		  is guarded by a mutex and no globals are used.
		- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.
		- Assertions aren't all over the place (yet).
	
	I've done leak testing using Valgrind in OSX and I seem to be in the clear; there are some inconclusive and (hopefully) irrelevant
	ones reported in the runtime library, but you shouldn't run into killer pileups.
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
#include <atomic>
#include <array>
// #include <deque>
#include <cassert>

#include "api.h"

// FIXME: 32-bit support, clean up the Morton mess, use 32-bit header!
#include "MZC2D64.h"

// #define debug_print printf
inline void debug_print(const char* format, ...) {}

const unsigned kNumThreads = std::thread::hardware_concurrency();
const unsigned kAlphaRange = ('Z'-'A')+1;
const unsigned kVisitedFlag = 1<<7;

inline unsigned LetterToIndex(char letter)
{
	return letter - 'A';
}

inline unsigned LetterToThreadIndex(char letter)
{
	return LetterToIndex(letter)%kNumThreads;
}

// We'll be using a word tree built out of these simple nodes.
// FIXME: clarify the difference between a copy and an original
class DictionaryNode
{
public:
	DictionaryNode() :
		alphaBits(0), wordIdx(-1)
	{
	}

	~DictionaryNode() 
	{
		// FIXME
	}

	inline bool IsWord() const
	{
		return -1 != wordIdx;
	}

	inline bool IsVoid() const
	{
		return 0 == alphaBits;
	}

	inline DictionaryNode* AddChild(char letter)
	{
		const unsigned index = LetterToIndex(letter);
		const unsigned bit = 1 << index;

		if (0 == (alphaBits & bit))
		{
			// FIXME: seq. pool?
			children[index] = new DictionaryNode();	
		}

		alphaBits |= bit;
		return children[index];
	}

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

	inline DictionaryNode* GetChild(unsigned index) const
	{
		const unsigned mask = HasChild(index);
		return (DictionaryNode*) (reinterpret_cast<uintptr_t>(children[index]) * mask);
	}

	// FIXME: better func. name
	inline void ClearWord()
	{
		assert(true == IsWord());
		wordIdx = -1;
	}

	unsigned wordIdx;
	unsigned alphaBits;

	// FIXME: this is a problem, you change values pointed to, don't matter if you're not deleting them
	DictionaryNode* children[kAlphaRange];
};

class DictionaryPrefixNode
{
public:

};


// We keep one dictionary at a time, but it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;
static std::vector<DictionaryNode> s_dictTrees;
static std::vector<std::string> s_dictionary;
static unsigned* s_scoreLUT = nullptr;
static size_t s_longestWord;
static size_t s_wordCount;

// Scoped lock for all dictionary globals.
// FIXME: locks for every thread so they won't fight on initial copy?
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
	const unsigned iThread = LetterToThreadIndex(firstLetter);
	DictionaryNode* current = &s_dictTrees[iThread];

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

	s_dictionary.push_back(word);
	current->wordIdx = s_wordCount;
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

	debug_print("Dictionary loaded. %zu words, longest being %zu characters\n", s_wordCount, s_longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;

	s_dictTrees.resize(kNumThreads, DictionaryNode());
	s_dictionary.clear();

	delete[] s_scoreLUT;

	s_wordCount = 0;
	s_longestWord = 0;
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
,		board(new char[instance->m_gridSize])
,		score(0)
,		reqStrBufLen(0)
		{
			assert(nullptr != instance);
			memcpy(board.get(), instance->m_sanitized, instance->m_gridSize);

			// FIXME: I could calculate this correctly
			wordsFound.reserve(s_wordCount/kNumThreads); 
		}

		~ThreadContext() {}

		const unsigned iThread;
		const Query* instance; // FIXME: what do I really want to know?

		std::unique_ptr<char[]> board;
		std::vector<unsigned> wordsFound;
		unsigned score;
		size_t reqStrBufLen;

		// DEBUG
		unsigned maxDepth;
		unsigned isDeadEnd;
	}; 

public:
	void Execute()
	{
		// Just in case another Execute() call is made on the same context: avoid leaking.
		if (nullptr != m_results.Words)
		{
			FreeWords(m_results);
		}

		// Lock dictionary for the entire run. 
		// This is a step back from my previous solution, but it gives me a little leeway to do less copying.
		DictionaryLock lock;

		// Kick off threads.
		const unsigned numThreads = kNumThreads;

		// FIXME: another container?
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
		// FIXME: threads can handle all below, give or take a few tweaks.

		m_results.Words = new char*[m_results.Count];
		char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.
		char* resBuf = new char[strBufLen];

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

private:
	static void ExecuteThread(ThreadContext* context)
	{
		auto& query = *context->instance;
		const unsigned iThread = context->iThread;

		// FIXME
		DictionaryNode& subDict = s_dictTrees[iThread];
//		subDict = s_dictTrees[iThread];

		const unsigned width = query.m_width;
		const unsigned height = query.m_height;

		// DEBUG
		context->maxDepth = 0;
		unsigned deadEnds = 0;

		if (false == subDict.IsVoid())
		{
			auto* board = context->board.get();

			uint64_t mortonX = ullMC2Dencode(0, 0);
			for (unsigned iX = 0; iX < width; ++iX)
			{
				uint64_t morton2D = mortonX;
				for (unsigned iY = 0; iY < height; ++iY)
				{
					// DEBUG
					context->isDeadEnd = 1;

					const unsigned index = board[morton2D];
					auto hasChild = subDict.HasChild(index);
					if (true == hasChild)
					{
						unsigned depth = 0;
						TraverseBoard(*context, morton2D, /* child */ subDict.GetChild(index), depth);

//						if (true == child->IsVoid())
//						{
//							subDict.RemoveChild(index);
//							if (true == subDict.IsVoid())
//								break;
//						}

					}

					// DEBUG
					deadEnds += !context->isDeadEnd;

					morton2D = ullMC2Dyplusv(morton2D, 1);
				}

				mortonX = ullMC2Dxplusv(mortonX, 1);
			}
		}
		
		const float deadPct = ((float)deadEnds/query.m_gridSize)*100.f;
		debug_print("Thread %u has max. traversal depth %u, %u dead ends (%.2f percent).\n", iThread, context->maxDepth, deadEnds, deadPct);
	}

private:
	inline static unsigned GetWordScore(size_t length) // const
	{
		const unsigned LUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return LUT[length-3];
	}

	inline static void TraverseBoard(ThreadContext& context, uint64_t mortonCode, DictionaryNode* child, unsigned& depth)
	{
		assert(nullptr != child);
		assert(depth < s_longestWord);

		context.maxDepth = std::max(context.maxDepth, depth);

		if (depth >= 2)
		{
			unsigned wordIdx = child->wordIdx;
			if (-1 != wordIdx)
			{
				// Found a word.
				const unsigned wordIdx = child->wordIdx;
				context.wordsFound.emplace_back(wordIdx);
				auto& word = s_dictionary[wordIdx];
				const size_t length = word.length();
				context.score += GetWordScore(length);
				context.reqStrBufLen += length;

				// FIXME: name et cetera
				child->ClearWord(); 

				// DEBUG
				context.isDeadEnd = 0;

				if (child->IsVoid()) 
				{
					return;
				}
			}
		}

		++depth;
//		if (++depth >= s_longestWord) return;

		auto& board = context.board;
		const size_t gridSize = context.instance->m_gridSize;

		// Recurse, as we've got a node that might be going somewhewre.
		// Before recursion, mark this board position as evaluated.
		board[mortonCode] |= kVisitedFlag;

		uint64_t mortonCodes[8];
		mortonCodes[0] = ullMC2Dxminusv(mortonCode, 1);
		mortonCodes[1] = ullMC2Dxplusv(mortonCode, 1);
		mortonCodes[2] = ullMC2Dyminusv(mortonCodes[0], 1);
		mortonCodes[3] = ullMC2Dyminusv(mortonCode, 1);
		mortonCodes[4] = ullMC2Dyminusv(mortonCodes[1], 1);
		mortonCodes[5] = ullMC2Dyplusv(mortonCodes[0], 1);
		mortonCodes[6] = ullMC2Dyplusv(mortonCode, 1);
		mortonCodes[7] = ullMC2Dyplusv(mortonCodes[1], 1);

		for (unsigned iN = 0; iN < 8; ++iN)
		{
			const uint64_t newMorton = mortonCodes[iN];
			if (newMorton < gridSize)
			{
				const unsigned nbTile = board[newMorton];
				const unsigned visited = nbTile & kVisitedFlag;

				if (0 == visited)
				{
					const unsigned nbIndex = nbTile & ~kVisitedFlag;
					if (true == child->HasChild(nbIndex))
					{
						// Traverse, and if we hit the wall go see if what we're left with his void.
						auto* nbChild = child->GetChild(nbIndex);
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
		board[mortonCode] &= ~kVisitedFlag;
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

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		// Yes: sanitize it (check for illegal input and force all to uppercase).
		const unsigned gridSize = width*height;
		std::unique_ptr<char[]> sanitized(new char[gridSize]);

		uint64_t mortonX = ullMC2Dencode(0, 0);
		for (unsigned iX = 0; iX < width; ++iX)
		{
			uint64_t morton2D = mortonX;
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

				morton2D = ullMC2Dyplusv(morton2D, 1);
			}

			mortonX = ullMC2Dxplusv(mortonX, 1);
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
