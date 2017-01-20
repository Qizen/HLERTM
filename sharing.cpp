//
// sharing.cpp
//
// Copyright (C) 2013 - 2016 jones@scss.tcd.ie
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software Foundation;
// either version 2 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
// 19/11/12 first version
// 19/11/12 works with Win32 and x64
// 21/11/12 works with Character Set: Not Set, Unicode Character Set or Multi-Byte Character
// 21/11/12 output results so they can be easily pasted into a spreadsheet from console
// 24/12/12 increment using (0) non atomic increment (1) InterlockedIncrement64 (2) InterlockedCompareExchange
// 12/07/13 increment using (3) RTM (restricted transactional memory)
// 18/07/13 added performance counters
// 27/08/13 choice of 32 or 64 bit counters (32 bit can oveflow if run time longer than a couple of seconds)
// 28/08/13 extended struct Result
// 16/09/13 linux support (needs g++ 4.8 or later)
// 21/09/13 added getWallClockMS()
// 12/10/13 Visual Studio 2013 RC
// 12/10/13 added FALSESHARING
// 14/10/14 added USEPMS
//

//
// NB: hints for pasting from console window
// NB: Edit -> Select All followed by Edit -> Copy
// NB: paste into Excel using paste "Use Text Import Wizard" option and select "/" as the delimiter
//

#include "stdafx.h"                             // pre-compiled headers
#include <iostream>                             // cout
#include <iomanip>                              // setprecision
#include <mutex>
#include "helper.h"                             //
#include "sharing.h"
#include <vector>

using namespace std;                            // cout

#define K           1024                        //
#define GB          (K*K*K)                     //
#define NOPS        10000                       //
#define NSECONDS    2                           // run each test for NSECONDS

#define COUNTER64                               // comment for 32 bit counter
												//#define FALSESHARING                          // allocate counters in same cache line
												//#define USEPMS                                // use PMS counters

#ifdef COUNTER64
#define VINT    UINT64                          //  64 bit counter
#else
#define VINT    UINT                            //  32 bit counter
#endif

UINT64 tstart;                                  // start of test in ms
int sharing;                                    // % sharing
int lineSz;                                     // cache line size
int maxThread;                                  // max # of threads

THREADH *threadH;                               // thread handles
UINT64 *ops;                                    // for ops per thread



#ifdef FALSESHARING
#define GINDX(n)    (g+n)                       //
#else
#define GINDX(n)    (g+n*lineSz/sizeof(VINT))   //
#endif

												//
												// OPTYP
												//
												// 0:inc
												// 1:InterlockedIncrement
												// 2:InterlockedCompareExchange
												// 3:RTM (restricted transactional memory)
												// 4:Atomic Increment
												// 5:BakeryLock
												//

#define OPTYP       7                     // set op type

#if OPTYP == 0

#define OPSTR       "inc"
#define INC(g)      (*g)++;

#elif OPTYP == 1

#ifdef COUNTER64
#define OPSTR       "InterlockedIncrement64"
#define INC(g)      InterlockedIncrement64((volatile LONG64*) g)
#else
#define OPSTR       "InterlockedIncrement"
#define INC(g)      InterlockedIncrement(g)
#endif

#elif OPTYP == 2

#ifdef COUNTER64
#define OPSTR       "InterlockedCompareExchange64"
#define INC(g)      do {                                                                        \
                        x = *g;                                                                 \
                    } while (InterlockedCompareExchange64((volatile LONG64*) g, x+1, x) != x);
#else
#define OPSTR       "InterlockedCompareExchange"
#define INC(g)      do {                                                                        \
                        x = *g;                                                                 \
                    } while (InterlockedCompareExchange(g, x+1, x) != x);
#endif

#elif OPTYP == 3

#define OPSTR       "RTM (restricted transactional memory)"
#define INC(g)      {                                                                           \
                        UINT status = _xbegin();                                                \
                        if (status == _XBEGIN_STARTED) {                                        \
                            (*g)++;                                                             \
                            _xend();                                                            \
                        } else {                                                                \
                            nabort++;                                                           \
                            InterlockedIncrement64((volatile LONG64*)g);                        \
                        }                                                                       \
                    }

//Bakery Lock
#elif OPTYP == 5
volatile LONGLONG number[8];	
volatile LONGLONG choosing[8];
#define INIT(x)		{}
#define ACQUIRE(pid)	{														\
							/*pid is thread ID*/								\
							choosing[pid] = 1;									\
							int	maxTick = 0;									\
							for (int i = 0; i < 8; i++)							\
							{													\
								/* find maximum ticket */						\
								if (number[i] > maxTick)						\
								{												\
									maxTick = number[i];						\
								}												\
							}													\
							number[pid] = maxTick + 1;								\
							/* our ticket number is maximum ticket found + 1 */	\
							choosing[pid] = 0;									\
							for (int j = 0; j < 8; j++)							\
							{													\
								/* wait until our turn i.e. have lowest ticket*/\
								while (choosing[j]) Sleep(0);					\
								while (number[j] && ((number[j] < number[pid]) || ((number[j] == number[pid]) && (j < pid)))) Sleep(0);		\
							}													\
						}													



#define RELEASE(pid)	{														\
							number[pid] = 0;									\
						}
#define OPSTR       "Bakery Lock"
#define INC(g)      {															\
						ACQUIRE(thread);											\
						_mm_mfence();											\
						(*g)++;													\
						RELEASE(thread);											\
						_mm_lfence();											\
					}
				
//TestAndTestAndSetLock
#elif OPTYP == 6
#define OPSTR "TestAndTestAndSet Lock"
volatile LONGLONG tatasLock = 0;
#define ACQUIRE() {															\
					do{														\
						while(tatasLock == 1)									\
							Sleep(0);									\
					} while (InterlockedExchange64(&tatasLock, 1));				\
				}
#define RELEASE()	{														\
						tatasLock=0;												\
					}
#define INC(g)	{															\
					ACQUIRE();												\
					(*g)++;													\
					RELEASE();												\
				}

#elif OPTYP == 7
#define OPSTR "MCS Lock"
class QNode : public ALIGNEDMA
{
public:
	volatile int waiting;
	volatile QNode* next;
};
QNode* mcsLock;
DWORD tlsIndex = TlsAlloc();
#define ACQUIRE(lock)	{															\
							volatile QNode* qn = (QNode*) TlsGetValue(tlsIndex);	\
							qn->next = NULL;										\
							volatile QNode* pred = (QNode*) InterlockedExchangePointer((PVOID*)lock, (PVOID)qn); \
							if(pred != NULL)										\
							{														\
								qn->waiting = 1;									\
								pred->next = qn;									\
								while(qn->waiting) Sleep(0);						\
							}														\
						}
#define RELEASE(lock)	{															\
							volatile QNode* qn = (QNode*) TlsGetValue(tlsIndex);	\
							volatile QNode* succ;									\
							bool foo = false;										\
							if(!(succ = qn->next))									\
							{														\
								if(InterlockedCompareExchangePointer((PVOID*) lock, NULL, (PVOID) qn) != qn)\
								{													\
									while((succ = qn->next) == NULL) Sleep(0);	\
								}													\
								else foo = true;									\
							}														\
							if(!foo) succ->waiting = 0;								\
						}
#define INC(g)	{																	\
					ACQUIRE(&mcsLock);												\
					(*g)++;															\
					RELEASE(&mcsLock);												\
				}

#endif

#if OPTYP == 3
UINT64 *aborts;                                 // for counting aborts
#endif

typedef struct {
	int sharing;                                // sharing
	int nt;                                     // # threads
	UINT64 rt;                                  // run time (ms)
	UINT64 ops;                                 // ops
	UINT64 incs;                                // should be equal ops
	UINT64 aborts;                              //
} Result;

Result *r;                                      // results
UINT indx;                                      // results index

volatile VINT *g;                               // NB: position of volatile

												//
												// test memory allocation [see lecture notes]
												//
ALIGN(64) UINT64 cnt0;
ALIGN(64) UINT64 cnt1;
ALIGN(64) UINT64 cnt2;
UINT64 cnt3;                                    // NB: in Debug mode allocated in cache line occupied by cnt0

												//
												// PMS
												//
#ifdef USEPMS

UINT64 *fixedCtr0;                              // fixed counter 0 counts
UINT64 *fixedCtr1;                              // fixed counter 1 counts
UINT64 *fixedCtr2;                              // fixed counter 2 counts
UINT64 *pmc0;                                   // performance counter 0 counts
UINT64 *pmc1;                                   // performance counter 1 counts
UINT64 *pmc2;                                   // performance counter 2 counts
UINT64 *pmc3;                                   // performance counter 2 counts

												//
												// zeroCounters
												//
void zeroCounters()
{
	for (UINT i = 0; i < ncpu; i++) {
		for (int j = 0; j < 4; j++) {
			if (j < 3)
				writeFIXED_CTR(i, j, 0);
			writePMC(i, j, 0);
		}
	}
}

//
// void setupCounters()
//
void setupCounters()
{
	if (!openPMS())
		quit();

	//
	// enable FIXED counters
	//
	for (UINT i = 0; i < ncpu; i++) {
		writeFIXED_CTR_CTRL(i, (FIXED_CTR_RING123 << 8) | (FIXED_CTR_RING123 << 4) | FIXED_CTR_RING123);
		writePERF_GLOBAL_CTRL(i, (0x07ULL << 32) | 0x0f);
	}

#if OPTYP == 3

	//
	// set up and enable general purpose counters
	//
	for (UINT i = 0; i < ncpu; i++) {
		writePERFEVTSEL(i, 0, PERFEVTSEL_EN | PERFEVTSEL_USR | RTM_RETIRED_START);
		writePERFEVTSEL(i, 1, PERFEVTSEL_EN | PERFEVTSEL_USR | RTM_RETIRED_COMMIT);
		writePERFEVTSEL(i, 2, PERFEVTSEL_IN_TXCP | PERFEVTSEL_IN_TX | PERFEVTSEL_EN | PERFEVTSEL_USR | CPU_CLK_UNHALTED_THREAD_P);  // NB: TXCP in PMC2 ONLY
		writePERFEVTSEL(i, 3, PERFEVTSEL_IN_TX | PERFEVTSEL_EN | PERFEVTSEL_USR | CPU_CLK_UNHALTED_THREAD_P);
	}

#endif

}

//
// void saveCounters()
//
void saveCounters()
{
	for (UINT i = 0; i < ncpu; i++) {
		fixedCtr0[indx*ncpu + i] = readFIXED_CTR(i, 0);
		fixedCtr1[indx*ncpu + i] = readFIXED_CTR(i, 1);
		fixedCtr2[indx*ncpu + i] = readFIXED_CTR(i, 2);
		pmc0[indx*ncpu + i] = readPMC(i, 0);
		pmc1[indx*ncpu + i] = readPMC(i, 1);
		pmc2[indx*ncpu + i] = readPMC(i, 2);
		pmc3[indx*ncpu + i] = readPMC(i, 3);
	}
}

#endif

//
// worker
//
WORKER worker(void *vthread)
{
	int thread = (int)((size_t)vthread);

	UINT64 n = 0;

	volatile VINT *gt = GINDX(thread);
	volatile VINT *gs = GINDX(maxThread);

#if OPTYP == 2
	VINT x;
#elif OPTYP == 3
	UINT64 nabort = 0;
#endif

	runThreadOnCPU(thread % ncpu);

	while (1) {

		//
		// do some work
		//
		for (int i = 0; i < NOPS / 4; i++) {

			switch (sharing) {
			case 0:

				INC(gt);
				INC(gt);
				INC(gt);
				INC(gt);
				break;

			case 25:
				INC(gt);
				INC(gt);
				INC(gt);
				INC(gs);
				break;

			case 50:
				INC(gt);
				INC(gs);
				INC(gt);
				INC(gs);
				break;

			case 75:
				INC(gt);
				INC(gs);
				INC(gs);
				INC(gs);
				break;

			case 100:
				INC(gs);
				INC(gs);
				INC(gs);
				INC(gs);

			}
		}
		n += NOPS;

		//
		// check if runtime exceeded
		//
		if ((getWallClockMS() - tstart) > NSECONDS * 1000)
			break;

	}

	ops[thread] = n;
#if OPTYP == 3
	aborts[thread] = nabort;
#endif
	return 0;

}

WORKER lockWorker(void* vthread)
{
	int thread = (int)((size_t)vthread);

	UINT64 n = 0;

	volatile VINT *gs = GINDX(maxThread);

#if OPTYP == 7
	QNode* qn = new QNode();
	TlsSetValue(tlsIndex, qn);
#endif

	runThreadOnCPU(thread % ncpu);

	while (1) 
	{
		// do some work		
		for (int i = 0; i < NOPS; i++) 
		{
			INC(gs);
		}
		n += NOPS;

		// check if runtime exceeded
		if ((getWallClockMS() - tstart) > NSECONDS * 1000)
			break;
	}

	ops[thread] = n;
	return 0;
}

//
//	ALIGNEDMA stuff
//
void* ALIGNEDMA::operator new(size_t sz)
{
	sz = (sz + lineSz - 1) / lineSz * lineSz;
	return _aligned_malloc(sz, lineSz);
}

void ALIGNEDMA::operator delete(void* p)
{
	_aligned_free(p);
}

//
//	Tut3 BST Stuff
//

BST::BST()
{
	root = nullptr;
}

int BST::contains(INT64 key)
{
	return NULL;
}

int BST::add(Node* n)
{
	Node* volatile* pp = &root;
	// should this also be volatile?
	Node* p = root;
	while (p)
	{
		if (n->key < p->key)
		{
			pp = &(p->left);
		}
		else if (n->key > p->key)
		{
			pp = &p->right;
		}
		else
		{
			return 0;
		}
		p = *pp;
	}
	*pp = n;
	return 1;
}

Node* BST::remove(INT64 key)
{
	Node* volatile* pp = &root;
	//should this also be volatile?
	Node* p = root;
	while (p)
	{
		if (key < p->key)
		{
			pp = &p->left;
		}
		else if (key > p->key)
		{
			pp = &p->right;
		}
		else
		{
			break;
		}
		p = *pp;
	}

	if (p == nullptr)
	{
		return nullptr;
	}

	if (p->left == nullptr && p->right == nullptr)
	{
		*pp = nullptr;
	}
	else if (p->left == nullptr)
	{
		*pp = p->right;
	}
	else if (p->right == nullptr)
	{
		*pp = p->left;
	}
	else
	{
		//volatility?
		Node* r = p->right;
		Node *volatile* ppr = &p->right;
		while (r->left)
		{
			ppr = &r->left;
			r = r->left;
		}
		p->key = r->key;
		p = r;
		*ppr = r->right;
	}
	return p;
}

//TODO: put this at the top with the rest of the defines
#define NUM_ITER 1000000
#define NUM_PRE 1000
#define EXP_VALS 20

volatile long TATASlock = 0;

//
// main
//
int main()
{
	// initialise
	lineSz = getCacheLineSz();
	threadH = (THREADH*)ALIGNED_MALLOC(maxThread*sizeof(THREADH), lineSz);

	for (int nt = 1; nt <= 4; nt *= 2)
	{
		BST* bst = new BST();
		cout << endl << endl << "NEW TEST: NUM THREADS: " << nt << endl;
		cout << "Iterations: " << NUM_ITER << endl;
		cout << "Prefill amount: " << NUM_PRE << endl;
		cout << "Range values: 0 - " << (pow(2, EXP_VALS)) << endl;
		// prefill
		cout << "Prefilling..." << endl;
		UINT64 seed = (UINT64)time(nullptr);
		UINT64 r = rand(seed);

		Node* nodes[NUM_PRE];
		for (int i = 0; i < NUM_PRE; i++)
		{
			nodes[i] = new Node();
			UINT64 newkey = r >> (64 - EXP_VALS);
			//cout << newkey << endl;
			nodes[i]->key = (r >> (64 - EXP_VALS));
			bst->add(nodes[i]);
			r = rand(r);
		}

		// start time
		cout << "Starting test..." << endl;
		tstart = getWallClockMS();

		// spawn threads
		for (int thread = 0; thread < nt; thread++)
			createThread(&threadH[thread], bstThread, static_cast<void*>(bst));

		// wait for ALL worker threads to finish
		waitForThreadsToFinish(nt, threadH);


		// find elapsed time
		UINT64 rt = getWallClockMS() - tstart;
		cout << "Test finished!" << endl;
		cout << "Time elapsed: " << rt << " ms" << endl;

		// verify correctness
		if (recursiveVerify(bst->root))
			cout << "BST verified" << endl;
		else
			cout << "ERROR: BST failed verification" << endl;

		// clean up everything on the tree (NOTE: not in the reuseQs)
		recursiveDelete(bst->root);
	}
	getchar();
}

bool recursiveVerify(Node* r)
{
	if (r == nullptr)
		return true;

	if (r->left)
	{
		if(!recursiveVerify(r->left)) 
			return false;
	}
	if (r->right)
	{
		if (!recursiveVerify(r->right))
			return false;
	}
	if (r->left && r->left->key > r->key)
	{
		cout << "left: " << r->left->key << " > " << r->key << endl;
		return false;
	}
	if (r->right && r->right->key < r->key)
	{
		cout << "right: " << r->right->key << " < " << r->key << endl;
		return false;
	}
	return true;	
}

void recursiveDelete(Node* r)
{
	if (r == nullptr)
		return;
	if (r->left)
	{
		recursiveDelete(r->left);
	}
	if (r->right)
	{
		recursiveDelete(r->right);
	}
	delete r;
}

WORKER bstThread(void* _bst)
{
	BST* bst = static_cast<BST*>(_bst);

	//initial seed rand
	UINT64 seed = (UINT64)time(nullptr);
	UINT64 r = rand(seed);
	vector<Node*> reuseQ;

	threadH = (THREADH*)ALIGNED_MALLOC(maxThread*sizeof(THREADH), lineSz);

	for (int i = 0; i < NUM_ITER; i++)
	{
		UINT64 val = r >> (64 - EXP_VALS);

		// add or remove
		if (r & 1)
		{
			//add
			Node* n;
			if (reuseQ.empty())
			{
				n = new Node();
			}
			else
			{
				n = reuseQ.back();
				reuseQ.pop_back();
			}
			n->key = val;

			//LOCK

			/* TATAS Lock */
			/*while (InterlockedExchange(&TATASlock, 1))
				while (TATASlock == 1)
					_mm_pause(); 
			*/

			/* HLE Lock */
			while (_InterlockedExchange_HLEAcquire(&TATASlock, 1))
				while (TATASlock == 1)
					_mm_pause();

			if (!bst->add(n))
			{
				reuseQ.push_back(n);
			}
			//UNLOCK
			/* TATAS */
			// TATASlock = 0;

			/* HLE */
			_Store_HLERelease(&TATASlock, 0);
		}
		else
		{
			//remove

			//LOCK

			/* TATAS Lock */
			/*while (InterlockedExchange(&TATASlock, 1))
			while (TATASlock == 1)
			_mm_pause();
			*/

			/* HLE Lock */
			while (_InterlockedExchange_HLEAcquire(&TATASlock, 1))
				while (TATASlock == 1)
					_mm_pause();

			Node* old = bst->remove(val);
			
			//UNLOCK
			/* TATAS */
			//TATASlock = 0;

			/* HLE */
			_Store_HLERelease(&TATASlock, 0);

			if (old)
			{
				old->left = old->right = nullptr;
				reuseQ.push_back(old);
			}
		}
		r = rand(r);
	}

	return 0;
}

int oldmain()
{
	ncpu = getNumberOfCPUs();   // number of logical CPUs
	maxThread = 2 * ncpu;       // max number of threads

								//
								// get date
								//
	char dateAndTime[256];
	getDateAndTime(dateAndTime, sizeof(dateAndTime));

	//
	// console output
	//
	cout << getHostName() << " " << getOSName() << " sharing " << (is64bitExe() ? "(64" : "(32") << "bit EXE)";
#ifdef _DEBUG
	cout << " DEBUG";
#else
	cout << " RELEASE";
#endif
	cout << " [" << OPSTR << "]" << " NCPUS=" << ncpu << " RAM=" << (getPhysicalMemSz() + GB - 1) / GB << "GB " << dateAndTime << endl;
#ifdef COUNTER64
	cout << "COUNTER64";
#else
	cout << "COUNTER32";
#endif
#ifdef FALSESHARING
	cout << " FALSESHARING";
#endif
	cout << " NOPS=" << NOPS << " NSECONDS=" << NSECONDS << " OPTYP=" << OPTYP;
#ifdef USEPMS
	cout << " USEPMS";
#endif
	cout << endl;
	cout << "Intel" << (cpu64bit() ? "64" : "32") << " family " << cpuFamily() << " model " << cpuModel() << " stepping " << cpuStepping() << " " << cpuBrandString() << endl;
#ifdef USEPMS
	cout << "performance monitoring version " << pmversion() << ", " << nfixedCtr() << " x " << fixedCtrW() << "bit fixed counters, " << npmc() << " x " << pmcW() << "bit performance counters" << endl;
#endif

	//
	// get cache info
	//
	lineSz = getCacheLineSz();
	//lineSz *= 2;

	if ((&cnt3 >= &cnt0) && (&cnt3 < (&cnt0 + lineSz / sizeof(UINT64))))
		cout << "Warning: cnt3 shares cache line used by cnt0" << endl;
	if ((&cnt3 >= &cnt1) && (&cnt3 < (&cnt1 + lineSz / sizeof(UINT64))))
		cout << "Warning: cnt3 shares cache line used by cnt1" << endl;
	if ((&cnt3 >= &cnt2) && (&cnt3 < (&cnt2 + lineSz / sizeof(UINT64))))
		cout << "Warning: cnt2 shares cache line used by cnt1" << endl;

#if OPTYP == 3

	//
	// check if RTM supported
	//
	if (!rtmSupported()) {
		cout << "RTM (restricted transactional memory) NOT supported by this CPU" << endl;
		quit();
		return 1;
	}

#endif

	cout << endl;

	//
	// allocate global variable
	//
	// NB: each element in g is stored in a different cache line to stop false sharing
	//
	threadH = (THREADH*)ALIGNED_MALLOC(maxThread*sizeof(THREADH), lineSz);             // thread handles
	ops = (UINT64*)ALIGNED_MALLOC(maxThread*sizeof(UINT64), lineSz);                   // for ops per thread

#if OPTYP == 3
	aborts = (UINT64*)ALIGNED_MALLOC(maxThread*sizeof(UINT64), lineSz);                // for counting aborts
#endif

#ifdef FALSESHARING
	g = (VINT*)ALIGNED_MALLOC((maxThread + 1)*sizeof(VINT), lineSz);                     // local and shared global variables
#else
	g = (VINT*)ALIGNED_MALLOC((maxThread + 1)*lineSz, lineSz);                         // local and shared global variables
#endif

#ifdef USEPMS

	fixedCtr0 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);      // for fixed counter 0 results
	fixedCtr1 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);      // for fixed counter 1 results
	fixedCtr2 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);      // for fixed counter 2 results
	pmc0 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);           // for performance counter 0 results
	pmc1 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);           // for performance counter 1 results
	pmc2 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);           // for performance counter 2 results
	pmc3 = (UINT64*)ALIGNED_MALLOC(5 * maxThread*ncpu*sizeof(UINT64), lineSz);           // for performance counter 3 results

#endif

	r = (Result*)ALIGNED_MALLOC(5 * maxThread*sizeof(Result), lineSz);                   // for results
	memset(r, 0, 5 * maxThread*sizeof(Result));                                           // zero

	indx = 0;

#ifdef USEPMS
	//
	// set up performance monitor counters
	//
	setupCounters();
#endif

	//
	// use thousands comma separator
	//
	setCommaLocale();

	//
	// header
	//
	cout << "sharing";
	cout << setw(4) << "nt";
	cout << setw(6) << "rt";
	cout << setw(16) << "ops";
	cout << setw(6) << "rel";
#if OPTYP == 3
	cout << setw(8) << "commit";
#endif
	cout << endl;

	cout << "-------";              // sharing
	cout << setw(4) << "--";        // nt
	cout << setw(6) << "--";        // rt
	cout << setw(16) << "---";      // ops
	cout << setw(6) << "---";       // rel
#if OPTYP == 3
	cout << setw(8) << "------";
#endif
	cout << endl;

	//
	// boost process priority
	// boost current thread priority to make sure all threads created before they start to run
	//
#ifdef WIN32
	//  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	//  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif

	//
	// run tests
	//
	UINT64 ops1 = 1;

	for (sharing = 0; sharing <= 100; sharing += 25) {

		for (int nt = 1; nt <= maxThread; nt *= 2, indx++) {

			//
			//  zero shared memory
			//
			for (int thread = 0; thread < nt; thread++)
				*(GINDX(thread)) = 0;   // thread local
			*(GINDX(maxThread)) = 0;    // shared

#ifdef USEPMS
			zeroCounters();             // zero PMS counters
#endif

										//
										// get start time
										//
			tstart = getWallClockMS();

			//
			// create worker threads
			//
			for (int thread = 0; thread < nt; thread++)
				createThread(&threadH[thread], lockWorker, (void*)(size_t)thread);

			//
			// wait for ALL worker threads to finish
			//
			waitForThreadsToFinish(nt, threadH);
			UINT64 rt = getWallClockMS() - tstart;

#ifdef USEPMS
			saveCounters();             // save PMS counters
#endif

										//
										// save results and output summary to console
										//
			for (int thread = 0; thread < nt; thread++) {
				r[indx].ops += ops[thread];
				r[indx].incs += *(GINDX(thread));
#if OPTYP == 3
				r[indx].aborts += aborts[thread];
#endif
			}
			r[indx].incs += *(GINDX(maxThread));
			if ((sharing == 0) && (nt == 1))
				ops1 = r[indx].ops;
			r[indx].sharing = sharing;
			r[indx].nt = nt;
			r[indx].rt = rt;

			cout << setw(6) << sharing << "%";
			cout << setw(4) << nt;
			cout << setw(6) << fixed << setprecision(2) << (double)rt / 1000;
			cout << setw(16) << r[indx].ops;
			cout << setw(6) << fixed << setprecision(2) << (double)r[indx].ops / ops1;

#if OPTYP == 3

			cout << setw(7) << fixed << setprecision(0) << 100.0 * (r[indx].ops - r[indx].aborts) / r[indx].ops << "%";

#endif

			if (r[indx].ops != r[indx].incs)
				cout << " ERROR incs " << setw(3) << fixed << setprecision(0) << 100.0 * r[indx].incs / r[indx].ops << "% effective";

			cout << endl;

			//
			// delete thread handles
			//
			for (int thread = 0; thread < nt; thread++)
				closeThread(threadH[thread]);

		}

	}

	cout << endl;

	//
	// output results so they can easily be pasted into a spread sheet from console window
	//
	setLocale();
	cout << "sharing/nt/rt/ops/incs";
#if OPTYP == 3
	cout << "/aborts";
#endif
	cout << endl;
	for (UINT i = 0; i < indx; i++) {
		cout << r[i].sharing << "/" << r[i].nt << "/" << r[i].rt << "/" << r[i].ops << "/" << r[i].incs;
#if OPTYP == 3
		cout << "/" << r[i].aborts;
#endif
		cout << endl;
	}
	cout << endl;

#ifdef USEPMS

	//
	// output PMS counters
	//
	cout << "FIXED_CTR0 instructions retired" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << fixedCtr0[i*ncpu + j];
		cout << endl;
	}
	cout << endl;
	cout << "FIXED_CTR1 unhalted core cycles" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << fixedCtr1[i*ncpu + j];
		cout << endl;
	}
	cout << endl;
	cout << "FIXED_CTR2 unhalted reference cycles" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << fixedCtr2[i*ncpu + j];
		cout << endl;
	}
	cout << endl;
	cout << "PMC0 RTM RETIRED START" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << pmc0[i*ncpu + j];
		cout << endl;
	}
	cout << endl;
	cout << "PMC1 RTM RETIRED COMMIT" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << pmc1[i*ncpu + j];
		cout << endl;
	}
	cout << endl;
	cout << "PMC2 unhalted core cycles in committed transactions" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << pmc2[i*ncpu + j];
		cout << endl;
	}
	cout << endl;
	cout << "PMC3 unhalted core cycles in committed and aborted transactions" << endl;
	for (UINT i = 0; i < indx; i++) {
		for (UINT j = 0; j < ncpu; j++)
			cout << ((j) ? "/" : "") << pmc3[i*ncpu + j];
		cout << endl;
	}

	closePMS();                 // close PMS counters

#endif

	quit();

	return 0;

}

// eof
