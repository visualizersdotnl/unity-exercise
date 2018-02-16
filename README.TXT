
** Out of date since I submitted solver_submitted.cpp, please check solver.cpp for up-to-date information and issues. **

Hi.

Here's my boggle solver, the entire solver lives in solver.cpp; please read as most details are explained
in (and on top of) that file.

* I've developed this on OSX (with clang as compiler), tested on Ubuntu and Windows (32-bit & 64-bit, VS2015).
  Makefile and VS2015 solution (stripped of all unnecessary files) for my test(s) are available.

* When using the test Windows solution (/VS2015) keep an eye on the command line parameters and working directory.

* The test accepts 2 parameters being the width and height of the board.

* It seems to be a little quicker on OSX and Linux, but given how the old slim MacBook I've developed this on runs
  Windows I can't say anything conclusive about this. It seems to perform well, though, and the execution time doesn't
  blow up.

* It should compile and run on any platform that adheres to the C++11 standard.

* Calls are threadsafe (the global dictionary is guarded by a mutex) and do not depend on any order.

* Leak testing with Valgrind on OSX reported no memory leaks, except a few bogus ones caused by the runtime library.

* Look for debug_print() on top of the solver to print some information on the go, if needed.


Cheers,
- Niels (ndewit@gmail.com)
