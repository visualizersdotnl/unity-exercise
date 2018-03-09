
// #define USE_UNITY_REF_GRID
// #define PRINT_WORDS
// #define PRINT_GRID

#define NUM_QUERIES 4

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include <memory>
#include <chrono>

#include "api.h"
#include "random.h"

int main(int argC, char **arguments)
{
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
	const char *dictPath = "dictionary.txt";
//	const char *dictPath = "dictionary-bigger.txt";
	LoadDictionary(dictPath);

	printf("- Finding in %ux%u... (%u iterations)\n", xSize, ySize, NUM_QUERIES);

	auto start = std::chrono::high_resolution_clock::now();

	Results results[NUM_QUERIES];
	for (unsigned iQuery = 0; iQuery < NUM_QUERIES; ++iQuery)
		results[iQuery] = FindWords(board.get(), xSize, ySize);

	auto end = std::chrono::high_resolution_clock::now();
	auto timing = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

	printf("-- Results (first run) --\n");
	printf("Count: %u Score: %u\n", results[0].Count, results[0].Score);

#ifdef PRINT_WORDS
	for (unsigned iWord = 0; iWord < results[0].Count; ++iWord) 
		printf("%s\n", results[0].Words[iWord]);	
#endif

	for (unsigned iQuery = 0; iQuery < NUM_QUERIES; ++iQuery)
		FreeWords(results[iQuery]);
	
	FreeDictionary();

	const float time = timing.count();
	const float avgTime = time/NUM_QUERIES;
	printf("\nSolver ran %u times for avg. %.2f MS or approx. %.2f second(s)\n", (unsigned) NUM_QUERIES, avgTime, avgTime*0.001f);

	return 0;
}
