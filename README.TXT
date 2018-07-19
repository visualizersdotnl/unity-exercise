
Here's my boggle solver, the entire solver lives in solver.cpp; please read as most details are explained
in (and on top of) that file.

The version submitted for job application was solver_submitted.cpp, which, well, could've used some improvement.
And it got some!

* I've developed this on OSX (with clang as compiler), tested on Ubuntu and Windows (32-bit & 64-bit, VS2015).
  Makefile and VS2015 solution (stripped of all unnecessary files) for my test(s) are available.
  -- No longer up to date, only tested on OSX.

* When using the test Windows solution (/VS2015) keep an eye on the command line parameters and working directory.

* The test accepts 2 parameters being the width and height of the board.

* Performance is rather high, but still there are more than a few optimizations possible.
  It could obviously be faster if I'd forego on all safety measures I've taken as to not fail a test
  harness I don't know.

* It should compile and run on any platform that adheres to the C++11 standard.

* Calls are threadsafe (the global dictionary is guarded by a mutex) and do *not* depend on call order.

* Leak testing with Valgrind on OSX reported no memory leaks, except a few bogus ones caused by the runtime library.
  On Windows there's CRT heap debugging functions in the stub to test for leaks and break on allocation.

* Look for DEBUG_STATS on top, disable it for speed, enable it for information.
* Also look for NED_FLANDERS, if defined all safety measures mentioned are in effect.


Cheers,
- Niels (ndewit@gmail.com)
