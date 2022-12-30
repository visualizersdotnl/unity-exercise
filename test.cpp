
// #define USE_UNITY_REF_GRID
// #define PRINT_WORDS
// #define PRINT_GRID
// #define DUPE_CHECK

#define NUM_QUERIES 4 // Get things into cache nice 'n easy...

#define WIN32_CRT_BREAK_ALLOC -1 // 497 // 991000 // 1317291

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include <memory>
#include <chrono>
#include <vector>
#include <unordered_set>
#include <string>
#include <algorithm>

#include "api.h"
#include "random.h"

#include "spooling.h"

int main(int argC, char **arguments)
{
	printf("Boggle assignment solver by Niels J. de Wit, the undisputed heavyweight boggle champion!\n");
	
#if defined(_DEBUG) && defined(_WIN32)
	// Dump leak report at any possible exit.
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	
	// Report all to debug pane.
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);

	if (-1 != WIN32_CRT_BREAK_ALLOC)
		_CrtSetBreakAlloc(WIN32_CRT_BREAK_ALLOC);
#endif

	initialize_random_generator();

#ifndef USE_UNITY_REF_GRID

	printf("Generating grid using Mersenne-Twister.\n");

	if (nullptr == arguments[1] || nullptr == arguments[2])
	{
		printf("Please specify grid size (X, Y) on command line!\n");
		return 1;
	}

	const unsigned xSize = atoi(arguments[1]);
	const unsigned ySize = atoi(arguments[2]);

	const unsigned gridSize = xSize*ySize;
	
//	srand(42);
	std::unique_ptr<char[]> board(new char[gridSize]);
	char* write = board.get();

	for (unsigned iBoard = 0, iY = 0; iY < ySize; ++iY)
	{
		for (unsigned iX = 0; iX < xSize; ++iX, ++iBoard)
		{
			int random;
			do
			{
				random = mt_randu32() % 26;
			}
			while (random == 'U' - 'A'); // No 'U'
			const char character = 'A' + random;
			*write++ = character;

#ifdef PRINT_GRID
			printf("%c", character);
#endif
		}

#ifdef PRINT_GRID
		printf("\n");
#endif
	}

//	board[10] = '#'; // Breaks the board..

#else // USE_UNITY_REF_GRID

	const unsigned xSize = 3;
	const unsigned ySize = 3;
	const unsigned gridSize = xSize*ySize;
	std::unique_ptr<char[]> board(new char[gridSize]);
	const char* reference = "DZXEAIQUT"; // "dzxeaiqut";
	memcpy(board.get(), reference, gridSize*sizeof(char));

#endif

	printf("- Loading dictionary...\n");
//	const char *dictPath = "dictionary-short.txt";
	const char *dictPath = "dictionary.txt";
//	const char *dictPath = "dictionary-bigger.txt";
	LoadDictionary(dictPath);

	printf("- Finding in %ux%u... (%u iterations)\n", xSize, ySize, NUM_QUERIES);

	Results results[NUM_QUERIES];

	std::vector<std::chrono::microseconds> durations;


	for (unsigned iQuery = 0; iQuery < NUM_QUERIES; ++iQuery)
	{
		auto start = std::chrono::high_resolution_clock::now();
		results[iQuery] = FindWords(board.get(), xSize, ySize);
		auto end = std::chrono::high_resolution_clock::now();
		auto timing = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
		durations.push_back(timing);
	}

	for (unsigned iQuery = 0; iQuery < NUM_QUERIES; ++iQuery)
	{
		const auto count  = results[iQuery].Count;
		const auto score = results[iQuery].Score;
		printf("Results (run %u): ", iQuery+1);
		printf("count: %u, score: %u, duration %.3f\n", count, score, (float) durations[iQuery].count()*0.000001f - float(SPOOLING_TIME_IN_SEC));
	}

#ifdef PRINT_WORDS
	for (unsigned iWord = 0; iWord < results[0].Count; ++iWord) 
		printf("%s\n", results[0].Words[iWord]);	
#endif

#ifdef DUPE_CHECK
	std::unordered_set<std::string> words;
	for (unsigned iWord = 0; iWord < results[0].Count; ++iWord)
	{
		std::string word(results[0].Words[iWord]);
		auto pair = words.insert(word);
		if (false == pair.second)
		{
			printf("Word found twice: %s\n", word.c_str());
		}
	}
#endif

	for (unsigned iQuery = 0; iQuery < NUM_QUERIES; ++iQuery)
		FreeWords(results[iQuery]);
	
	FreeDictionary();

	// Sort so we can conveniently pick the fastest run
	std::sort(durations.begin(), durations.end());

	const float time = (float) durations[0].count();
	printf("\nSolver ran %u times, fastest: %.f microsec. (incl. %d sec. spooling) or approx. %.3f second(s)\n", (unsigned) NUM_QUERIES, time, SPOOLING_TIME_IN_SEC, time*0.000001f - float(SPOOLING_TIME_IN_SEC));

	return 0;
}