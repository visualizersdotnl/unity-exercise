
# Be sure to compile with -O3 for the best performance!

all:

#	Optimized.
#	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O3 -pg
	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O3 -march=native

#	Debug (Valgrind).
#	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O0 -g
#	valgrind --leak-check=yes -v ./boggle 10 100
