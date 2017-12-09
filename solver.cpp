
/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017.

	Rules:
		- Only use the same word once.
		- Can't reuse a letter in the same word.
		- There's no 'q', but there is 'qu'.
		- Words must be at least 3 letters long.

	Scoring:
		- 3, 4 = 1
		- 5 = 2
		- 6 = 3
		- 7 = 5
		- >7 = 11

	Notes:
		I can't assume anything about the test harness, so I'm not printing anything; if the input dictionary
		could not be loaded, the current dictionary will be empty and FindWords() will simply yield zero result.

		If an invalid board is supplied, I'll report back 1 word in the Results structure indicating this was the case.

		The code style is of course according to personal preferences given the type and scope of this
		exercise. They're purely personal, I adapt to a company or client's way of working and most of all
		just try to keep things consistent. You'll notice some Yoda notation here and there.. ;)

		I've written this in the latest OSX, but it should pretty much compile out of the box on most
		platforms that adhere to the standards.

	To do:
	- Eliminate dictionary list.
	- Optimize traversal & sanitize (check) supplied board.
	- Full word in tree nodes: kill it?
	- Use hashes?
	- Let Albert run it through his own test?
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

// Note-to-self: before packaging comment this one, comment lines where used.
#define debug_print printf

// We'll be using a word tree built out of these simple nodes.
struct DictionaryNode
{
	bool IsWord() const
	{
		return false == word.empty();
	}

	std::string word; // FIXME: perhaps find a better way?
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
		if (true == isalpha(character))
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
	Query(Results &results, const char* board, unsigned width, unsigned height) :
		m_results(results)
,		m_board(board)
,		m_width(width)
,		m_height(height)
,		m_gridSize(width*height)
	{
		DictionaryLock lock;
		m_tree = s_dictTree;

		// FIXME: temporary.
		m_visited = new bool[m_gridSize];
		memset(m_visited, 0, m_gridSize);
	}

	~Query() 
	{
		delete[] m_visited;
	}

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
		
		char** words = const_cast<char**>(m_results.Words); // Not too pretty, but I own this data.
		for (const std::string &word:m_wordsFound)
		{
			m_results.Score += WordScore(word);

			const size_t length = word.length();
			*words = new char[length+1];
			strcpy(*words++, word.c_str());
		}
	}

private:

	unsigned WordScore(const std::string& word)
	{
		const unsigned LUT[] = { 1, 1, 2, 3, 5, 11 };
		size_t length = word.length();
		if (length > 8) length = 8;
		return LUT[length-3];
	}

	void TraverseBoard(unsigned iY, unsigned iX, DictionaryNode *parent)
	{
		const unsigned iBoard = iY*m_width + iX;

		if (true == m_visited[iBoard])
		{
			// Can't use the same letter twice in 1 word: bail.
			return;
		}

		const char letter = m_board[iBoard];

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
		m_visited[iBoard] = true;

		const unsigned boundY = m_height-1;
		const unsigned boundX = m_width-1;

		if (iX > 0) 
		{
			TraverseBoard(iY, iX-1, node); // Left.
			if (iY > 0) TraverseBoard(iY-1, iX-1, node); // Top left.
			if (iY < boundY) TraverseBoard(iY+1, iX-1, node); // Bottom left.
		}

		if (iX < boundX)
		{
			TraverseBoard(iY, iX+1, node); // Right.
			if (iY > 0) TraverseBoard(iY-1, iX+1, node); // Top right.
			if (iY < boundY) TraverseBoard(iY+1, iX+1, node); // Bottom right.
		}

		if (iY > 0)
		{
			// Top.
			TraverseBoard(iY-1, iX, node);
		}

		if (iY < boundY)
		{
			// Bottom.
			TraverseBoard(iY+1, iX, node);
		}

		// FIXME: re-evaluate the above tomorrow :-)

		// Open up this position on the board again.
		m_visited[iBoard] = false;
	}


	Results& m_results;
	const char* m_board;
	const unsigned m_width, m_height;
	const size_t m_gridSize;

	DictionaryNode m_tree;	
	bool* m_visited; // FIXME: optimize into copy of board (bit).

	std::vector<std::string> m_wordsFound;
};


Results FindWords(const char* board, unsigned width, unsigned height)
{
	Results results;
	results.Words = nullptr;
	results.Count = 0;
	results.Score = 0;
	results.UserData = nullptr; // Unused, didn't spend time threading the algorithm myself.

	// Sane board?
	if (nullptr != board && !(0 == width && 0 == height))
	{
		// Convert board to lowercase, just to be sure.
		// I'm not going to assume there might be garbage in it.
		// ...

		Query query(results, board, width, height);
		query.Execute();
	}

	return results;
}

void FreeWords(Results results)
{
	// ...
}

