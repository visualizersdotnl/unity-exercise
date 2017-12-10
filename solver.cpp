
/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	Please take a second to read this piece of text.

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

	IMPORTANT:
	- Compile with (at least) -O3 (or equivalent)!
	- I can't assume much about the test harness. 
	- I'm not printing anything. 
	- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
	- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
	  is shielded by a mutex and no globals are used.
	- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.

	The code style is of course according to personal preferences given the type and scope of this
	exercise. They're purely personal, I adapt to a company or client's way of working and most of all
	just try to keep things consistent. You'll also notice some Yoda notation here and there.. ;)

	I've written this in the latest OSX, but it should pretty much compile out of the box on most
	platforms that adhere to the standards.

	As for the approach, I am sure there are a few approaches and optimizations possible to improve performance
	but my primary goal was to be functional, readable and portable.

	To do (must):
	- Stash a bullshit character in the dictionary (eliminate 'covfefe') and the board to see where that goes.
	- Do a memory leak test (CRT or Valgrind).
	- Eliminate dictionary list (not needed).
	- Kill prints, write a README.TXT, pack, ship!
*/

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#include <vector>
#include <string>
#include <map>

#include "api.h"

// Disable before packing!
#define debug_print printf

// We'll be using a word tree built out of these simple nodes.
struct DictionaryNode
{
	bool IsWord() const
	{
		return false == word.empty();
	}

	std::string word; // FIXME: possible to track during traversal.
	std::map<char, DictionaryNode> children;
};

// We keep one dictionary at a time, but it's access is protected by a mutex, just to be safe.
static pthread_mutex_t s_dictMutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<std::string> s_dictionary;
static DictionaryNode s_dictTree;
static size_t s_longestWord = 0;

// Scoped lock for all dictionary globals.
class DictionaryLock
{
public:
	DictionaryLock()  { pthread_mutex_lock(&s_dictMutex);   }
	~DictionaryLock() { pthread_mutex_unlock(&s_dictMutex); }
};

// Input word must be lowercase!
static void AddWordToDictionary(const std::string& word)
{
	const size_t length = word.length();

	// Word not too short?
	if (word.length() < 3)
	{
		debug_print("Skipped word because it's got less than 3 letters: %s\n", word.c_str());
		return;
	}

	if (length > s_longestWord)
	{
		// Longest word thusfar (just in case I see use for that).
		s_longestWord = length;
	}

	DictionaryNode* current = &s_dictTree;

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;
		current = &current->children[letter];

		// Handle 'Qu' rule.
		if ('q' == letter)
		{
			auto next = iLetter+1;
			if (next == word.end() || *next != 'u') 
			{
				debug_print("Skipped word due to 'Qu' rule: %s\n", word.c_str());

				// This word can't be made with the boggle tiles due to the 'Qu' rule.
				return;
			}
			else
			{
				// Skip over 'u'.
				++iLetter;
			}
		}
	}

	current->word = word;
	s_dictionary.push_back(word);
}

void LoadDictionary(const char* path)
{
	// If the dictionary fails to load, you'll be left with an empty dictionary.
	DictionaryLock lock;
	s_dictionary.clear();

	if (nullptr == path)
		return;

	FILE* file = fopen(path, "r");
	if (nullptr == file)
		return;

	int character;
	std::string word;

	do
	{
		character = fgetc(file);
		if (true == isalpha((unsigned char) character))
		{
			// Boggle tiles are simply A-Z, where Q means 'Qu'.
			word += tolower(character);
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

	debug_print("Dictionary loaded. %zu words, longest being %zu characters.\n", s_dictionary.size(), s_longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;

	s_dictTree.word.clear();
	s_dictTree.children.clear();

	s_dictionary.clear();
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.
class Query
{
public:
	Query(Results &results, char* sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_board(sanitized)
,		m_width(width)
,		m_height(height)
,		m_gridSize(width*height)
	{
		DictionaryLock lock;
		m_tree = s_dictTree;
	}

	~Query() {}

	void Execute()
	{
		m_wordsFound.clear();

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

		// Copy words to Results structure and calculate the score.
		m_results.Count = m_wordsFound.size();
		m_results.Words = new char*[m_results.Count];
		m_results.Score = 0;
		
		char** words = const_cast<char**>(m_results.Words); // After all I own this data.
		for (const std::string &word:m_wordsFound)
		{
			// Uses full word to get the correct score.
			m_results.Score += GetWordScore(word);

			const size_t length = word.length();
			*words = new char[length+1];
			strcpy(*words++, word.c_str());
		}
	}

private:

	unsigned GetWordScore(const std::string& word) const
	{
		const unsigned LUT[] = { 1, 1, 2, 3, 5, 11 };
		size_t length = word.length();
		if (length > 8) length = 8;
		return LUT[length-3];
	}

	void TraverseBoard(unsigned iY, unsigned iX, DictionaryNode *parent)
	{
		const unsigned iBoard = iY*m_width + iX;

		const char letter = m_board[iBoard];

		// Using the MSB of the board to indicate if this tile has to be skipped (to avoid reuse of a letter).
		if (letter & ~0x7f)
		{
			return;
		}

		auto iNode = parent->children.find(letter);
		if (iNode == parent->children.end())
		{
			// This letter doesn't yield anything from this point onward.
			return;
		}

		DictionaryNode* node = &iNode->second;
		if (node->IsWord())
		{
			// Found a word.
			m_wordsFound.push_back(node->word);
//			debug_print("Word found: %s\n", node->word.c_str());

			// In this run we don't want to find this word again, so wipe it.
			node->word.clear();
		}

		// Before recursion, mark this board position as evaluated.
		m_board[iBoard] |= 128;

		const unsigned boundY = m_height-1;
		const unsigned boundX = m_width-1;

		// I've played around with different rolled and unrolled traversion calls & orders, and when unrolled it doesn't
		// seem to differ significantly where I go first, but to the eye this make sense.

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

		// Open up this position on the board again.
		m_board[iBoard] &= 0x7f;
	}


	Results& m_results;
	char* const m_board;
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
	if (nullptr != board && !(0 == width && 0 == height))
	{
		// Yes: sanitize it (check for illegal input and force all to lowercase).
		const unsigned gridSize = width*height;
		char* sanitized = new char[gridSize];
		for (unsigned iTile = 0; iTile < gridSize; ++iTile)
		{
			char letter = *board++;
			if (isalpha((unsigned char) letter))
			{
				sanitized[iTile] = tolower(letter);
			}
			else
			{
				// BAIL!
				// use std scoped ptr
			}
		}

		Query query(results, sanitized, width, height);
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

