
// To do:
// - Timing
// - Leak check

// #define UNITY_GRID 

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "api.h"

int main(int argC, char **arguments)
{
	const char *dictPath = "dictionary.txt";
	LoadDictionary(dictPath);

#ifndef UNITY_GRID
	const unsigned xSize = atoi(arguments[1]);
	const unsigned ySize = atoi(arguments[2]);

	srand(42);
	char* board = new char[xSize*ySize];
	char* write = board;

	int i = 0;
	for (unsigned iY = 0; iY < ySize; ++iY)
	{
		for (unsigned iX = 0; iX < xSize; ++iX)
		{
//			const int random = i % 25;
			const int random = rand() % 25;
			const char character = 'a' + random;
			*write++ = character;

			printf("%c", character);

			++i;
		}

		printf("\n");
	}
#else
	const unsigned xSize = 3;
	const unsigned ySize = 3;
	const char *board = "dzxeaiqut";
#endif

	printf("Finding in %ux%u...\n", xSize, ySize);
	Results results = FindWords(board, xSize, ySize);

	printf("Results:\n");
	printf("Count: %u Score: %u\n", results.Count, results.Score);

	for (unsigned iWord = 0; iWord < results.Count; ++iWord) printf("%s\n", results.Words[iWord]);	

	FreeWords(results);
	FreeDictionary();

	return 0;
}
