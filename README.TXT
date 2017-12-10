
Hi!

Here's my boggle solver.

Most details are explaind both on top of and in the implementation (solver.cpp), so please read.

I've developed this on OSX (with clang as compiler), tested on Ubuntu and Windows (32-bit & 64-bit, VS2015).
When using the test Windows solution keep an eye on the command line parameters and working directory.

The test accepts 2 parameters being the width and height of the board.

It seems to be a little quicker on OSX and Linux, but given the slim, beaten up and old MacBook I've developed it on
there's really no telling other than that the speed seem pretty decent, and doesn't blow up exponentially.

- It should compile and run on any platform that adheres to the C++11 standard.
- Calls are threadsafe (the global dictionary is guarded by a mutex) and do not depend on any order.

Leak testing with Valgrind on OSX reported no memory leaks, except a few bogus ones caused by the runtime library.

And if I forgot to remove the .gitignore, please keep in mind that I used the private area of my account.

Cheers,
- Niels (ndewit@gmail.com)
