#define UTILBASE
#include <define.h>
#include <libthread.h>

#define	RBUF_OFF_MASK	(0x00000000ffffffffUL)
#define	WRAP_LOCK_BIT	(0x8000000000000000UL)
#define	RBUF_OFF_MAX	(UINT64_MAX & ~WRAP_LOCK_BIT)

#define	WRAP_COUNTER	(0x7fffffff00000000UL)
#define	WRAP_INCR(x)	(((x) + 0x100000000UL) & WRAP_COUNTER)

#define STABLE_CALL(expr, type, val) \
    do { \
        unsigned __count = SPIN_BACKOFF_MIN; \
        type __val; \
        do { \
            __val = (expr); \
            if (__val & WRAP_LOCK_BIT) { \
                SPIN_BACKOFF(__count); \
            } else { \
                break; \
            } \
        } while(1); \
        val = __val; \
    } while(0)

#if defined(DEBUG)
#define	ASSERT		assert
#else
#define	ASSERT(x)
#endif


__BEGIN_DECLS

pst_RingQueue pstInit_RingQueue(int dNumWorker, long long llLength)
{
	if (llLength >= RBUF_OFF_MASK) {
		errno = EINVAL;
		return NULL;
	}

    pst_RingQueue pstRQ = malloc(RING_QUEUE_DESC_SIZE(dNumWorker));
    if (pstRQ == NULL) {
        return NULL;
    }
	memset(pstRQ, 0, RING_QUEUE_DESC_SIZE(dNumWorker));
    pstRQ->pszBuf = malloc(llLength);
    if (pstRQ->pszBuf == NULL) {
        free(pstRQ);
        return NULL;
    }
	pstRQ->ullBufLen = llLength;
	pstRQ->ullWrapLimit = RBUF_OFF_MAX;
	pstRQ->dNumWorker = dNumWorker;

    atomic_store_explicit64(&pstRQ->ullWritePos, 0, memory_order_relaxed);
    atomic_store_explicit64(&pstRQ->ullReadPos, 0, memory_order_relaxed);
    for (int i = 0; i < dNumWorker; i++) {
    	pst_QueueWorker pstQW = &pstRQ->stWorker[i];
        atomic_store_explicit64(&pstQW->ullOffReady, 0, memory_order_relaxed);
        atomic_store_explicit(&pstQW->isActive, 0, memory_order_relaxed);
    }

	return pstRQ;
}

void Destroy_RingQueue(pst_RingQueue *ppstRQ)
{
    if (ppstRQ) {
        free((*ppstRQ)->pszBuf);
        free(*ppstRQ);
        ppstRQ = NULL;
    }
}

pst_QueueWorker pstGet_RingQueueWorker(pst_RingQueue pstRQ, int dIndex)
{
	pst_QueueWorker pstQW = &pstRQ->stWorker[dIndex];

	pstQW->ullOffReady = RBUF_OFF_MAX;
	atomic_store_explicit(&pstQW->isActive, 1, memory_order_release);
	return pstQW;
}

void Release_RingQueueWorker(pst_QueueWorker pstQW)
{
	atomic_store_explicit(&pstQW->isActive, 0, memory_order_relaxed);
}

long long llAcquire_RingQueue(pst_RingQueue pstRQ, pst_QueueWorker pstQW, unsigned long long ullLen)
{
	unsigned long long ullReadyPos, ullWritePos, ullTargetPos;

	ASSERT(ullLen > 0 && ullLen <= pstRQ->ullBufLen);
	ASSERT(atomic_load_explicit64(&pstQW->ullOffReady, memory_order_acquire) == RBUF_OFF_MAX);

    unsigned long long ullReadPos;
	do {
		STABLE_CALL(atomic_load_explicit64(&pstRQ->ullWritePos, memory_order_acquire), unsigned long long, ullReadyPos);
		ullWritePos = ullReadyPos & RBUF_OFF_MASK;
		ASSERT(ullWritePos < pstRQ->ullBufLen);
		atomic_store_explicit64(&pstQW->ullOffReady, ullWritePos | WRAP_LOCK_BIT,
		    memory_order_relaxed);

		ullTargetPos = ullWritePos + ullLen;
		ullReadPos = atomic_load_explicit64(&pstRQ->ullReadPos, memory_order_acquire);
		if (UNLIKELY(ullWritePos < ullReadPos && ullTargetPos >= ullReadPos)) {
			atomic_store_explicit64(&pstQW->ullOffReady,
			    RBUF_OFF_MAX, memory_order_release);
			return -1;
		}

		if (UNLIKELY(ullTargetPos >= pstRQ->ullBufLen)) {
			const int isWrapAround = ullTargetPos > pstRQ->ullBufLen;

			ullTargetPos = isWrapAround ? (WRAP_LOCK_BIT | ullLen) : 0;
			if ((ullTargetPos & RBUF_OFF_MASK) >= ullReadPos) {
				atomic_store_explicit64(&pstQW->ullOffReady,
				    RBUF_OFF_MAX, memory_order_release);
				return -1;
			}
			ullTargetPos |= WRAP_INCR(ullReadyPos & WRAP_COUNTER);
		} else {
			ullTargetPos |= ullReadyPos & WRAP_COUNTER;
		}
	} while (!atomic_compare_exchange_weak64(&pstRQ->ullWritePos, &ullReadyPos, ullTargetPos));

	atomic_fetch_and_explicit64(&pstQW->ullOffReady, ~WRAP_LOCK_BIT, memory_order_relaxed);

	if (UNLIKELY(ullTargetPos & WRAP_LOCK_BIT)) {
		ASSERT(ullReadPos <= ullWritePos);
		ASSERT(pstRQ->ullWrapLimit == RBUF_OFF_MAX);
		pstRQ->ullWrapLimit = ullWritePos;
		ullWritePos = 0;

		atomic_store_explicit64(&pstRQ->ullWritePos,
		    (ullTargetPos & ~WRAP_LOCK_BIT), memory_order_release);
	}
	ASSERT((ullTargetPos & RBUF_OFF_MASK) <= pstRQ->ullBufLen);
	return (long long)ullWritePos;
}

void Produce_RingQueue(pst_QueueWorker pstQW)
{
	ASSERT(atomic_load_explicit(&pstQW->isActive, memory_order_acquire));
	ASSERT(atomic_load_explicit64(&pstQW->ullOffReady, memory_order_acquire) != RBUF_OFF_MAX);
	atomic_store_explicit64(&pstQW->ullOffReady, RBUF_OFF_MAX, memory_order_release);
}

long long llConsume_RingQueue(pst_RingQueue pstRQ, unsigned long long *pullWriteLen)
{
	unsigned long long ullReadPos = atomic_load_explicit64(&pstRQ->ullReadPos, memory_order_acquire);
    unsigned long long ullWritePos, ullReady;

retry:
	STABLE_CALL(atomic_load_explicit64(&pstRQ->ullWritePos, memory_order_acquire), unsigned long long, ullWritePos);
	ullWritePos &= RBUF_OFF_MASK;
	if (ullReadPos == ullWritePos) {
		return -1;
	}

	ullReady = RBUF_OFF_MAX;

	for (unsigned i = 0; i < pstRQ->dNumWorker; i++) {
		pst_QueueWorker pstQW = &pstRQ->stWorker[i];
		unsigned long long ullOffReady;

		if (!atomic_load_explicit(&pstQW->isActive, memory_order_relaxed))
			continue;

		STABLE_CALL(atomic_load_explicit64(&pstQW->ullOffReady, memory_order_acquire), unsigned long long, ullOffReady);
		if (ullOffReady >= ullReadPos) {
			ullReady = MIN(ullOffReady, ullReady);
		}
		ASSERT(ullReady >= ullReadPos);
	}

	if (UNLIKELY(ullWritePos < ullReadPos)) {
		const unsigned long long ullWrapLimit = MIN(pstRQ->ullBufLen, pstRQ->ullWrapLimit);

		if (ullReady == RBUF_OFF_MAX && ullReadPos == ullWrapLimit) {
			if (pstRQ->ullWrapLimit != RBUF_OFF_MAX) {
				pstRQ->ullWrapLimit = RBUF_OFF_MAX;
			}

			ullReadPos = 0;
			atomic_store_explicit64(&pstRQ->ullReadPos, ullReadPos, memory_order_release);
			goto retry;
		}

		ASSERT(ullReady > ullWritePos);
		ullReady = MIN(ullReady, ullWrapLimit);
		ASSERT(ullReady >= ullReadPos);
	} else {
		ullReady = MIN(ullReady, ullWritePos);
	}
	*pullWriteLen = ullReady - ullReadPos;

    //printf("ullReadPos %llu\n", ullReadPos);
	ASSERT(ullReady >= ullReadPos);
	ASSERT(*pullWriteLen <= pstRQ->ullBufLen);
	return (long long)ullReadPos;
}

void Release_RingQueue(pst_RingQueue pstRQ, unsigned long long ullReaded)
{
	unsigned long long ullReadPos = atomic_load_explicit64(&pstRQ->ullReadPos, memory_order_acquire);

	ASSERT(ullReadPos <= pstRQ->ullBufLen);
	ASSERT(ullReadPos <= pstRQ->ullWrapLimit);

    ullReadPos += ullReaded;
	ASSERT(ullReadPos <= pstRQ->ullBufLen);

    //printf("ullReadPos %llu pstRQ->ullReadPos %llu ullBufLen %llu\n", ullReadPos, pstRQ->ullReadPos, pstRQ->ullBufLen);

	atomic_store_explicit64(&pstRQ->ullReadPos, (ullReadPos == pstRQ->ullBufLen) ? 0 : ullReadPos,
            memory_order_relaxed);

    //printf("ullReadPos %llu pstRQ->ullReadPos %llu ullBufLen %llu\n", ullReadPos, pstRQ->ullReadPos, pstRQ->ullBufLen);
}

__END_DECLS
