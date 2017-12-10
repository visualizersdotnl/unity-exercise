
Hi!

Here's my boggle solver; most details are explained on top of the solver implementation which resides in 'solver.cpp', please read.

I'll quickly go over the most important things here:

	* I developed and tested this solution with OSX (w/clang), be sure to take a look at the test and the Makefile.

	* Tested on OSX, Ubuntu & Windows (Visual Studio Community 2015).

	* It should compile with any modern standard-adhering C++ compiler; please turn optimization on ;)

	* Calls are threadsafe (the global dictionary is shielded by a mutex lock) and can be called in any order.

	* Testing with Valgrind reported no memory leaks (except a few bogus ones caused by the runtime library).


And if I forgot to remove the .gitignore, please keep in mind that I used the private area of my account.

Cheers,
- Niels (ndewit@gmail.com)
