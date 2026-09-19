#![allow(non_snake_case)]
#![deny(unsafe_op_in_unsafe_fn)]

use nonblock_ring_queue::*;
use std::collections::HashSet;
use std::ptr;
use std::sync::{Arc, Barrier};
use std::thread;
use std::time::{Duration, Instant};

unsafe fn write_bytes(pstRQ: &st_RingQueue, ullOff: u64, bytes: &[u8]) {
    assert!(ullOff + bytes.len() as u64 <= pstRQ.ullBufLen());
    // SAFETY: the caller owns this producer reservation and the checked range
    // lies inside the queue allocation.
    unsafe {
        ptr::copy_nonoverlapping(
            bytes.as_ptr(),
            pstRQ.pszBuf_ptr().add(ullOff as usize),
            bytes.len(),
        );
    }
}

unsafe fn read_bytes(pstRQ: &st_RingQueue, ullOff: u64, out: &mut [u8]) {
    assert!(ullOff + out.len() as u64 <= pstRQ.ullBufLen());
    // SAFETY: the caller owns this consumer range and the checked range lies
    // inside the queue allocation.
    unsafe {
        ptr::copy_nonoverlapping(
            pstRQ.pszBuf_ptr().add(ullOff as usize),
            out.as_mut_ptr(),
            out.len(),
        );
    }
}

#[test]
fn basic_single_worker_roundtrip() {
    let pstRQ = pstInit_RingQueue(1, 64).unwrap();
    let pstQW = pstGet_RingQueueWorker(&pstRQ, 0);

    let payload = b"hello";
    let ullWritePos = llAcquire_RingQueue(&pstRQ, pstQW, payload.len() as u64);
    assert_eq!(ullWritePos, 0);

    unsafe {
        write_bytes(&pstRQ, ullWritePos as u64, payload);
    }
    Produce_RingQueue(pstQW);

    let mut ullWriteLen = 0;
    let ullReadPos = unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) };
    assert_eq!(ullReadPos, 0);
    assert_eq!(ullWriteLen, payload.len() as u64);

    let mut out = [0u8; 5];
    unsafe {
        read_bytes(&pstRQ, ullReadPos as u64, &mut out);
    }
    assert_eq!(&out, payload);

    unsafe { Release_RingQueue(&pstRQ, ullWriteLen) };
    assert_eq!(unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) }, -1);
    Release_RingQueueWorker(pstQW);
}

#[test]
fn wraps_at_buffer_end() {
    let pstRQ = pstInit_RingQueue(1, 16).unwrap();
    let pstQW = pstGet_RingQueueWorker(&pstRQ, 0);

    let first = [0x11u8; 10];
    let first_pos = llAcquire_RingQueue(&pstRQ, pstQW, first.len() as u64);
    assert_eq!(first_pos, 0);
    unsafe {
        write_bytes(&pstRQ, first_pos as u64, &first);
    }
    Produce_RingQueue(pstQW);

    let mut ullWriteLen = 0;
    let first_read = unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) };
    assert_eq!(first_read, 0);
    assert_eq!(ullWriteLen, 10);
    unsafe { Release_RingQueue(&pstRQ, ullWriteLen) };

    let second = [0x22u8; 8];
    let second_pos = llAcquire_RingQueue(&pstRQ, pstQW, second.len() as u64);
    assert_eq!(second_pos, 0);
    unsafe {
        write_bytes(&pstRQ, second_pos as u64, &second);
    }
    Produce_RingQueue(pstQW);

    let second_read = unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) };
    assert_eq!(second_read, 0);
    assert_eq!(ullWriteLen, 8);

    let mut out = [0u8; 8];
    unsafe {
        read_bytes(&pstRQ, second_read as u64, &mut out);
    }
    assert_eq!(out, second);

    unsafe { Release_RingQueue(&pstRQ, ullWriteLen) };
    Release_RingQueueWorker(pstQW);
}

#[test]
fn unfinished_earlier_worker_blocks_later_ready_bytes() {
    let pstRQ = pstInit_RingQueue(2, 64).unwrap();
    let pstQW0 = pstGet_RingQueueWorker(&pstRQ, 0);
    let pstQW1 = pstGet_RingQueueWorker(&pstRQ, 1);

    let pos0 = llAcquire_RingQueue(&pstRQ, pstQW0, 4);
    let pos1 = llAcquire_RingQueue(&pstRQ, pstQW1, 4);
    assert_eq!(pos0, 0);
    assert_eq!(pos1, 4);

    unsafe {
        write_bytes(&pstRQ, pos1 as u64, b"BBBB");
    }
    Produce_RingQueue(pstQW1);

    let mut ullWriteLen = 99;
    let ullReadPos = unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) };
    assert_eq!(ullReadPos, 0);
    assert_eq!(ullWriteLen, 0);

    unsafe {
        write_bytes(&pstRQ, pos0 as u64, b"AAAA");
    }
    Produce_RingQueue(pstQW0);

    let ullReadPos = unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) };
    assert_eq!(ullReadPos, 0);
    assert_eq!(ullWriteLen, 8);

    let mut out = [0u8; 8];
    unsafe {
        read_bytes(&pstRQ, ullReadPos as u64, &mut out);
    }
    assert_eq!(&out, b"AAAABBBB");

    unsafe { Release_RingQueue(&pstRQ, ullWriteLen) };
    Release_RingQueueWorker(pstQW0);
    Release_RingQueueWorker(pstQW1);
}

#[test]
fn multi_producer_single_consumer_smoke() {
    const WORKERS: usize = 4;
    const PER_WORKER: u64 = 500;
    const RECORD_SIZE: u64 = 8;

    let pstRQ: Arc<st_RingQueue> = Arc::from(pstInit_RingQueue(WORKERS as i32, 1024).unwrap());
    let start = Arc::new(Barrier::new(WORKERS + 1));
    let mut producers = Vec::new();

    for worker in 0..WORKERS {
        let pstRQ = Arc::clone(&pstRQ);
        let start = Arc::clone(&start);
        producers.push(thread::spawn(move || {
            let pstQW = pstGet_RingQueueWorker(&pstRQ, worker as i32);
            start.wait();

            for seq in 0..PER_WORKER {
                let value = ((worker as u64) << 48) | seq;
                let bytes = value.to_le_bytes();

                loop {
                    let ullWritePos = llAcquire_RingQueue(&pstRQ, pstQW, RECORD_SIZE);
                    if ullWritePos < 0 {
                        thread::yield_now();
                        continue;
                    }

                    unsafe {
                        write_bytes(&pstRQ, ullWritePos as u64, &bytes);
                    }
                    Produce_RingQueue(pstQW);
                    break;
                }
            }

            Release_RingQueueWorker(pstQW);
        }));
    }

    // Worker registration is part of the queue setup contract. Do not begin
    // consuming until every producer has activated its dedicated worker slot.
    start.wait();

    let expected = WORKERS as u64 * PER_WORKER;
    let mut seen = HashSet::with_capacity(expected as usize);
    let deadline = Instant::now() + Duration::from_secs(30);

    while seen.len() < expected as usize {
        assert!(Instant::now() < deadline, "multi-producer smoke test timed out");
        let mut ullWriteLen = 0;
        let ullReadPos = unsafe { llConsume_RingQueue(&pstRQ, &mut ullWriteLen) };

        if ullReadPos < 0 || ullWriteLen == 0 {
            thread::yield_now();
            continue;
        }

        assert_eq!(ullWriteLen % RECORD_SIZE, 0);
        let mut offset = 0;
        while offset < ullWriteLen {
            let mut bytes = [0u8; 8];
            unsafe {
                read_bytes(&pstRQ, ullReadPos as u64 + offset, &mut bytes);
            }
            let value = u64::from_le_bytes(bytes);
            let worker = value >> 48;
            let seq = value & ((1u64 << 48) - 1);
            assert!(worker < WORKERS as u64);
            assert!(seq < PER_WORKER);
            assert!(seen.insert(value));
            offset += RECORD_SIZE;
        }

        unsafe { Release_RingQueue(&pstRQ, ullWriteLen) };
    }

    for producer in producers {
        producer.join().unwrap();
    }

    assert_eq!(seen.len(), expected as usize);
}

#[test]
fn invalid_init_arguments_are_rejected() {
    assert!(matches!(
        pstInit_RingQueue(-1, 64),
        Err(RingQueueError::InvalidArgument)
    ));
    assert!(matches!(
        pstInit_RingQueue(1, 0),
        Err(RingQueueError::InvalidArgument)
    ));
}
