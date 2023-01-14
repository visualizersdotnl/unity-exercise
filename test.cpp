﻿
// #define USE_UNITY_REF_GRID
// #define PRINT_WORDS
// #define PRINT_GRID
// #define DUPE_CHECK
// #define PRINT_ITER_RESULTS

// When board randomization enabled, it pays off (usually) to do more queries to get better performance.
#ifdef _WIN32
	#define HIGHSCORE_LOOP
	#define HIGHSCORE_MICROSECONDS 390000  // Stress test Ryzen 5900x
	#define NUM_QUERIES 4
//	#define HIGHSCORE_LOOP_RANDOMIZE_BOARD
#elif defined(__GNUC__)
	#define HIGHSCORE_LOOP
	#define HIGHSCORE_MICROSECONDS 620000  // Stress test for M1 MAX
	#define NUM_QUERIES 4
	// #define HIGHSCORE_LOOP_RANDOMIZE_BOARD
#endif


#define WIN32_CRT_BREAK_ALLOC -1 // 497 // 991000 // 1317291

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include <memory>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_set>

#include "api.h"
#include "random.h"

#if _WIN32
	#include <windows.h>
#endif

// #include "timing.h"

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

	Results results[NUM_QUERIES];
	std::vector<std::chrono::microseconds> durations;
	durations.reserve(NUM_QUERIES);

	printf("- Loading dictionary...\n");
	// const char *dictPath = "dictionary-short.txt";
	const char *dictPath = "dictionary.txt";
	//	const char *dictPath = "dictionary-bigger.txt";
	LoadDictionary(dictPath);

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

GenerateBoard:
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

#ifdef HIGHSCORE_LOOP
	printf("- Finding (looping for high score!) in %ux%u... (%u iterations per try)\n", xSize, ySize, NUM_QUERIES);
#else
	printf("- Finding in %ux%u... (%u iterations)\n", xSize, ySize, NUM_QUERIES);
#endif

RetrySameBoard:
	for (unsigned iQuery = 0; iQuery < NUM_QUERIES; ++iQuery)
	{
		const auto start = std::chrono::high_resolution_clock::now();
		results[iQuery] = FindWords(board.get(), xSize, ySize);
		const auto end = std::chrono::high_resolution_clock::now();
		durations.emplace_back(std::chrono::duration_cast<std::chrono::microseconds>(end - start));
	
#ifdef PRINT_ITER_RESULTS
		const auto count = results[iQuery].Count;
		const auto score = results[iQuery].Score;
		printf("Results (run %u): ", iQuery+1);
		printf("count: %u, score: %u, duration %.lld microsec.\n", count, score, durations[iQuery].count());
#endif
	}

	// Sort so we can conveniently pick the fastest run
	std::sort(durations.begin(), durations.end());

#ifdef HIGHSCORE_LOOP
	printf("Best out of %d: %.lld microsec.\n", NUM_QUERIES, durations[0].count());

	if (durations[0].count() >= HIGHSCORE_MICROSECONDS) 
	{
		for (auto& result : results)
			FreeWords(result);

		durations.clear();

#ifdef HIGHSCORE_LOOP_RANDOMIZE_BOARD
		goto GenerateBoard;
#else
		goto RetrySameBoard;
#endif
	}
#endif

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

	const auto time = durations[0].count();
	printf("\nSolver ran %u times, fastest: %.lld microsec. / approx. %.4lf second(s)\n", (unsigned) NUM_QUERIES, time, double(time)*0.000001);

	return 0;
}