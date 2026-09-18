#ifndef __LIBTHREAD__
#define __LIBTHREAD__

__BEGIN_DECLS

/* SPIN_BACKOFF define */
#if defined(ENV_WINDOWS)
#include <windows.h>
#define SPIN_BACKOFF_HOOK YieldProcessor()
#elif defined(ENV_GNUC)
#if defined(__x86_64__) || defined(__i386__)
#define SPIN_BACKOFF_HOOK __asm__ __volatile__("pause" ::: "memory")
#else
#define SPIN_BACKOFF_HOOK __asm__ __volatile__("" ::: "memory")
#endif
#elif defined(ENV_STDC_C11)
#define SPIN_BACKOFF_HOOK atomic_signal_fence(memory_order_seq_cst)
#else
#define SPIN_BACKOFF_HOOK ((void)0) /* fallback: no-op */
#endif
#define SPIN_BACKOFF_MIN    4
#define SPIN_BACKOFF_MAX    128
#define SPIN_BACKOFF(__count) \
  { \
    for (int __i = 0; __i < __count; __i++) { \
      SPIN_BACKOFF_HOOK; \
    } \
    if (__count < SPIN_BACKOFF_MAX) __count += __count; \
  }

/* atomic operations */
#if defined(ENV_WINDOWS)
#define atomic_compare_exchange_weak(ptr, expected, desired) \
	(InterlockedCompareExchange((volatile LONG*)(ptr), (LONG)(desired), (LONG)(*(expected))) == (LONG)(*(expected)))
#define atomic_compare_exchange_strong(ptr, expected, desired) atomic_compare_exchange_weak(ptr, expected, desired)
#define atomic_compare_exchange_weak64(ptr, expected, desired) \
	(InterlockedCompareExchange64((volatile LONGLONG*)(ptr), (LONGLONG)(desired), (LONGLONG)(*(expected))) == (LONGLONG)(*(expected)))
#define atomic_compare_exchange_strong64(ptr, expected, desired) atomic_compare_exchange_weak64(ptr, expected, desired)
#define memory_order_relaxed 0
#define memory_order_acquire 0
#define memory_order_release 0
#define memory_order_seq_cst 0
#define atomic_thread_fence(m) MemoryBarrier()
#define atomic_store_explicit(ptr, value, order) \
	InterlockedExchange((volatile LONG*)(ptr), (LONG)(value))
#define atomic_store(ptr, value) atomic_store_explicit(ptr, value, 0)
#define atomic_store_explicit64(ptr, value, order) \
	InterlockedExchange64((volatile LONGLONG*)(ptr), (LONGLONG)(value))
#define atomic_store64 atomic_store_explicit64(ptr, value)
#define atomic_load_explicit(ptr, order) \
	InterlockedCompareExchange((volatile LONG*)(ptr), 0, 0)
#define atomic_load(ptr) atomic_load_explicit(ptr, 0)
#define atomic_load_explicit64(ptr, order) \
	InterlockedCompareExchange64((volatile LONGLONG*)(ptr), 0, 0)
#define atomic_load64(ptr) atomic_load_explicit64(ptr, 0)
#define atomic_fetch_and_explicit(ptr, mask, order) \
    InterlockedAnd((volatile LONG*)(ptr), mask)
#define atomic_fetch_and(ptr, mask) atomic_fetch_and_explicit(ptr, mask, 0)
#define atomic_fetch_and_explicit64(ptr, mask, order) \
    InterlockedAnd64((volatile LONGLONG*)(ptr), mask)
#define atomic_fetch_and64(ptr, mask) atomic_fetch_and_explicit64(ptr, mask, 0)
#define atomic_fetch_or_explicit(ptr, mask, order) \
    InterlockedOr((volatile LONG*)(ptr), mask)
#define atomic_fetch_or(ptr, mask) atomic_fetch_or_explicit(ptr, mask, 0)
#define atomic_fetch_or_explicit64(ptr, mask, order) \
    InterlockedOr64((volatile LONGLONG*)(ptr), mask)
#define atomic_fetch_or64(ptr, mask) atomic_fetch_or_explicit64(ptr, mask, 0)
#else
#include <stdatomic.h>
#ifndef atomic_compare_exchange_weak
#define	atomic_compare_exchange_weak(ptr, expected, desired) \
    __sync_bool_compare_and_swap(ptr, *(expected), desired)
#endif
#ifndef atomic_thread_fence
#define	memory_order_relaxed	__ATOMIC_RELAXED
#define	memory_order_acquire	__ATOMIC_ACQUIRE
#define	memory_order_release	__ATOMIC_RELEASE
#define	memory_order_seq_cst	__ATOMIC_SEQ_CST
#define	atomic_thread_fence(m)	__atomic_thread_fence(m)
#endif
#ifndef atomic_store_explicit
#define	atomic_store_explicit	__atomic_store_n
#endif
#ifndef atomic_load_explicit
#define	atomic_load_explicit	__atomic_load_n
#endif
#ifndef atomic_fetch_and_explicit
#define	atomic_fetch_and_explicit	__atomic_fetch_and 
#endif
#endif
#ifndef atomic_compare_exchange_weak64
#define atomic_compare_exchange_weak64 atomic_compare_exchange_weak
#define atomic_compare_exchange_strong64 atomic_compare_exchange_strong
#endif
#ifndef atomic_store_explicit64
#define atomic_store_explicit64 atomic_store_explicit
#define atomic_store64 atomic_store
#endif
#ifndef atomic_load_explicit64
#define atomic_load_explicit64 atomic_load_explicit
#define atomic_load64 atomic_load
#endif
#ifndef atomic_fetch_and_explicit64
#define atomic_fetch_and_explicit64 atomic_fetch_and_explicit
#define atomic_fetch_and64 atomic_fetch_and
#endif
#ifndef atomic_fetch_or_explicit64
#define atomic_fetch_or_explicit64 atomic_fetch_or_explicit
#define atomic_fetch_or64 atomic_fetch_or
#endif

/* If TIME_UTC is missing, provide it and provide a wrapper for
   timespec_get. */
#ifndef TIME_UTC
#define TIME_UTC 1
#define _TTHREAD_EMULATE_TIMESPEC_GET_

#if defined(ENV_WINDOWS)
struct _tthread_timespec {
    time_t tv_sec;
    long tv_nsec;
};
#define timespec _tthread_timespec
#endif

int _tthread_timespec_get(struct timespec* ts, int base);
#define timespec_get _tthread_timespec_get
#endif

/* Macros */
#if defined(ENV_WINDOWS)
#define TSS_DTOR_ITERATIONS (4)
#else
#define TSS_DTOR_ITERATIONS PTHREAD_DESTRUCTOR_ITERATIONS
#endif

/* Function return values */
#define E_THRD_ERROR    0
#define E_THRD_SUCCESS  1
#define E_THRD_TIMEDOUT 2
#define E_THRD_BUSY     3
#define E_THRD_NOMEM    4

/* Mutex types */
#define MTX_PLAIN       0
#define MTX_TIMED       1
#define MTX_RECURSIVE   2

/* Mutex */
#if defined(ENV_WINDOWS)
typedef struct {
    union {
        CRITICAL_SECTION cs; /* Critical section handle (used for non-timed mutexes) */
        HANDLE mut; /* Mutex handle (used for timed mutex) */
    } mHandle; /* Mutex handle */
    int mAlreadyLocked; /* TRUE if the mutex is already locked */
    int mRecursive; /* TRUE if the mutex is recursive */
    int mTimed; /* TRUE if the mutex is timed */
} st_Mtx, *pst_Mtx;
#else
typedef pthread_mutex_t st_Mtx, *pst_Mtx;
#endif

int dInit_Mtx(pst_Mtx pstMtx, int type);
void Destory_Mtx(pst_Mtx pstMtx);
int dLock_Mtx(pst_Mtx pstMtx);
int dLockTimed_Mtx(pst_Mtx pstMtx, const struct timespec* ts);
int dLockTry_Mtx(pst_Mtx pstMtx);
int dUnlock_Mtx(pst_Mtx pstMtx);

/* Condition variable */
#if defined(ENV_WINDOWS)
typedef struct {
    HANDLE mEvents[2]; /* Signal and broadcast event HANDLEs. */
    unsigned int mWaitersCount; /* Count of the number of waiters. */
    CRITICAL_SECTION mWaitersCountLock; /* Serialize access to mWaitersCount. */
} st_ThreadCond, *pst_ThreadCond;
#else
typedef pthread_cond_t st_ThreadCond, *pst_ThreadCond;
#endif

int dInit_ThreadCond(pst_ThreadCond pstCond);
void Destroy_ThreadCond(pst_ThreadCond pstCond);
int dSignal_ThreadCond(pst_ThreadCond pstCond);
int dBroadcast_ThreadCond(pst_ThreadCond pstCond);
int dWait_ThreadCond(pst_ThreadCond pstCond, pst_Mtx pstMtx);
int dWaitTimed_ThreadCond(pst_ThreadCond pstCond, pst_Mtx pstMtx, const struct timespec* ts);

/* Thread */
#if defined(ENV_WINDOWS)
typedef HANDLE thrd_t;
#else
typedef pthread_t thrd_t;
#endif

typedef int (*pf_TheadStart)(void*);

int dCreate_Thread(thrd_t* pkThr, pf_TheadStart pfFunc, void* pvArg);
thrd_t kCurrent_Thread(void);
int dDetach_Thread(thrd_t kThr);
int dEqual_Thread(thrd_t kThr0, thrd_t kThr1);
NORETURN void Exit_Thread(int dRes);
int dJoin_Thread(thrd_t kThr, int* pdRes);
int dSleep_Thread(const struct timespec* pstDuration, struct timespec* pstRemaining);
void Yield_Thread(void);

/* Thread local storage */
#if defined(ENV_WINDOWS)
typedef DWORD tss_t;
#else
typedef pthread_key_t tss_t;
#endif

typedef void (*pf_DestructTss)(void* pvVal);

int dCreate_Tss(tss_t* pkKey, pf_DestructTss pfDtor);
void Delete_Tss(tss_t kKey);
void* pvGet_Tss(tss_t kKey);
int dSet_Tss(tss_t kKey, void* pvVal);

#if defined(ENV_WINDOWS)
typedef struct {
    LONG volatile lStatus;
    CRITICAL_SECTION stLock;
} once_flag;
#define ONCE_FLAG_INIT { \
    0,                   \
}
#else
#define once_flag pthread_once_t
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#endif

#if defined(ENV_WINDOWS)
void Call_ThreadOnce(once_flag* flag, void (*func)(void));
#else
#define Call_ThreadOnce(flag, func) pthread_once(flag, func)
#endif

/* Thread barrior */
#if defined(ENV_WINDOWS)
typedef struct {
    SYNCHRONIZATION_BARRIER barrier;
} barrier_t;
#elif defined(ENV_POSIX)
typedef pthread_barrier_t barrier_t;
#else
#error "Unsupported platform for barrier abstraction"
#endif

int dInit_ThreadBarrier(barrier_t* b, unsigned int count);
int dWait_ThreadBarrier(barrier_t* b);
int dDestroy_ThreadBarrier(barrier_t* b);

/* ring queue related */

#define DEF_RING_QUEUE_SIZE (10 * 1024 * 1024)

#if 0 //queue_old.c
typedef struct CACHE_ALIGN {
    _Atomic(unsigned int) uiHead;
    char _padHead[CACHE_LINE_SIZE - sizeof(_Atomic(unsigned int))];
    _Atomic(unsigned int) uiEnTail;
    char _padEnTail[CACHE_LINE_SIZE - sizeof(_Atomic(unsigned int))];
    _Atomic(unsigned int) uiDeTail;
    char _padDeTail[CACHE_LINE_SIZE - sizeof(_Atomic(unsigned int))];
    char* pszBuffer;
    unsigned int uiBufferSize;
} st_RingQueue, *pst_RingQueue;

int dInit_RingQueue(pst_RingQueue pstQueue, unsigned int uiBufferSize);
void Release_RingQueue(pst_RingQueue pstQueue);
int dEnqueue_RingQueue(pst_RingQueue pstQueue, const void* pvData, unsigned long uiLength);
int dDequeue_RingQueue(pst_RingQueue pstQueue, void* pvOutput, unsigned int *puiLength);
#endif

typedef struct {
	ATOMIC unsigned long long ullOffReady;
	ATOMIC int isActive;
} st_QueueWorker, *pst_QueueWorker;

typedef struct {
	unsigned long long ullBufLen;
    char *pszBuf;
	ATOMIC unsigned long long ullWritePos;
	unsigned long long ullWrapLimit;
	ATOMIC unsigned long long ullReadPos;
	int dNumWorker;
	st_QueueWorker stWorker[];
} st_RingQueue, *pst_RingQueue;

#define RING_QUEUE_DESC_SIZE(n)     (sizeof(st_RingQueue) + (size_t)(n) * sizeof(st_QueueWorker))

pst_RingQueue pstInit_RingQueue(int dNumWorker, long long llLength);
void Destroy_RingQueue(pst_RingQueue *ppstRQ);
pst_QueueWorker pstGet_RingQueueWorker(pst_RingQueue pstRQ, int dIndex);
void Release_RingQueueWorker(pst_QueueWorker pstQW);
long long llAcquire_RingQueue(pst_RingQueue, pst_QueueWorker, unsigned long long);
void Produce_RingQueue(pst_QueueWorker pstQW);
long long llConsume_RingQueue(pst_RingQueue pstRQ, unsigned long long *pullWriteLen);
void Release_RingQueue(pst_RingQueue pstRQ, unsigned long long ullReaded);

__END_DECLS

#endif /* __LIBTHREAD__ */
