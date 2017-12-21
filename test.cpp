
// #define USE_UNITY_REF_GRID
#define PRINT_WORDS
#define PRINT_GRID

// To even out the timing results a little, since clock() isn't the sharpest of knives.
// WARNING: multiple queries causes leaks (no FreeWords() calls made except on the last set).
#define NUM_QUERIES 1

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include <memory>

#include "api.h"
#include "random.h"

int main(int argC, char **arguments)
{
	initialize_random_generator();

#ifndef USE_UNITY_REF_GRID

	if (nullptr == arguments[1] || nullptr == arguments[2])
	{
		printf("Please specify grid size (X, Y) on command line!\n");
		return 1;
	}

	const unsigned xSize = atoi(arguments[1]);
	const unsigned ySize = atoi(arguments[2]);
	const unsigned gridSize = xSize*ySize;
	
	srand(42);
	std::unique_ptr<char[]> board(new char[gridSize]);
	char* write = board.get();

	for (unsigned iBoard = 0, iY = 0; iY < ySize; ++iY)
	{
		for (unsigned iX = 0; iX < xSize; ++iX, ++iBoard)
		{
//			const int random = iBoard % 26;
			const int random = mt_rand32() % 26;
			const char character = 'a' + random;
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
	const char* reference = "dzxeaiqut";
	memcpy(board.get(), reference, gridSize*sizeof(char));

#endif

	printf("- Loading dictionary...\n");
	const char *dictPath = "dictionary.txt";
	LoadDictionary(dictPath);

	printf("- Finding in %ux%u...\n", xSize, ySize);

	const double start = clock();

	Results results;
	for (int i = 0; i < NUM_QUERIES; ++i)
		results = FindWords(board.get(), xSize, ySize);

	const double end = clock();

	printf("-- Results --\n");
	printf("Count: %u Score: %u\n", results.Count, results.Score);

#ifdef PRINT_WORDS
	for (unsigned iWord = 0; iWord < results.Count; ++iWord) 
		printf("%s\n", results.Words[iWord]);	
#endif

	FreeWords(results);
	FreeDictionary();

	printf("\nSolver ran %u times for avg. %.2f second(s), %.4f sec. per tile.\n", (unsigned) NUM_QUERIES, (end-start)/CLOCKS_PER_SEC/NUM_QUERIES, gridSize/(end-start));
	// ^^ Reports a false positive in Valgrind on OSX.

	return 0;
}
