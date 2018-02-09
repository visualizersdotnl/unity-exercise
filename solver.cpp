/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	Updated thereafter :-)
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
		- Kill dead end recursion (prefix detection), always recursing at least 3 times to find a word is costly.
		- Tighter allocation (sequential, swap pointers for indices), per-thread dictionary lock.
		- Lighter Trie?
		- Write final word list at once (or at least allocate the memory for them at once), or defer that to the thread too.
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

#define debug_print printf
// inline void debug_print(const char* format, ...) {}

const unsigned kAlphaRange = ('Z'-'A')+1;

const unsigned kNumThreads = 1;
// const unsigned kNumThreads = 2; // My Core M has 2 cores, performance generally gets worse using more (like 4, which is the "threads").
// const unsigned kNumThreads = std::thread::hardware_concurrency();

inline unsigned LetterToIndex(char letter)
{
	return letter - 'A';
}

inline unsigned LetterToThreadIndex(char letter)
{
	return LetterToIndex(letter) % kNumThreads;
}

// We'll be using a word tree built out of these simple nodes.
// FIXME: more functions less direct access, const-correctness.
// FIXME: clarify the difference between a copy and an original
class DictionaryNode
{
public:
	DictionaryNode() :
		alphaBits(0)
	{
		children.fill(nullptr);
	}

	~DictionaryNode() {}

	inline bool IsWord() const
	{
		// FIXME: bit check?
		return false == word.empty();
	}

	// Just tell if there's any children left to recurse as per the official definition.
	inline bool IsLeaf() const
	{
	 	return 0 == alphaBits;
	}

	inline DictionaryNode* AddChild(char letter)
	{
		const unsigned index = LetterToIndex(letter);

		if (nullptr == children[index])
		{
			const unsigned bit = 1 << index;
			alphaBits |= bit;

			children[index] = new DictionaryNode();	
		}

		return children[index];
	}

	// Return value indicates if node is now a dead end.
	inline bool RemoveChild(char letter)
	{
		const unsigned index = LetterToIndex(letter);

		// Clear the according bit; this operation is performed on a copy so there's no deletion necessary.
		const unsigned bit = 1 << index;
		alphaBits &= ~bit;

		return 0 == alphaBits && false == IsWord();
	}


	inline DictionaryNode* GetChild(char letter)
	{
		const unsigned index = LetterToIndex(letter);
		const unsigned bit = 1 << index;
		return (alphaBits & bit) ? children[index] : nullptr;	
	}

	// FIXME: get rid of it?
	inline void ClearWord()
	{
		assert(true == IsWord());
		word.clear();
	}

	// Full word without 'Qu'. 
	// FIXME: get rid of it.
	std::string word; 

	// FIXME: this is a problem, you change values pointed to, don't matter if you're not deleting them
	std::array<DictionaryNode*, kAlphaRange> children;

	unsigned alphaBits;
};

// We keep one dictionary (in subsets) at a time, but it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;
static std::vector<DictionaryNode> s_dictTrees;
// static std::array<DictionaryNode, kNumThreads> s_dictTrees;
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
	auto iQ = word.find('Q');
	if (std::string::npos != iQ)
	{
		auto next = word.begin() + iQ+1;
		if (next == word.end() || *next != 'U')
		{
			debug_print("Invalid word due to 'Qu' rule: %s\n", word.c_str());
			return false;
		}
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
		// Longest word thus far (just a print statistic).
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

	current->word = word;
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

	s_dictTrees.clear();
	s_dictTrees.resize(kNumThreads, DictionaryNode());

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
		{
//			assert(nullptr != instance);
			memcpy(board.get(), instance->m_sanitized, instance->m_gridSize);
			wordsFound.reserve(s_wordCount);
		}

		~ThreadContext() {}

		const unsigned iThread;
		const Query* instance; // FIXME: what do I really want to know?

		std::unique_ptr<char[]> board;
		std::vector<std::string> wordsFound;

		// FIXME: debug.
		unsigned deadEnd;

		DictionaryNode* root;
	};

public:
	void Execute()
	{
		// Just in case another Execute() call is made on the same context: avoid leaking.
		if (nullptr != m_results.Words)
		{
			FreeWords(m_results);
		}

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

		// FIXME: threads can handle all below.

		m_results.Count = 0;
		for (auto& context : contexts)
		{
			const unsigned count = (unsigned) context->wordsFound.size();
			m_results.Count += count;
			debug_print("Thread %u joined with %u words.\n", context->iThread, count);
		}

		// Copy words to Results structure and calculate the score.
		m_results.Words = new char*[m_results.Count];
		m_results.Score = 0;
		
		char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.
		for (auto& context : contexts)
		{
			for (auto& word : context->wordsFound)
			{
				const size_t length = word.length();

				// Uses full word to get the correct score.
				m_results.Score += GetWordScore(length);

				// FIXME: this takes a fucking second or more.. At least allocate it all at once.
//				*words_cstr = new char[length+1];
//				strcpy(*words_cstr++, word.c_str());
			}
		}
	}

private:
	static void ExecuteThread(ThreadContext* context)
	{
		auto& query = *context->instance;
		const unsigned iThread = context->iThread;

		// FIXME
		// Grab a copy of the part of the dictionary we need.
//		DictionaryNode subDict;
		{
//			DictionaryLock lock;
//			subDict = s_dictTrees[iThread];
		}

		DictionaryNode& subDict = s_dictTrees[iThread];


		context->root = &subDict;


		const unsigned width = query.m_width;
		const unsigned height = query.m_height;

		// FIXME: make debug option.
		unsigned deadEnds = 0;

		if (false == subDict.IsLeaf())
		{
			uint64_t mortonX = ullMC2Dencode(0, 0);
			for (unsigned iX = 0; iX < width; ++iX)
			{
				uint64_t morton2D = mortonX;
				for (unsigned iY = 0; iY < height; ++iY)
				{
					// FIXME: debug.
					context->deadEnd = 0;

					TraverseBoard(*context, morton2D, &subDict);
//					if (true == subDict.IsLeaf())
//					{
//						debug_print("Dictionary exhausted for thread %u.\n", iThread);
//						break;
//					}

					if (0 == context->deadEnd)
					{
						++deadEnds;
					}

					morton2D = ullMC2Dyplusv(morton2D, 1);
				}

				mortonX = ullMC2Dxplusv(mortonX, 1);
			}
		}

		debug_print("Thread %u has %u dead ends in a %zu grid.\n", iThread, deadEnds, query.m_gridSize);
	}

private:
	inline unsigned GetWordScore(size_t length) const
	{
		const unsigned LUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return LUT[length-3];
	}

	inline static void TraverseBoard(ThreadContext& context, uint64_t mortonCode, DictionaryNode* parent)
	{
		const uint64_t iBoard = mortonCode;

		auto& board = context.board;
		const int tile = board[iBoard];
		const int letter = tile;

		DictionaryNode* node = parent->GetChild(letter);
		if (nullptr == node)
		{
			// Letter not found.
			return;
		}

		if (true == node->IsWord())
		{
			// Found a word.
			context.wordsFound.emplace_back(node->word);
			node->ClearWord();

			// FIXME: debug.
			context.deadEnd = 1;

			if (node->IsLeaf()) 
			{
				parent->RemoveChild(letter);
				return;
			}
		}

		assert(node->alphaBits != 0);

		// Recurse, as we've got a node that might be going somewhewre.
		// Before recursion, mark this board position as evaluated.
		board[iBoard] = 0;

		// FIXME: optimize this crude loop, unroll?

		const size_t gridSize = context.instance->m_gridSize;

		const int kNeighbours[8][2] = 
		{
			{ -1, -1 },
			{  0, -1 },
			{  1, -1 },
			{ -1,  1 },
			{  0,  1 },
			{  1,  1 },
			{ -1,  0 },
			{  1,  0 }
		};

		for (unsigned iNeighbour = 0; iNeighbour < 8; ++iNeighbour)
		{
			auto& neighbour = kNeighbours[iNeighbour];
			const int X = neighbour[0];
			const int Y = neighbour[1];

			// FIXME: this hurts.
			uint64_t newMorton = (X >= 0) ? ullMC2Dxplusv(mortonCode, X) : ullMC2Dxminusv(mortonCode, -X);
			newMorton = (Y >= 0) ? ullMC2Dyplusv(newMorton, Y) : ullMC2Dyminusv(newMorton, -Y);

			if (newMorton >= gridSize)
			{
				continue;
			}

			const int letterAdj = board[newMorton]; // adjMorton
			if (0 != letterAdj && nullptr != node->GetChild(letterAdj))
			{
				// FIXME: child meteen doorgeven?

				// Traverse, and if we hit the wall go see if what we're left with is a leaf.
				TraverseBoard(context, newMorton, node);
				if (true == node->IsLeaf())
				{
					// Restore board tile.
					board[iBoard] = letter;

					// Remove this node from it's parent, it's a dead end.
					parent->RemoveChild(letter);

					return;
				}
			}
		}

		// Open up this position on the board again.
		board[iBoard] = letter;
	}

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;
	const size_t m_gridSize;
};

Results FindWords(const char* board, unsigned width, unsigned height)
{
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
					const int sanity = toupper(letter);
					sanitized[morton2D] = sanity;
//					sanitized[iY*width + iX] = sanity;
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
		for (unsigned iWord = 0; iWord < results.Count; ++iWord)
		{
			delete[] results.Words[iWord];
		}
	}

	delete[] results.Words;
	results.Words = nullptr;

	results.Count = results.Score = 0;
	results.UserData = nullptr;
}
