use std::cell::UnsafeCell;
use std::fmt;
use std::hint::spin_loop;
use std::sync::atomic::{AtomicI32, AtomicU64, Ordering};

#[cfg(not(target_has_atomic = "64"))]
compile_error!("st_RingQueue requires native 64-bit atomic operations");

pub const DEF_RING_QUEUE_SIZE: usize = 10 * 1024 * 1024;

const RBUF_OFF_MASK: u64 = 0x0000_0000_ffff_ffff;
const WRAP_LOCK_BIT: u64 = 0x8000_0000_0000_0000;
const RBUF_OFF_MAX: u64 = u64::MAX & !WRAP_LOCK_BIT;

const WRAP_COUNTER: u64 = 0x7fff_ffff_0000_0000;
const SPIN_BACKOFF_MIN: u32 = 4;
const SPIN_BACKOFF_MAX: u32 = 128;

#[inline]
fn WRAP_INCR(x: u64) -> u64 {
    x.wrapping_add(0x1_0000_0000) & WRAP_COUNTER
}

#[inline]
fn SPIN_BACKOFF(__count: &mut u32) {
    for _ in 0..*__count {
        // On x86/x86_64 this lowers to PAUSE, matching libthread.h.
        spin_loop();
    }

    if *__count < SPIN_BACKOFF_MAX {
        *__count = (*__count).saturating_mul(2).min(SPIN_BACKOFF_MAX);
    }
}

#[inline]
fn STABLE_CALL(pstAtomic: &AtomicU64, ordering: Ordering) -> u64 {
    let mut __count = SPIN_BACKOFF_MIN;

    loop {
        let __val = pstAtomic.load(ordering);
        if (__val & WRAP_LOCK_BIT) != 0 {
            SPIN_BACKOFF(&mut __count);
        } else {
            return __val;
        }
    }
}

/// Rust counterpart of `st_QueueWorker` from `libthread.h`.
///
/// The field names and integer representation intentionally follow the C source.
pub struct st_QueueWorker {
    ullOffReady: AtomicU64,
    isActive: AtomicI32,
}

impl st_QueueWorker {
    #[inline]
    fn new() -> Self {
        Self {
            ullOffReady: AtomicU64::new(0),
            isActive: AtomicI32::new(0),
        }
    }
}

/// Rust counterpart of `st_RingQueue` from `libthread.h`.
///
/// C uses `char *pszBuf` plus a flexible-array member `stWorker[]`. Rust owns the
/// same two allocations as boxed slices. `ullWrapLimit` intentionally remains
/// non-atomic, as in the C header; accesses are ordered by the queue's
/// `WRAP_LOCK_BIT` / `ullWritePos` and `ullReadPos` protocol.
pub struct st_RingQueue {
    ullBufLen: u64,
    pszBuf: Box<[UnsafeCell<u8>]>,
    ullWritePos: AtomicU64,
    ullWrapLimit: UnsafeCell<u64>,
    ullReadPos: AtomicU64,
    dNumWorker: i32,
    stWorker: Box<[st_QueueWorker]>,
}

// The original algorithm deliberately shares the buffer and ullWrapLimit between
// threads. Atomics establish reservation/publication order; raw payload access is
// exposed only through the unsafe pszBuf_ptr() API below.
unsafe impl Sync for st_RingQueue {}

pub type pst_RingQueue = Box<st_RingQueue>;
pub type pst_QueueWorker<'a> = &'a st_QueueWorker;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RingQueueError {
    InvalidArgument,
    AllocationFailed,
}

impl fmt::Display for RingQueueError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidArgument => f.write_str("invalid ring queue argument"),
            Self::AllocationFailed => f.write_str("ring queue allocation failed"),
        }
    }
}

impl std::error::Error for RingQueueError {}

impl st_RingQueue {
    #[inline]
    fn ullWrapLimit_load(&self) -> u64 {
        // SAFETY: The C algorithm makes this non-atomic field visible only after
        // the wrapping producer clears WRAP_LOCK_BIT with a Release store. The
        // consumer first observes ullWritePos with Acquire through STABLE_CALL.
        unsafe { *self.ullWrapLimit.get() }
    }

    #[inline]
    fn ullWrapLimit_store(&self, value: u64) {
        // SAFETY: Writes follow the same single-owner phases as the C algorithm:
        // the wrapping producer owns the wrap transition, while the single
        // consumer resets the limit after completing that tail region.
        unsafe {
            *self.ullWrapLimit.get() = value;
        }
    }

    pub fn ullBufLen(&self) -> u64 {
        self.ullBufLen
    }

    pub fn dNumWorker(&self) -> i32 {
        self.dNumWorker
    }

    /// Returns the base address corresponding to C's `pstRQ->pszBuf`.
    ///
    /// # Safety
    ///
    /// The caller must obey the original queue protocol:
    ///
    /// - producer: `llAcquire_RingQueue` -> write only its reserved range ->
    ///   `Produce_RingQueue`;
    /// - consumer: `llConsume_RingQueue` -> read only the returned range ->
    ///   `Release_RingQueue`;
    /// - no payload reference or slice may outlive that reservation/consume phase.
    pub unsafe fn pszBuf_ptr(&self) -> *mut u8 {
        self.pszBuf.as_ptr().cast::<u8>() as *mut u8
    }
}

pub fn pstInit_RingQueue(
    dNumWorker: i32,
    llLength: i64,
) -> Result<pst_RingQueue, RingQueueError> {
    // The C implementation assumes a non-negative worker count and positive
    // buffer length. Rust rejects invalid values instead of allowing an integer
    // conversion to turn them into a huge allocation.
    if dNumWorker < 0 || llLength <= 0 || (llLength as u64) >= RBUF_OFF_MASK {
        return Err(RingQueueError::InvalidArgument);
    }

    let ullBufLen = llLength as u64;
    let buf_len = usize::try_from(ullBufLen).map_err(|_| RingQueueError::InvalidArgument)?;
    let worker_len = usize::try_from(dNumWorker).map_err(|_| RingQueueError::InvalidArgument)?;

    let mut pszBuf = Vec::new();
    pszBuf
        .try_reserve_exact(buf_len)
        .map_err(|_| RingQueueError::AllocationFailed)?;
    pszBuf.extend((0..buf_len).map(|_| UnsafeCell::new(0u8)));

    let mut stWorker = Vec::new();
    stWorker
        .try_reserve_exact(worker_len)
        .map_err(|_| RingQueueError::AllocationFailed)?;
    stWorker.extend((0..worker_len).map(|_| st_QueueWorker::new()));

    Ok(Box::new(st_RingQueue {
        ullBufLen,
        pszBuf: pszBuf.into_boxed_slice(),
        ullWritePos: AtomicU64::new(0),
        ullWrapLimit: UnsafeCell::new(RBUF_OFF_MAX),
        ullReadPos: AtomicU64::new(0),
        dNumWorker,
        stWorker: stWorker.into_boxed_slice(),
    }))
}

#[allow(clippy::boxed_local)]
pub fn Destroy_RingQueue(_pstRQ: pst_RingQueue) {
    // Rust drops pszBuf, stWorker and the descriptor automatically here.
}

pub fn pstGet_RingQueueWorker(pstRQ: &st_RingQueue, dIndex: i32) -> pst_QueueWorker<'_> {
    let dIndex = usize::try_from(dIndex).expect("dIndex must be non-negative");
    let pstQW = &pstRQ.stWorker[dIndex];

    // `pstQW->ullOffReady = RBUF_OFF_MAX` is an atomic assignment in C11, hence
    // sequentially consistent. Keep that property before publishing isActive.
    pstQW.ullOffReady.store(RBUF_OFF_MAX, Ordering::SeqCst);
    pstQW.isActive.store(1, Ordering::Release);
    pstQW
}

pub fn Release_RingQueueWorker(pstQW: &st_QueueWorker) {
    pstQW.isActive.store(0, Ordering::Relaxed);
}

pub fn llAcquire_RingQueue(
    pstRQ: &st_RingQueue,
    pstQW: &st_QueueWorker,
    ullLen: u64,
) -> i64 {
    debug_assert!(ullLen > 0 && ullLen <= pstRQ.ullBufLen);
    debug_assert_eq!(pstQW.ullOffReady.load(Ordering::Acquire), RBUF_OFF_MAX);

    let mut ullReadyPos: u64;
    let mut ullWritePos: u64;
    let mut ullTargetPos: u64;
    let mut ullReadPos: u64;

    loop {
        ullReadyPos = STABLE_CALL(&pstRQ.ullWritePos, Ordering::Acquire);
        ullWritePos = ullReadyPos & RBUF_OFF_MASK;
        debug_assert!(ullWritePos < pstRQ.ullBufLen);

        pstQW
            .ullOffReady
            .store(ullWritePos | WRAP_LOCK_BIT, Ordering::Relaxed);

        ullTargetPos = ullWritePos + ullLen;
        ullReadPos = pstRQ.ullReadPos.load(Ordering::Acquire);

        if ullWritePos < ullReadPos && ullTargetPos >= ullReadPos {
            pstQW.ullOffReady.store(RBUF_OFF_MAX, Ordering::Release);
            return -1;
        }

        if ullTargetPos >= pstRQ.ullBufLen {
            let isWrapAround = ullTargetPos > pstRQ.ullBufLen;

            ullTargetPos = if isWrapAround {
                WRAP_LOCK_BIT | ullLen
            } else {
                0
            };

            if (ullTargetPos & RBUF_OFF_MASK) >= ullReadPos {
                pstQW.ullOffReady.store(RBUF_OFF_MAX, Ordering::Release);
                return -1;
            }

            ullTargetPos |= WRAP_INCR(ullReadyPos & WRAP_COUNTER);
        } else {
            ullTargetPos |= ullReadyPos & WRAP_COUNTER;
        }

        // On Rocky/GNU C11, atomic_compare_exchange_weak() from <stdatomic.h>
        // uses sequentially-consistent ordering when no explicit order is given.
        if pstRQ
            .ullWritePos
            .compare_exchange_weak(
                ullReadyPos,
                ullTargetPos,
                Ordering::SeqCst,
                Ordering::SeqCst,
            )
            .is_ok()
        {
            break;
        }
    }

    pstQW
        .ullOffReady
        .fetch_and(!WRAP_LOCK_BIT, Ordering::Relaxed);

    if (ullTargetPos & WRAP_LOCK_BIT) != 0 {
        debug_assert!(ullReadPos <= ullWritePos);
        debug_assert_eq!(pstRQ.ullWrapLimit_load(), RBUF_OFF_MAX);

        pstRQ.ullWrapLimit_store(ullWritePos);
        ullWritePos = 0;

        // Publish ullWrapLimit before exposing the unlocked wrapped write pos.
        pstRQ
            .ullWritePos
            .store(ullTargetPos & !WRAP_LOCK_BIT, Ordering::Release);
    }

    debug_assert!((ullTargetPos & RBUF_OFF_MASK) <= pstRQ.ullBufLen);
    ullWritePos as i64
}

pub fn Produce_RingQueue(pstQW: &st_QueueWorker) {
    debug_assert_ne!(pstQW.isActive.load(Ordering::Acquire), 0);
    debug_assert_ne!(pstQW.ullOffReady.load(Ordering::Acquire), RBUF_OFF_MAX);

    // Producer payload writes happen-before a consumer that observes this
    // completion marker through an Acquire ullOffReady load.
    pstQW.ullOffReady.store(RBUF_OFF_MAX, Ordering::Release);
}

pub fn llConsume_RingQueue(pstRQ: &st_RingQueue, pullWriteLen: &mut u64) -> i64 {
    let mut ullReadPos = pstRQ.ullReadPos.load(Ordering::Acquire);

    loop {
        let mut ullWritePos = STABLE_CALL(&pstRQ.ullWritePos, Ordering::Acquire);
        ullWritePos &= RBUF_OFF_MASK;

        if ullReadPos == ullWritePos {
            return -1;
        }

        let mut ullReady = RBUF_OFF_MAX;

        for i in 0..(pstRQ.dNumWorker as usize) {
            let pstQW = &pstRQ.stWorker[i];

            if pstQW.isActive.load(Ordering::Relaxed) == 0 {
                continue;
            }

            let ullOffReady = STABLE_CALL(&pstQW.ullOffReady, Ordering::Acquire);

            if ullOffReady >= ullReadPos {
                ullReady = ullReady.min(ullOffReady);
            }

            debug_assert!(ullReady >= ullReadPos);
        }

        if ullWritePos < ullReadPos {
            let ullWrapLimit = pstRQ.ullBufLen.min(pstRQ.ullWrapLimit_load());

            if ullReady == RBUF_OFF_MAX && ullReadPos == ullWrapLimit {
                if pstRQ.ullWrapLimit_load() != RBUF_OFF_MAX {
                    pstRQ.ullWrapLimit_store(RBUF_OFF_MAX);
                }

                ullReadPos = 0;
                pstRQ.ullReadPos.store(ullReadPos, Ordering::Release);
                continue;
            }

            debug_assert!(ullReady > ullWritePos);
            ullReady = ullReady.min(ullWrapLimit);
            debug_assert!(ullReady >= ullReadPos);
        } else {
            ullReady = ullReady.min(ullWritePos);
        }

        *pullWriteLen = ullReady - ullReadPos;

        debug_assert!(ullReady >= ullReadPos);
        debug_assert!(*pullWriteLen <= pstRQ.ullBufLen);
        return ullReadPos as i64;
    }
}

pub fn Release_RingQueue(pstRQ: &st_RingQueue, ullReaded: u64) {
    let mut ullReadPos = pstRQ.ullReadPos.load(Ordering::Acquire);

    debug_assert!(ullReadPos <= pstRQ.ullBufLen);
    debug_assert!(ullReadPos <= pstRQ.ullWrapLimit_load());

    ullReadPos += ullReaded;
    debug_assert!(ullReadPos <= pstRQ.ullBufLen);

    let ullReadPos = if ullReadPos == pstRQ.ullBufLen {
        0
    } else {
        ullReadPos
    };

    // The C source uses memory_order_relaxed here. Rust strengthens this store to
    // Release so the consumer's completed payload reads happen-before a producer
    // that observes the advanced read position with Acquire and reuses the bytes.
    pstRQ.ullReadPos.store(ullReadPos, Ordering::Release);
}
