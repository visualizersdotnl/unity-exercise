
# Be sure to compile with -O3 for the best performance!

all:

#	Optimized.
	g++ -o boggle test.cpp solver.cpp -std=c++11 -O3

#	Debug.
#	g++ -o boggle test.cpp solver.cpp -std=c++11 -G
