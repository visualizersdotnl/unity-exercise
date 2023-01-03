
# Be sure to compile with -O3 for the best performance!

all:

#	Optimized.
#	g++ -o boggle test.cpp solver.cpp random.cpp tlsf/tlsf.c -std=c++11
	g++ -o boggle test.cpp solver.cpp random.cpp tlsf/tlsf.c -std=c++20 -O3 -DNDEBUG -fno-exceptions -lpthread -funroll-loops
	g++ -o boggle_sub test.cpp solver_submitted.cpp random.cpp -std=c++20 -O3 -fno-exceptions -lpthread -funroll-loops -DNDEBUG

#	Debug (Valgrind).
#	g++ -o boggle test.cpp solver.cpp random.cpp -std=c++11 -O0 -g
#	valgrind --leak-check=yes -v ./boggle 16 16