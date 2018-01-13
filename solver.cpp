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
		- Fix Morton mess (most importantly non-power-of-2 grids and 32-bit support).
		- FIXMEs
		- What's with this dynamic programming deal? Seems like I could use early outs.
		- Cache-wise I perform favorably now, my tile cost decreasing (or steadying at least) with bigger grids.

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

		There's a lot of pre- and post-query copying going on I'm not too happy about.
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
#include <mutex>

#include "api.h"

// FIXME: 32-bit support, clean up the Morton mess, use 32-bit header!
#include "MZC2D64.h"

// #define debug_print printf
inline void debug_print(const char* format, ...) {}

// I'm flagging tiles of my sanitized copy of the board to prevent reuse of letters in a word, and I'm flagging the edges.
const unsigned kEdgeBitX0      = (1 << 7);
const unsigned kEdgeBitX1      = (1 << 8);
const unsigned kEdgeBitY0      = (1 << 9);
const unsigned kEdgeBitY1      = (1 << 10);
const unsigned kTileVisitedBit = (1 << 11);
const unsigned kTileEdgeMask   = kEdgeBitX0|kEdgeBitX1|kEdgeBitY0|kEdgeBitY1;
const unsigned kTileFlagMask   = kEdgeBitX0|kEdgeBitX1|kEdgeBitY0|kEdgeBitY1|kTileVisitedBit;

// We'll be using a word tree built out of these simple nodes.
struct DictionaryNode
{
	bool IsWord() const
	{
		return false == word.empty();
	}

	// FIXME: I can eliminate this, and store 2 bits to determine if it's a word and if there's a 'Qu' in it.
	std::string word;

	std::map<char, DictionaryNode> children;
	unsigned prefixCount;
};

// We keep one dictionary at a time, but it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;
static DictionaryNode s_dictTree;

// Scoped lock for all dictionary globals.
class DictionaryLock
{
public:
	DictionaryLock() :
		m_lock(s_dictMutex) {}

private:
	std::lock_guard<std::mutex> m_lock;
};

// Input word must be lowercase!
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
		// Longest word thusfar (just a print statistic).
		longestWord = length;
	}

	DictionaryNode* current = &s_dictTree;

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

		++current->prefixCount;
	}

	current->word = word;
	++wordCount;
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

	debug_print("Dictionary loaded. %zu words, longest being %zu characters.\n", wordCount, longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;
	s_dictTree.word.clear();
	s_dictTree.children.clear();
	s_dictTree.prefixCount = 0;
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.

class Query
{
public:
	Query(Results& results, int* sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_board(sanitized)
,		m_width(width)
,		m_height(height)
,		m_gridSize(width*height) {}

	~Query() {}

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

		m_wordsFound.clear();

#if defined(DO_NOT_SWIZZLE)
		if (false == m_tree.children.empty())
		{
			for (unsigned iY = 0; iY < m_height; ++iY)
			{
				for (unsigned iX = 0; iX < m_width; ++iX)
				{
					TraverseBoard(iY, iX, &m_tree);
				}
			}
		}
#else
		if (false == m_tree.children.empty())
		{
			unsigned mortonY = ullMC2Dencode(0, 0);
			for (unsigned iY = 0; iY < m_height; ++iY)
			{
				unsigned morton2D = mortonY;
				for (unsigned iX = 0; iX < m_width; ++iX)
				{
					TraverseBoard(morton2D, &m_tree);
					morton2D = ullMC2Dxplusv(morton2D, 1);
				}

				mortonY = ullMC2Dyplusv(mortonY, 1);
			}
		}
#endif

		// Copy words to Results structure and calculate the score.
		m_results.Count = (unsigned) m_wordsFound.size();
		m_results.Words = new char*[m_results.Count];
		m_results.Score = 0;
		
		char** words = const_cast<char**>(m_results.Words); // After all I own this data.
		for (const std::string& word : m_wordsFound)
		{
			const size_t length = word.length();

			// Uses full word to get the correct score.
			m_results.Score += GetWordScore(length);

			// FIXME: this takes a fucking second or more..
			*words = new char[length+1];
			strcpy(*words++, word.c_str());
		}
	}

private:

	void GetLatestDictionary()
	{
		DictionaryLock lock;
		m_tree = s_dictTree;
	}

	inline int& Board(unsigned index)
	{
		return m_board[index];
	}

	inline unsigned GetWordScore(size_t length) const
	{
		const unsigned LUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return LUT[length-3];
	}

#if defined(DO_NOT_SWIZZLE)

	// NOTE: this path isn't really optimized anymore.
	// An idea can be to use it for smaller boards.

	inline void TraverseBoard(unsigned iY, unsigned iX, DictionaryNode* parent)
	{
		const unsigned iBoard = iY*m_width + iX;
		const int tile = Board(iBoard);

		// Using a bit on the board to indicate if this tile has to be skipped (to avoid reuse of a letter).
		if (tile & kTileVisitedBit)
		{
			return;
		}

		const char letter = tile & ~kTileFlagMask;
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
			m_wordsFound.push_back(node->word);
//			debug_print("Word found: %s\n", node->word.c_str());

			// In this run we don't want to find this word again, so wipe it.
			node->word.clear();
		}

		// Recurse if necessary (i.e. more letters to look for).
		if (false == node->children.empty())
		{
			// Before recursion, mark this board position as evaluated.
			Board(iBoard) |= kTileVisitedBit;

			const unsigned boundY = m_height-1;
			const unsigned boundX = m_width-1;

			// Top row.
			if (iY > 0) 
			{
				TraverseBoard(iY-1, iX, node);
				if (iX > 0) TraverseBoard(iY-1, iX-1, node);
				if (iX < boundX) TraverseBoard(iY-1, iX+1, node);
			}

			// Bottom row.
			if (iY < boundY)
			{
				TraverseBoard(iY+1, iX, node); 
				if (iX > 0) TraverseBoard(iY+1, iX-1, node); 
				if (iX < boundX) TraverseBoard(iY+1, iX+1, node); 
			}

			if (iX > 0)
			{
				// Left.
				TraverseBoard(iY, iX-1, node);
			}

			if (iX < boundX)
			{
				// Right.
				TraverseBoard(iY, iX+1, node);
			}

			// Open up this position on the board again.
			Board(iBoard) &= ~kTileVisitedBit;
		}
	}
#else
	inline void TraverseBoard(unsigned mortonCode, DictionaryNode* parent)
	{
		const unsigned iBoard = mortonCode;
		const int tile = Board(iBoard);

		// Using a bit on the board to indicate if this tile has to be skipped (to avoid reuse of a letter).
		if (tile & kTileVisitedBit)
		{
			return;
		}

		const char letter = tile & ~kTileFlagMask;
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
			m_wordsFound.push_back(node->word);
//			debug_print("Word found: %s\n", node->word.c_str());

			// In this run we don't want to find this word again, so wipe it.
			node->word.clear();

			// And get rid of the child node if necessary.
			--node->prefixCount;
			if (0 == node->prefixCount)
			{
				parent->children.erase(iNode);
				return;
			}
		}

		// Recurse if necessary (i.e. more words to look for).
		if (node->prefixCount > 0)
		{
			// Before recursion, mark this board position as evaluated.
			Board(iBoard) |= kTileVisitedBit;
//			++recursion;

			const bool edgeX0 = tile & kEdgeBitX0;
			const bool edgeX1 = tile & kEdgeBitX1;

			// Top row.
			if (0 == (tile & kEdgeBitY0))
			{
				const unsigned mortonY0 = ullMC2Dyminusv(mortonCode, 1);
				TraverseBoard(mortonY0, node);
				if (!edgeX0) TraverseBoard(ullMC2Dxminusv(mortonY0, 1), node);
				if (!edgeX1) TraverseBoard(ullMC2Dxplusv(mortonY0, 1), node);
			}

			// Bottom row.
			if (0 == (tile & kEdgeBitY1))
			{
				const unsigned mortonY1 = ullMC2Dyplusv(mortonCode, 1);
				TraverseBoard(mortonY1, node); 
				if (!edgeX0) TraverseBoard(ullMC2Dxminusv(mortonY1, 1), node); 
				if (!edgeX1) TraverseBoard(ullMC2Dxplusv(mortonY1, 1), node); 
			}

			if (!edgeX0)
			{
				// Left.
				const unsigned mortonX0 = ullMC2Dxminusv(mortonCode, 1);
				TraverseBoard(mortonX0, node);
			}

			if (!edgeX1)
			{
				// Right.
				const unsigned mortonX1 = ullMC2Dxplusv(mortonCode, 1);
				TraverseBoard(mortonX1, node);
			}

			// Open up this position on the board again.
			Board(iBoard) &= ~kTileVisitedBit;
//			--recursion;
		}
	}
#endif

	Results& m_results;
	int* const m_board;
	const unsigned m_width, m_height;
	const size_t m_gridSize;

	DictionaryNode m_tree;	

	std::vector<std::string> m_wordsFound;
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

#if defined(DO_NOT_SWIZZLE)
		std::unique_ptr<int[]> sanitized(new int[gridSize]);

		for (unsigned iY = 0; iY < height; ++iY)
		{
//			const int yEdgeBit = (iY == 0) ? kEdgeBitY0 : (iY == height-1) ? kEdgeBitY1 : 0;
			for (unsigned iX = 0; iX < width; ++iX)
			{
//				const int xEdgeBit = (iX == 0) ? kEdgeBitX0 : (iX == width-1) ? kEdgeBitX1 : 0;
				const char letter = *board++;
				if (0 != isalpha((unsigned char) letter))
				{
					int sanity = toupper(letter);
//					sanity |= yEdgeBit | xEdgeBit;
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
		debug_print("Grid swizzle (cache optimization) enabled.\n");

		std::unique_ptr<int[]> sanitized(new int[gridSize]);

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

					// FIXME: this RW pattern can be faster, look at Fabian Giesen's article.
					const unsigned mortonCode = ullMC2Dencode(iX, iY);
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

		Query query(results, sanitized.get(), width, height);
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
