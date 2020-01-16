/*
	Boggle solver implementation, written the weekend of December 9 & 10, 2017 by Niels J. de Wit (ndewit@gmail.com).
	Please check 'solver_submitted.cpp' for original information.

	This is an optimized version.

	- Always check for leaks (Windows debug build does it automatically).
	- FIXMEs.

	To do (low priority):
		- Check compile & run status on OSX.
		- Building (or loading) my dictionary is slow(ish), I'm fine with that as I focus on the solver.
		- Fix class members (notation).

	There's this idea floating that if you have a hash and eliminate a part of the dict. tree that way
	by special-casing the first 3 characters you're golden, but it did not really help at all plus it 
	makes the algorithm less flexible. What if someone decides to change the rules? ;)

	Notes:
		- Currently tested on Windows 10 (VS2017), Linux & OSX.
		- It's currently faster on a proper multi-core CPU than the Core M, probably due to those allocator locks.
		- Compile with full optimization (-O3 for ex.) for best performance.
		  Disabling C++ exceptions helps too, as they hinder inlining and are not used.
		  Please look at Albert's makefile for Linux/OSX optimal parameters!
		  And don't hesitate to play with them a little!
q		- I could not assume anything about the test harness, so I did not; if you want debug output check debug_print().
		  ** I violate this to tell if this was compiled with or without NED_FLANDERS (see below).
		- If LoadDictionary() fails, the current dictionary will be empty and FindWords() will simply yield zero results.
		- All these functions can be called at any time from any thread as the single shared resource, the dictionary,
		  is guarded by a mutex and no globals are used.
		- If an invalid board is supplied (anything non-alphanumerical detected) the query is skipped, yielding zero results.
		- My class design isn't really tight (functions and public member values galore), but for now that's fine.
		
		** Some of these stability claims only work if NED_FLANDERS (see below) is defined! **

	** 14/01/2020 **

	Fiddling around to make this faster; I have a few obvious things in mind; I won't be beautifying
	the code, so you're warned.

	- This is a non-Morton version that now handily outperforms the Morton version, so I deleted that one.
	- There's quite a bit of branching going on but trying to be smart doesn't always please the predictor/pipeline the best way, rather it's quite fast
	  in critical places because the predictor can do it's work very well (i.e. the cases lean 99% towards one side).
	- Just loosely using per-thread instances of CustomAlloc doesn't improve performance, I still think it could work because it enhances
	  locality and eliminates the need for those mutex locks.

	Threading issues (as seen in/by Superluminal):

	- Even distribution of words does not work well, but distributing them by letter is faster even though the thread loads are
	  uneven; it's a matter of how much traversal is done and the impact that has.
	- Using twice the amount of threads that would be sensible works way faster than bigger loads (once again, loads matter).
	- Currently there is code in place to use the "right" amount of threads and distribute words evenly but it is commented out.
	- I could add an option to sort the main dictionary, though currently I am loading a sorted version.

	Use profilers (MSVC & Superluminal) to really figure out what either works so well by accident or what could be fixed
	to use the "normal" amount of threads, evenly distributing the load. My gut says it's a matter of recursion.

	Thinking out loud: let's say I end up at the point of traversal with only one letter constantly being asked for (since the
	only child of the root node is 1 single letter), that probably means I ask for the same piece of memory again and again and
	cause less cache misses and less needless traversal.

	Conclusion for now: this model works because multiple smaller threads have a smaller footprint due to touching less memory,
	traversing less and thus are likely to be done much faster. The fact that some sleep more than others might just have to be
	taken for granted. That however does not mean that the memory allocation strategy for the tree must be altered by using sequential
	pools.

	** 16/01/2020 **

	I replaced DeepCopy for ThreadCopy, which keeps a sequential list of nodes and hands them out as needed, and releases the
	entire block of memory when the thread is done. I can see a speed increase without breaking consistent results.
*/

// Make VC++ 2015 shut up and walk in line.
#define _CRT_SECURE_NO_WARNINGS 

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <string>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <cassert>
	
#include "api.h"

#include "random.h"
#include "bit-tricks.h"
#include "simple-tlsf.h"
#include "inline.h"

// Undef. to skip dead end percentages and all prints and such.
// #define DEBUG_STATS

// Undef. to enable all the work I put in to please a, as it turns out, very forgiving test harness.
// But basically the only gaurantee here is that this works with my own test!
// #define NED_FLANDERS

// Undef. to use only 1 thread.
// #define SINGLE_THREAD

// Undef. to kill assertions.
// #define ASSERTIONS

#if defined(_DEBUG) || defined(ASSERTIONS)
	#define Assert assert
#else
	inline void Assert(bool condition) {}
#endif

#if defined(DEBUG_STATS)
	#define debug_print printf
#else
	inline void debug_print(const char* format, ...) {}
#endif

constexpr unsigned kAlphaRange = ('Z'-'A')+1;

#if defined(SINGLE_THREAD)
	constexpr size_t kNumThreads = 1;
#else
	const size_t kNumConcurrrency = std::thread::hardware_concurrency();
	
	// FIXME: this *would* be correct for a "normal" algorithms
//	const size_t kNumThreads = kNumConcurrrency-1;
	
	// FIXME: but *this*, more threads with lower loads, is faster!
	const size_t kNumThreads = kNumConcurrrency*2;
//	const size_t kNumThreads = std::max<size_t>(kAlphaRange-1, (kNumConcurrrency*2) - 1);
#endif

constexpr size_t kCacheLine = sizeof(size_t)*8;

// Number of words and number of nodes (1 for root) per thread.
class ThreadInfo
{
public:
	ThreadInfo() : 
		load(0), nodes(1) {}

	size_t load;
	size_t nodes;
};

static std::vector<ThreadInfo> s_threadInfo;

// If you see 'letter' and 'index' used: all it means is that an index is 0-based.
BOGGLE_INLINE unsigned LetterToIndex(char letter)
{
	return letter - 'A';
}

// FWD.
static void AddWordToDictionary(const std::string& word);
class DictionaryNode;

// A tree root per thread.
static std::vector<DictionaryNode*> s_threadDicts;

class DictionaryNode
{
	friend void AddWordToDictionary(const std::string& word);
	friend void FreeDictionary();

public:
	CUSTOM_NEW
	CUSTOM_DELETE

	DictionaryNode() : 
		m_wordIdx(-1)
,		m_indexBits(0)
	{
		memset(m_children, 0, sizeof(DictionaryNode*)*kAlphaRange);
	}

	DictionaryNode(size_t wordIdx, unsigned indexBits) :
		m_wordIdx(wordIdx)
,		m_indexBits(indexBits)
	{
		memset(m_children, 0, sizeof(DictionaryNode*)*kAlphaRange);
	}

	class ThreadCopy
	{
	public:
		CUSTOM_NEW
		CUSTOM_DELETE

		ThreadCopy(unsigned iThread) : 
			m_iThread(iThread), m_root(nullptr), m_iAlloc(0)
		{
			// Allocate pool for all necessary nodes.
			const auto numNodes = s_threadInfo[iThread].nodes;
			const auto size = numNodes*sizeof(DictionaryNode);
			m_pool = static_cast<DictionaryNode*>(s_customAlloc.Allocate(size, 16));

			// Recursively copy them.
			m_root = Copy(s_threadDicts[iThread]);						
		}

		~ThreadCopy()
		{
			s_customAlloc.Free(m_root);
		}

		DictionaryNode* GetRoot() /* const */
		{
			return m_root;
		}

	private:
		DictionaryNode* Copy(DictionaryNode* parent)
		{
			DictionaryNode& node = m_pool[m_iAlloc++];
			Assert(m_iAlloc < s_threadInfo[m_iThread].nodes);

			node.m_wordIdx   = parent->m_wordIdx;
			node.m_indexBits = parent->m_indexBits;
			memset(node.m_children, 0, kAlphaRange*sizeof(DictionaryNode*));

			unsigned indexBits = node.m_indexBits;
			unsigned index = 0;
			while (indexBits)
			{
				if (indexBits & 1)
					node.m_children[index] = Copy(parent->GetChild(index));

				indexBits >>= 1;
				++index;
			}

			return &node;
		}

		const unsigned m_iThread;

		DictionaryNode* m_pool;
		size_t m_iAlloc;

		DictionaryNode* m_root;
	};

	// Destructor is not called when using ThreadCopy!
	~DictionaryNode()
	{
		for (unsigned iChar = 0; iChar < kAlphaRange; ++iChar)
			delete m_children[iChar];
	}

private:
	// Only called from LoadDictionary().
	DictionaryNode* AddChild(char letter, unsigned iThread)
	{
		const unsigned index = LetterToIndex(letter);

		const unsigned bit = 1 << index;
		if (m_indexBits & bit)
		{
			return m_children[index];
		}

		++s_threadInfo[iThread].nodes;
			
		m_indexBits |= bit;
		return m_children[index] = new DictionaryNode();
	}

public:
	BOGGLE_INLINE unsigned HasChildren() const { return m_indexBits;                    } // Non-zero.
	BOGGLE_INLINE bool     IsWord()      const { return -1 != m_wordIdx;                }
	BOGGLE_INLINE int      IsVoid()      const { return int(m_indexBits+m_wordIdx)>>31; } // Is 1 (sign bit) if zero children (0) and no word (-1).

	// Returns zero if node is now a dead end.
	BOGGLE_INLINE unsigned RemoveChild(unsigned index)
	{
		Assert(index < kAlphaRange);
		Assert(HasChild(index));

		const unsigned bit = 1 << index;
		m_indexBits ^= bit;

		return m_indexBits;
	}

	// Returns non-zero if true.
	BOGGLE_INLINE unsigned HasChild(unsigned index)
	{
		Assert(index < kAlphaRange);
		const unsigned bit = 1 << index;
		return m_indexBits & bit;
	}

	BOGGLE_INLINE DictionaryNode* GetChild(unsigned index)
	{
		Assert(HasChild(index));
		return m_children[index];
	}

	// Returns index and wipes it (eliminating need to do so yourself whilst not changing a negative outcome)
	BOGGLE_INLINE size_t GetWordIndex() /*  const */
	{
		const auto index = m_wordIdx;
		m_wordIdx = -1;
		return index;
	}

private:
	size_t m_wordIdx;
	unsigned m_indexBits;
	DictionaryNode* m_children[kAlphaRange];
};

// We keep one dictionary at a time so it's access is protected by a mutex, just to be safe.
static std::mutex s_dictMutex;

// Sequential dictionary of all full words (FIXME: might be unsorted, depends on dictionary loaded).
static std::vector<std::string> s_dictionary;

// Counters, the latter being useful to reserve space.
static unsigned s_longestWord;
static size_t s_wordCount;

#ifdef NED_FLANDERS
// Scoped lock for all dictionary globals.
class DictionaryLock
{
public:
	DictionaryLock() :
		m_lock(s_dictMutex) {}

private:
	std::lock_guard<std::mutex> m_lock;
};
#else
class DictionaryLock {};
#endif // NED_FLANDERS

// Tells us if a word adheres to the rules.
static bool IsWordValid(const std::string& word)
{
	const size_t length = word.length();

	// Word not too short?
	if (length < 3)
	{
		debug_print("Invalid word because it's got less than 3 letters: %s\n", word.c_str());
		return false;
	}

	// Check if it violates the 'Qu' rule.
	auto iQ = word.find_first_of('Q');
	while (std::string::npos != iQ)
	{
		auto next = iQ+1;
		if (next == length || word[next] != 'U')
		{
			debug_print("Invalid word due to 'Qu' rule: %s\n", word.c_str());
			return false;
		}

		iQ = word.substr(next).find_first_of('Q');
	}

	return true;
}

// Input word must be uppercase!
static void AddWordToDictionary(const std::string& word)
{
	// Word of any use given the Boggle rules?	
	if (false == IsWordValid(word))
		return;
	
	const unsigned length = unsigned(word.length());
	if (length > s_longestWord)
	{
		s_longestWord = length;
	}

	// FIXME: this distributes by letter, which does a better job at containing the amount of traversal
	char letter = word[0];
	if ('Q' == letter)
		letter = (mt_rand32() & 1) ? 'Q' : 'U';

	const auto iThread = LetterToIndex(letter)%kNumThreads;
	
	// This distributes evenly, but makes things dreadfully slow (see above)
//	unsigned iThread = s_wordCount%kNumThreads;

	DictionaryNode* node = s_threadDicts[iThread];

	for (auto iLetter = word.begin(); iLetter != word.end(); ++iLetter)
	{
		const char letter = *iLetter;

		// Get or create child node.
		node = node->AddChild(letter, iThread);

		// Handle 'Qu' rule.
		if ('Q' == letter)
		{
			// Verified to be 'Qu' by IsWordValid().
			// Skip over 'U'.
			++iLetter;
		}
	}

	// Store.
	s_dictionary.emplace_back(word);
	node->m_wordIdx = s_wordCount;

	++s_threadInfo[iThread].load;
	++s_wordCount;
}

void LoadDictionary(const char* path)
{
	// If the dictionary fails to load, you'll be left with an empty dictionary.
	FreeDictionary();
	
	if (nullptr == path)
		return;

	FILE* file = fopen(path, "r");
	if (nullptr == file)
	{
		debug_print("Can not open dictionary for read access at: %s\n", path);
		return;
	}

	DictionaryLock lock;
	{
		size_t iThread = kNumThreads;
		while (iThread-- > 0)
			s_threadDicts.emplace_back(new DictionaryNode());

		int character;
		std::string word;

		do
		{
			character = fgetc(file);
			if (0 != isalpha((unsigned char) character))
			{
				// Boggle tiles are simply A-Z, where Q means 'Qu'.
				word += toupper(character);
			}
			else
			{
				// We've hit EOF or a non-alphanumeric character.
				if (false == word.empty()) // Got a word?
				{
					AddWordToDictionary(word);
					word.clear();
				}
			}
		}
		while (EOF != character);

		fclose(file);

#ifdef NED_FLANDERS		
		// Check thread load total.
		size_t count = 0;
		for (auto& info : s_threadInfo)
		{
			count += info.load;
		}

		if (count != s_wordCount)
			debug_print("Thread word count %zu != total word count %zu!", count, s_wordCount);
#endif
	}

	debug_print("Dictionary loaded. %zu words, longest being %u characters\n", s_wordCount, s_longestWord);
}

void FreeDictionary()
{
	DictionaryLock lock;
	{
		// Delete roots;
		for (auto* root : s_threadDicts) 
			delete root;

		s_threadDicts.clear();

		// Reset thread information.		
		s_threadInfo.resize(kNumThreads, ThreadInfo());

		// Reset counters.
		s_longestWord = 0;
		s_wordCount = 0;
	}
}

// This class contains the actual solver and it's entire context, including a local copy of the dictionary.
// This means that there will be no problem reloading the dictionary whilst solving, nor will concurrent FindWords()
// calls cause any fuzz due to globals and such.

#include <emmintrin.h>

class Query
{
public:
	CUSTOM_NEW
	CUSTOM_DELETE

	Query(Results& results, const char* sanitized, unsigned width, unsigned height) :
		m_results(results)
,		m_sanitized(sanitized)
,		m_width(width)
,		m_height(height)
,		m_gridSize(width*height) 
	{
	}

	~Query() {}

public:
	// FIXME: slim this down, fix notation! 
	class ThreadContext
	{
	public:
		CUSTOM_NEW
		CUSTOM_DELETE

		ThreadContext(unsigned iThread, const Query* instance) :
		iThread(iThread)
,		gridSize(instance->m_gridSize)
,		sanitized(instance->m_sanitized)
,		width(instance->m_width)
,		height(instance->m_height)
,		visited(nullptr)
,		score(0)
,		reqStrBufLen(0)
		{
			Assert(nullptr != instance);
		}

		~ThreadContext()
		{
			s_customAlloc.Free(visited);
		}

		void OnExecuteThread()
		{
			// Handle allocation and initialization of memory.

			visited = static_cast<bool*>(s_customAlloc.Allocate(gridSize*sizeof(bool), kCacheLine));

			if (gridSize >= 32)
			{
				// This has proven to be a little faster than memset().
				size_t numStreams = gridSize*sizeof(bool) / sizeof(int);
				int* pWrite = reinterpret_cast<int*>(visited);
				while (numStreams--)
					_mm_stream_si32(pWrite++, 0);
			}
			else
				memset(visited, 0, gridSize*sizeof(bool));

			// If a thread finds 50% it's pretty good.
			wordsFound.reserve(s_threadInfo[iThread].load >> 1); 
		}

		// Input
		const unsigned iThread;
		const size_t gridSize;
		const char* sanitized;
		const unsigned width, height;
		
		// Grid to flag visited tiles.
		bool* visited;

		// Output
		std::vector<size_t> wordsFound;
		unsigned score;
		size_t reqStrBufLen;

#if defined(DEBUG_STATS)
		unsigned maxDepth;
#endif
	}; 

public:
	void Execute()
	{
		// Just in case another Execute() call is made on the same context: avoid leaking.
		FreeWords(m_results);

		// Bit of a step back from what it was, but as I'm picking words out of the global list now..
		DictionaryLock dictLock;
		{
			// Kick off threads.

			std::vector<std::thread> threads;
			std::vector<std::unique_ptr<ThreadContext>> contexts;

			debug_print("Kicking off %zu threads.\n", kNumThreads);
			
			for (unsigned iThread = 0; iThread < kNumThreads; ++iThread)
			{
				contexts.emplace_back(std::unique_ptr<ThreadContext>(new ThreadContext(iThread, this)));
				threads.emplace_back(std::thread(ExecuteThread, contexts[iThread].get()));
			}
			
			for (auto& thread : threads)
				thread.join();

			m_results.Count  = 0;
			m_results.Score  = 0;
			size_t strBufLen = 0;

			for (auto& context : contexts)
			{
				const unsigned numWords = (unsigned) context->wordsFound.size();
				const unsigned score = context->score;
				m_results.Count += numWords;
				m_results.Score += score;
				strBufLen += context->reqStrBufLen + numWords; // Add numWords for 0-string-terminator for each.

				debug_print("Thread %u joined with %u words (scoring %u).\n", context->iThread, numWords, score);
			}

			m_results.Words = new char*[m_results.Count];

#ifdef NED_FLANDERS
			// Copy words to Results structure.
			// I'd rather set pointers into the dictionary, but that would break the results as soon as new dictionary is loaded.

			char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.
			char* resBuf = new char[strBufLen]; // Allocate sequential buffer.

			for (auto& context : contexts)
			{
				for (auto wordIdx : context->wordsFound)
				{
					*words_cstr = resBuf;
					auto& word = s_dictionary[wordIdx];
					strcpy(*words_cstr++, word.c_str());
					const size_t length = word.length();
					resBuf += length+1;
				}
			}
		}
#else
			// Copy words to Results structure.
			// The dirty way: set pointers.

			char** words_cstr = const_cast<char**>(m_results.Words); // After all I own this data.

			for (auto& context : contexts)
			{
				for (auto wordIdx : context->wordsFound)
				{
					*words_cstr++ = const_cast<char*>(s_dictionary[wordIdx].c_str());
				}
			}
		}
#endif
	}

private:
	static void ExecuteThread(ThreadContext* context);

private:
	BOGGLE_INLINE static unsigned GetWordScore(size_t length) /* const */
	{
		const unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
		if (length > 8) length = 8;
		return kLUT[length-3];
	}

#if defined(DEBUG_STATS)
	static void BOGGLE_INLINE TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node, unsigned& depth);
	static void BOGGLE_INLINE TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node, unsigned& depth);
#else
	static void BOGGLE_INLINE TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node);
	static void BOGGLE_INLINE TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node);
#endif

	Results& m_results;
	const char* m_sanitized;
	const unsigned m_width, m_height;
	const size_t m_gridSize;
};

/* static */ void Query::ExecuteThread(ThreadContext* context)
{
	context->OnExecuteThread();

	const unsigned iThread = context->iThread;
	const auto* sanitized = context->sanitized;

	auto* visited = context->visited;
	const unsigned gridSize = context->gridSize;

	// Create copy of source dictionary tree
	auto threadCopy = DictionaryNode::ThreadCopy(iThread);
	DictionaryNode* root = threadCopy.GetRoot();
			
	const unsigned width  = context->width;
	const unsigned height = context->height;

#if defined(DEBUG_STATS)
	debug_print("Thread %u has a load of %zu words and %zu nodes.\n", iThread, s_threadInfo[iThread].load, s_threadInfo[iThread].nodes);

	context->maxDepth = 0;
#endif
	
	size_t boardIdx = 0;
	for (unsigned iY = 0; iY < height; ++iY)
	{
		for (unsigned iX = 0; iX < width; ++iX)
		{
			const unsigned index = sanitized[boardIdx++];

			// We know, for now, that the root won't be removed, so we can just get the child node pointer
			// and see if it's NULL instead of calling 2 functions to get to the same conclusion.
//			if (root->HasChild(index))
			{
				DictionaryNode* child = root->GetChild(index);
				if (nullptr != child)
				{
#if defined(DEBUG_STATS)
				unsigned depth = 0;
				TraverseBoard(*context, iX, iY, child, depth);
#else
				TraverseBoard(*context, iX, iY, child);
#endif
				}
			}
		}
		
		// Yielding at this point saves time, but is it the best place? (FIXME)
		std::this_thread::yield();
	}

	auto& wordsFound = context->wordsFound;
	std::sort(wordsFound.begin(), wordsFound.end());

	// Tally up the score and required buffer length.
	for (auto wordIdx : wordsFound)
	{
		const size_t length = s_dictionary[wordIdx].length();
		context->score += GetWordScore(length);
		context->reqStrBufLen += length;
	}
		
#if defined(DEBUG_STATS)
	const float missesPct = ((float)wordsFound.size()/s_threadInfo[iThread].load)*100.f;
	debug_print("Thread %u has max. traversal depth %u (max. %u), misses: %.2f percent of load.\n", iThread, context->maxDepth, s_longestWord, missesPct);
#endif
}

#if defined(DEBUG_STATS)
/* static */ BOGGLE_INLINE void Query::TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node, unsigned& depth)
#else
/* static */ BOGGLE_INLINE void Query::TraverseCall(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode *node)
#endif
{
	const auto width = context.width;
	const unsigned nbBoardIdx = iY*width + iX;

	auto* board = context.sanitized;
	const unsigned nbIndex = board[nbBoardIdx];

	if (0 != node->HasChild(nbIndex))
	{
		auto* child = node->GetChild(nbIndex);
#if defined(DEBUG_STATS)
		TraverseBoard(context, iX, iY, child, depth);
#else
		TraverseBoard(context, iX, iY, child);
#endif

		// Child node exhausted?
		if (child->IsVoid())
		{
			node->RemoveChild(nbIndex);
		}
	}
}

#if defined(DEBUG_STATS)
/* static */ void BOGGLE_INLINE Query::TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node, unsigned& depth)
#else
/* static */ void BOGGLE_INLINE Query::TraverseBoard(ThreadContext& context, unsigned iX, unsigned iY, DictionaryNode* node)
#endif
{
	Assert(nullptr != node);

	const auto width = context.width;
	const auto height = context.height;

	const unsigned boardIdx = iY*width + iX;

	auto* visited = context.visited;
	if (true == visited[boardIdx])
		return;

#if defined(DEBUG_STATS)
	Assert(depth < s_longestWord);
	context.maxDepth = std::max(context.maxDepth, depth);
	++depth;
#endif

	// Recurse, as we've got a node that might be going somewhewre.
	
	visited[boardIdx] = true;

	const bool xSafe = iX < width-1;

#if defined(DEBUG_STATS)
	if (iY < height-1)
	{
		TraverseCall(context, iX, iY+1, node, depth);
		
		if (xSafe) 
			TraverseCall(context, iX+1, iY+1, node, depth);
		if (iX > 0)
			TraverseCall(context, iX-1, iY+1, node, depth);
	}

	if (iY > 0)
	{
		TraverseCall(context, iX, iY-1, node, depth);
		
		if (xSafe)
			TraverseCall(context, iX+1, iY-1, node, depth);
		if (iX > 0) 
			TraverseCall(context, iX-1, iY-1, node, depth);
	}

	if (iX > 0)
		TraverseCall(context, iX-1, iY, node, depth);

	if (xSafe)
		TraverseCall(context, iX+1, iY, node, depth);

		--depth;
#else
	// This has been ordered specifically to be as cache friendly as possible,
	// plus due to the enormous advantage that the branching goes 1 way (everywhere but on the edges)
	// the predictor does it's job and the branches aren't expensive at all.

	if (iY < height-1)
	{
		TraverseCall(context, iX, iY+1, node);

		if (xSafe) 
			TraverseCall(context, iX+1, iY+1, node);
		if (iX > 0)
			TraverseCall(context, iX-1, iY+1, node);
	}

	if (iY > 0) {
		TraverseCall(context, iX, iY-1, node);

		if (xSafe)
			TraverseCall(context, iX+1, iY-1, node);
		if (iX > 0) 
			TraverseCall(context, iX-1, iY-1, node);
	}

	if (iX > 0)
		TraverseCall(context, iX-1, iY, node);

	if (xSafe)
		TraverseCall(context, iX+1, iY, node);
#endif
		
	visited[boardIdx] = false;

	// It's faster to do this *after* traversal; I haven't looked at why exactly that is, might have to do with the
	// branch predictor again.
	const size_t wordIdx = node->GetWordIndex();
	if (-1 != wordIdx)
		context.wordsFound.emplace_back(wordIdx);
}

Results FindWords(const char* board, unsigned width, unsigned height)
{
	debug_print("Using debug prints, takes a little off the performance.\n");

#if !defined(NED_FLANDERS)
	static bool warned = false;
	if (false == warned)
	{
		printf("Built without the NED_FLANDERS define, so the safety measures are largely off.\n");
		warned = true;
	}
#endif

	const size_t nodeSize = sizeof(DictionaryNode);
	debug_print("Node size: %zu\n", nodeSize);
	
	Results results;
	results.Words = nullptr;
	results.Count = 0;
	results.Score = 0;
	results.UserData = nullptr; // Didn't need it in this implementation.

	// Board parameters check out?
	if (nullptr != board && !(0 == width || 0 == height))
	{
		const unsigned gridSize = width*height;
		char* sanitized = static_cast<char*>(s_customAlloc.AllocateUnsafe(gridSize*sizeof(char), kCacheLine));

#ifdef NED_FLANDERS
		// Sanitize that checks for illegal input and uppercases.
		size_t index = 0;
		for (unsigned iY = 0; iY < height; ++iY)
		{
			for (unsigned iX = 0; iX < width; ++iX)
			{
				// FIXME: does not check for 'u'!
				const char letter = board[index];
				if (0 != isalpha((unsigned char) letter))
				{
					const unsigned sanity = LetterToIndex(toupper(letter));
					sanitized[index] = sanity;
				}
				else
				{
					// Invalid character: skip query.
					return results;
				}

				++index;
			}
		}
#else
		// Sanitize that just reorders and expects uppercase.
		for (unsigned index = 0; index < gridSize; ++index)
		{
			const char letter = *board++;
			const unsigned sanity = LetterToIndex(letter); // LetterToIndex(toupper(letter));
			sanitized[index] = sanity;
		}
#endif

		Query query(results, (nullptr == sanitized) ? board : sanitized, width, height);
		query.Execute();

		s_customAlloc.Free(sanitized);
	}

	return results;
}

void FreeWords(Results results)
{
#ifdef NED_FLANDERS
	if (0 != results.Count && nullptr != results.Words)
	{
		// Allocated a single buffer.
		delete[] results.Words[0];
	}
#endif

	delete[] results.Words;
	results.Words = nullptr;

	results.Count = results.Score = 0;
	results.UserData = nullptr;
}
