
# Be sure to compile with -O3 for the best performance!

all:

#	Optimized.
#	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O3 
	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O3 -march=native -DNDEBUG -fno-exceptions
#	g++ -o boggle_sub test.cpp solver_submitted.cpp random.cpp -std=c++11 -O3 -march=native -DNDEBUG

#	Debug (Valgrind).
#	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O0 -g
#	valgrind --leak-check=yes -v ./boggle 16 16