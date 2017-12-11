
# Be sure to compile with -O3 for the best performance!

all:

#	Optimized.
	g++ -o boggle test.cpp solver.cpp -std=c++11 -O3

#	Debug (Valgrind).
#	g++ -o boggle test.cpp solver.cpp -std=c++11 -g
#	valgrind --leak-check=yes -v ./boggle 10 100
