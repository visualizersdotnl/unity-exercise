
// To do:
// - Leak check

// #define USE_UNITY_REF_GRID
// #define PRINT_WORDS
// #define PRINT_GRID

// To even out the timing results a little, since clock() isn't the sharpest of knives.
// WARNING: multiple queries causes leaks (no FreeWords() calls made except on the last set).
#define NUM_QUERIES 24

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "api.h"

int main(int argC, char **arguments)
{
	const char *dictPath = "dictionary.txt";
	LoadDictionary(dictPath);

#ifndef USE_UNITY_REF_GRID

	const unsigned xSize = atoi(arguments[1]);
	const unsigned ySize = atoi(arguments[2]);

	srand(42);
	char* board = new char[xSize*ySize];
	char* write = board;

	for (unsigned i = 0, iY = 0; iY < ySize; ++iY)
	{
		for (unsigned iX = 0; iX < xSize; ++iX, ++i)
		{
//			const int random = i % 25;
			const int random = rand() % 25;
			const char character = 'a' + random;
			*write++ = character;

#ifdef PRINT_GRID
			printf("%c", character);
#endif

			++i;
		}

#ifdef PRINT_GRID
		printf("\n");
#endif
	}

#else // USE_UNITY_REF_GRID

	const unsigned xSize = 3;
	const unsigned ySize = 3;
	const char *board = "dzxeaiqut";

#endif

	printf("Finding in %ux%u...\n", xSize, ySize);

	const double start = clock();

	Results results;
	for (int i = 0; i < NUM_QUERIES; ++i)
		results = FindWords(board, xSize, ySize);

	const double end = clock();

	printf("-- Results --\n");
	printf("Count: %u Score: %u\n", results.Count, results.Score);

#ifdef PRINT_WORDS
	for (unsigned iWord = 0; iWord < results.Count; ++iWord) printf("%s\n", results.Words[iWord]);	
#endif

	FreeWords(results);
	FreeDictionary();

	printf("Solver ran %u times for avg. %.5f second(s).\n", NUM_QUERIES, (end-start)/CLOCKS_PER_SEC/NUM_QUERIES);

	return 0;
}
