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
		- Per-thread visited array? Keep it?
		- Review threading strategy (tree balance), grid splits.
		- Fix Morton mess (most importantly non-power-of-2 grids and 32-bit support).
		- Clean up the mess in general (uint64_t).
		- Better early-outs!
		- FIXMEs
		- Platform testing.
		- More Valgrinding.

	Notes:
		- Compile with full optimization (-O3 for ex.) for best performance.
		- I could not assume anything about the test harness, so I did not; if you want debug output check debug_print().
		- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
		- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
		  is guarded by a mutex and no globals are used.
		- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.
	
	I've done leak testing using Valgrind in OSX and I seem to be in the clear; there are some inconclusive and (hopefully) irrelevant
	ones reported in the runtime library, but you shouldn't run into killer pileups.

	About:
		This code style is influenced by my personal preference (today) and the scope of this project.
		As a professional, I like to adapt and feel it's in everyone's interest to be consistent.

		I might have been a tad more verbose with my comments than usual ;)

		I've written this using the latest OSX, but it should compile out of the box on most platforms that adhere to
		the C++11 standard; it does on Linux and Windows.

		As for my approach: I wanted function, readability and portability. There are options worth considering that
		could improve performance, but for now I feel confident this is sufficient.

		There's a lot of pre and post query copying going on I'm not too happy about.
*/

// Make VC++ 2015 shut up and walk in line.
#define _CRT_SECURE_NO_WARNINGS 

// Cache-coherency swizzling: yes or no?
// #define DO_NOT_SWIZZLE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <vector>
#include <string>
#include <map>
// #include <unordered_map>
// #include <set>
#include <list>
#include <mutex>
#include <thread>
#include <atomic>
// #include <array>
// #include <deque>

#include "api.h"

// FIXME: 32-bit support, clean up the Morton mess, use 32-bit header!
#include "MZC2D64.h"

// #define debug_print printf
inline void debug_print(const char* format, ...) {}

const unsigned kAlphaRange = ('Z'-'A')+1;

// FIXME: this is of course a bit primitive, best look for available cores.
const unsigned kNumThreads = 1;

// I flag edge tiles.
const unsigned kEdgeBitX0  = (1 << 8);
const unsigned kEdgeBitX1  = (1 << 9);
const unsigned kEdgeBitY0  = (1 << 10);
const unsigned kEdgeBitY1  = (1 << 11);

// And, a bit per thread, the tiles I've visited during recursion.
const unsigned kTileFlagOffs = 12;

// The same board is used for all threads and this is the only bit that's written, that way the memory can be safely shared (for now).
inline unsigned TileVisitedFlag(unsigned iThread) 
{ 
	return 1 << (kTileFlagOffs+iThread); 
}

// This one is only to be used to isolate the actual letter (FIXME: I use 7 bits, but I could easily do with less by subtracting an offset).
const unsigned kTileLetterMask = 128-1;

// Lookup table to distribute root nodes among threads (FIXME: very primitive).
static std::vector<unsigned> s_letterThreadLUT;

static void CreateLetterLUT()
{
	const unsigned quotient = kAlphaRange/kNumThreads;

	for (unsigned iThread = 0; iThread < kNumThreads; ++iThread)
	{
		// FIXME: make this a range fill with a clearer head.
		for (unsigned iQ = 0; iQ < quotient; ++iQ)
		{
			s_letterThreadLUT.push_back(iThread);
		}
	}

	// Add remainder to last thread.
	unsigned remainder = kAlphaRange%kNumThreads;
	while (remainder--)
	{
		s_letterThreadLUT.push_back(kNumThreads-1);
	}

//	for (auto index : s_letterThreadLUT) debug_print("Letter LUT: %u\n", index);
//	debug_print("Letter LUT size %zu\n", s_letterThreadLUT.size());
}

inline unsigned LetterToIndex(char letter)
{
	const unsigned index = letter - 'A';
	return s_letterThreadLUT[index];
}

// We'll be using a word tree built out of these simple nodes.
class DictionaryNode
{
public:
	bool IsWord() const
	{
		return false == word.empty();
	}

	// FIXME: I can eliminate this, and store 2 bits to determine if it's a word and if there's a 'Qu' in it.
	std::string word;

	std::map<char, DictionaryNode> children;
//	unsigned prefixCount;
};

// We keep one dictionary (in subsets) at a time, but it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;
static std::vector<DictionaryNode> s_dictTrees;

// Scoped lock for all dictionary globals.
class DictionaryLock
{
public:
	DictionaryLock() :
		m_lock(s_dictMutex) {}

private:
	std::lock_guard<std::mutex> m_lock;
};

// Input word must be uppercase!
static void AddWordToDictionary(const std::string& word, size_t& longestWord, size_t& wordCount)
{
	const size_t length = word.length();

	// Word not too short?
	if (word.length() < 3)
	{
		debug_print("Skipped word because it's got less than 3 letters: %s\n", word.c_str());
		return;
	}

	if (length > longestWord)
	{
		// Longest word thus far (just a print statistic).
		longestWord = length;
	}

	// As a first strategy we'll split at the root, disregarding the balance (FIXME).
	DictionaryNode* current = &s_dictTrees[LetterToIndex(word[0])];

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;
		current = &current->children[letter];

		// Handle 'Qu' rule.
		if ('Q' == letter)
		{
			auto next = iLetter+1;
			if (next == word.end() || *next != 'U') 
			{
				debug_print("Skipped word due to 'Qu' rule: %s\n", word.c_str());

				// This word can't be made with the boggle tiles due to the 'Qu' rule.
				return;
			}
			else
			{
				// Skip over 'U'.
				++iLetter;
			}
		}

//		++current->prefixCount;
	}

	current->word = word;
	++wordCount;
}

void LoadDictionary(const char* path)
{
	// FIXME: initialize through global construction.
	CreateLetterLUT();

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
	size_t longestWord = 0, wordCount = 0;

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
				AddWordToDictionary(word, longestWord, wordCount);
				word.clear();
			}
		}
	}
	while (EOF != character);

	fclose(file);

	debug_print("Dictionary loaded. %zu words, longest being %zu characters\n", wordCount, longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;
	s_dictTrees.resize(kNumThreads, DictionaryNode());
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.

// typedef std::vector<std::atomic<int>> Board;
typedef std::vector<int> Board;

class Query
{
public:
	Query(Results& results, Board& sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_board(sanitized)
,		m_width(width)
,		m_height(height)
,		m_gridSize(width*height) 
	{
	}

	~Query() {}

private:
	class ThreadContext
	{
	public:
		ThreadContext(unsigned iThread, Query* instance) :
		iThread(iThread)
,		instance(instance)
,		visited(new bool[instance->m_gridSize]) 
		{
			memset(visited.get(), 0, instance->m_gridSize*sizeof(bool));
		}

		~ThreadContext() {}

		// FIXME: references!
		const unsigned iThread;
		Query* instance; // FIXME: what do I really want to know?
		std::vector<std::string> wordsFound;
		std::unique_ptr<bool[]> visited;
	};

public:
	void Execute()
	{
		// Just in case another Execute() call is made on the same context: avoid leaking.
		if (nullptr != m_results.Words)
		{
			FreeWords(m_results);
		}

		// Get a copy of the current dictionary.
		// TraverseBoard() changes the local copy at runtime, so we need this in case of multiple Execute() calls.
		GetLatestDictionary();

		// Kick off threads.
		const unsigned numThreads = kNumThreads;

		std::vector<std::thread*> threads;
		std::vector<ThreadContext*> contexts; // FIXME: can go into TLS.

		debug_print("Kicking off %u threads.\n", numThreads);

		for (unsigned iThread = 0; iThread < numThreads; ++iThread)
		{
			contexts.push_back(new ThreadContext(iThread, this));
			threads.push_back(new std::thread(ExecuteThread, contexts[iThread], &m_trees[iThread]));
		}

		for (auto* thread : threads)
		{
			thread->join();
			delete thread;
		}

		m_results.Count = 0;
		for (auto* context : contexts)
		{
			const unsigned count = (unsigned) context->wordsFound.size();
			m_results.Count += count;
			debug_print("Thread %u joined with %u words.\n", context->iThread, count);
		}

		// Copy words to Results structure and calculate the score.
		m_results.Words = new char*[m_results.Count];
		m_results.Score = 0;
		
		char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.
		for (auto* context : contexts)
		{
			for (auto word : context->wordsFound)
			{
				const size_t length = word.length();

				// Uses full word to get the correct score.
				m_results.Score += GetWordScore(length);

				// FIXME: this takes a fucking second or more..
				*words_cstr = new char[length+1];
				strcpy(*words_cstr++, word.c_str());
			}

			delete context;
		}
	}

private:
	static void ExecuteThread(ThreadContext* context, DictionaryNode* parent)
	{
		// FIXME: ref.
		Query& query = *context->instance;

		const unsigned width = query.m_width;
		const unsigned height = query.m_height;

#if defined(DO_NOT_SWIZZLE)
		if (false == parent->children.empty())
		{
			for (unsigned iY = 0; iY < height; ++iY)
			{
				for (unsigned iX = 0; iX < width; ++iX)
				{
					TraverseBoard(context, iY, iX, parent);
				}
			}
		}
#else
		if (false == parent->children.empty())
		{
			if (0) // (context->iThread & 1)
			{
				uint64_t mortonY = ullMC2Dencode(0, 0);
				for (unsigned iY = 0; iY < height; ++iY)
				{
					uint64_t morton2D = mortonY;
					for (unsigned iX = 0; iX < width; ++iX)
					{
						TraverseBoard(context, morton2D, parent);
						morton2D = ullMC2Dxplusv(morton2D, 1);
					}

					mortonY = ullMC2Dyplusv(mortonY, 1);
				}
			}
			else
			{
				uint64_t mortonX = ullMC2Dencode(0, 0);
				for (unsigned iX = 0; iX < width; ++iX)
				{
					uint64_t morton2D = mortonX;
					for (unsigned iY = 0; iY < height; ++iY)
					{
						TraverseBoard(context, morton2D, parent);
						morton2D = ullMC2Dyplusv(morton2D, 1);
					}

					mortonX = ullMC2Dxplusv(mortonX, 1);
				}
			}
		}
#endif
	}

private:

	void GetLatestDictionary()
	{
		DictionaryLock lock;
		m_trees = s_dictTrees;
	}

	inline unsigned GetWordScore(size_t length) const
	{
		const unsigned LUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return LUT[length-3];
	}

#if defined(DO_NOT_SWIZZLE)

	// NOTE: this is not kept up to date momentarily.

	inline static void TraverseBoard(ThreadContext* context, unsigned iY, unsigned iX, DictionaryNode* parent)
	{
		// This is safe since we won't be fiddling with data except thread-specific bits.
		Board& board = context->instance->m_board;

		const unsigned width = context->instance->m_width;
		const unsigned height = context->instance->m_height;

		const unsigned iBoard = iY*width + iX;
		const int tile = board[iBoard];

		// Using a bit on the board to indicate if this tile has to be skipped (to avoid reuse of a letter).
		const unsigned kTileVisitedBit = TileVisitedFlag(context->iThread);
		if (tile & kTileVisitedBit)
		{
			return;
		}

		const char letter = tile & kTileLetterMask;
		auto iNode = parent->children.find(letter);
		if (iNode == parent->children.end())
		{
			// This letter doesn't yield anything from this point onward.
			return;
		}

		DictionaryNode* node = &iNode->second;
		if (true == node->IsWord())
		{
			// Found a word.
			context->wordsFound.push_back(node->word);
//			debug_print("Word found: %s\n", node->word.c_str());

			// In this run we don't want to find this word again, so wipe it.
			node->word.clear();
		}

		// Recurse if necessary (i.e. more letters to look for).
		if (false == node->children.empty())
		{
			// Before recursion, mark this board position as evaluated.
			board[iBoard] |= kTileVisitedBit;

			const unsigned boundY = height-1;
			const unsigned boundX = width-1;

			// Top row.
			if (iY > 0) 
			{
				TraverseBoard(context, iY-1, iX, node);
				if (iX > 0) TraverseBoard(context, iY-1, iX-1, node);
				if (iX < boundX) TraverseBoard(context, iY-1, iX+1, node);
			}

			// Bottom row.
			if (iY < boundY)
			{
				TraverseBoard(context, iY+1, iX, node); 
				if (iX > 0) TraverseBoard(context, iY+1, iX-1, node); 
				if (iX < boundX) TraverseBoard(context, iY+1, iX+1, node); 
			}

			if (iX > 0)
			{
				// Left.
				TraverseBoard(context, iY, iX-1, node);
			}

			if (iX < boundX)
			{
				// Right.
				TraverseBoard(context, iY, iX+1, node);
			}

			// Open up this position on the board again.
			board[iBoard] &= ~kTileVisitedBit;
		}
	}
#else
	inline static void TraverseBoard(ThreadContext* context, uint64_t mortonCode, DictionaryNode* parent)
	{
		const uint64_t iBoard = mortonCode;

		bool& visited = context->visited[iBoard];
		if (true == visited)
			return;

		// This is safe since we won't be fiddling with data except thread-specific bits, maybe.
		Board& board = context->instance->m_board;
		const int tile = board[iBoard];

/*
		// Using a bit on the board to indicate if this tile has to be skipped (to avoid reuse of a letter).
		const unsigned kTileVisitedBit = TileVisitedFlag(context->iThread);
		if (tile & kTileVisitedBit)
		{
			return;
		}
*/

		const char letter = tile & kTileLetterMask;
		auto iNode = parent->children.find(letter);
		if (iNode == parent->children.end())
		{
			// This letter doesn't yield anything from this point onward.
			return;
		}

		DictionaryNode* node = &iNode->second;
		if (true == node->IsWord())
		{
			// Found a word.
			context->wordsFound.push_back(node->word);
//			debug_print("Word found: %s\n", node->word.c_str());

			if (true == node->children.empty())
			{
				// End of the line? Kill it.
				parent->children.erase(iNode);
				return;
			}
			else
			{
				// In this run we don't want to find this word again, so wipe it.
				node->word.clear();
			}
		}

		// Recurse if necessary (i.e. more words to look for).
		if (false == node->children.empty())
		{
			// Before recursion, mark this board position as evaluated.
//			board[iBoard] |= kTileVisitedBit;
			visited = true;

			const bool edgeX0 = tile & kEdgeBitX0;
			const bool edgeX1 = tile & kEdgeBitX1;

			// Top row.
			if (0 == (tile & kEdgeBitY0))
			{
				const uint64_t mortonY0 = ullMC2Dyminusv(mortonCode, 1);
				TraverseBoard(context, mortonY0, node);
				if (!edgeX0) TraverseBoard(context, ullMC2Dxminusv(mortonY0, 1), node);
				if (!edgeX1) TraverseBoard(context, ullMC2Dxplusv(mortonY0, 1), node);
			}

			// Bottom row.
			if (0 == (tile & kEdgeBitY1))
			{
				const uint64_t mortonY1 = ullMC2Dyplusv(mortonCode, 1);
				TraverseBoard(context, mortonY1, node); 
				if (!edgeX0) TraverseBoard(context, ullMC2Dxminusv(mortonY1, 1), node); 
				if (!edgeX1) TraverseBoard(context, ullMC2Dxplusv(mortonY1, 1), node); 
			}

			if (!edgeX0)
			{
				// Left.
				const uint64_t mortonX0 = ullMC2Dxminusv(mortonCode, 1);
				TraverseBoard(context, mortonX0, node);
			}

			if (!edgeX1)
			{
				// Right.
				const uint64_t mortonX1 = ullMC2Dxplusv(mortonCode, 1);
				TraverseBoard(context, mortonX1, node);
			}

			// Open up this position on the board again.
//			board[iBoard] &= ~kTileVisitedBit;
			visited = false;
		}
	}
#endif

	Results& m_results;
	Board& m_board;
	const unsigned m_width, m_height;
	const size_t m_gridSize;

	std::vector<DictionaryNode> m_trees;
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
		// Yes: sanitize it (check for illegal input and force all to lowercase).
		const unsigned gridSize = width*height;
		Board sanitized(gridSize);

#if defined(DO_NOT_SWIZZLE)
		for (unsigned iY = 0; iY < height; ++iY)
		{
			const int yEdgeBit = (iY == 0) ? kEdgeBitY0 : (iY == height-1) ? kEdgeBitY1 : 0;
			for (unsigned iX = 0; iX < width; ++iX)
			{
				const int xEdgeBit = (iX == 0) ? kEdgeBitX0 : (iX == width-1) ? kEdgeBitX1 : 0;
				const char letter = *board++;
				if (0 != isalpha((unsigned char) letter))
				{
					int sanity = toupper(letter);
					sanity |= yEdgeBit | xEdgeBit;
					sanitized[iY*width + iX] = sanity;
				}
				else
				{
					// Invalid character: skip query.
					return results;
				}
			}
		}
#elif !defined (DO_NOT_SWIZZLE)
		// FIXME: this RW pattern can be faster, look at Ryg's article.

		debug_print("Grid swizzle (cache optimization) enabled.\n");

		for (unsigned iY = 0; iY < height; ++iY)
		{
			const int yEdgeBit = (iY == 0) ? kEdgeBitY0 : (iY == height-1) ? kEdgeBitY1 : 0;
			for (unsigned iX = 0; iX < width; ++iX)
			{
				const int xEdgeBit = (iX == 0) ? kEdgeBitX0 : (iX == width-1) ? kEdgeBitX1 : 0;
				const char letter = *board++;
				if (0 != isalpha((unsigned char) letter))
				{
					int sanity = toupper(letter);
					sanity |= yEdgeBit|xEdgeBit;

					const uint64_t mortonCode = ullMC2Dencode(iX, iY);
					sanitized[mortonCode] = sanity;
				}
				else
				{
					// Invalid character: skip query.
					return results;
				}
			}
		}
#endif

		Query query(results, sanitized, width, height);
		query.Execute();
//		query.Execute(); // Leak test..
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
