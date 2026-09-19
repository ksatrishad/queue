# nonblock-ring-queue

Rust port of the supplied C `queue.c` / `libthread.h` ring queue.

The port intentionally retains the original C names (`st_RingQueue`,
`st_QueueWorker`, `pstInit_RingQueue`, `llAcquire_RingQueue`,
`Produce_RingQueue`, `llConsume_RingQueue`, `Release_RingQueue`) so the two
implementations can be reviewed side by side.

## C -> Rust representation

| C | Rust |
| --- | --- |
| `_Atomic unsigned long long ullOffReady` | `AtomicU64 ullOffReady` |
| `_Atomic int isActive` | `AtomicI32 isActive` |
| `unsigned long long ullBufLen` | `u64 ullBufLen` |
| `char *pszBuf` | `NonNull<u8> pszBuf` + matching `Drop` |
| `_Atomic unsigned long long ullWritePos` | `AtomicU64 ullWritePos` |
| `unsigned long long ullWrapLimit` | `UnsafeCell<u64> ullWrapLimit` |
| `_Atomic unsigned long long ullReadPos` | `AtomicU64 ullReadPos` |
| `int dNumWorker` | `i32 dNumWorker` |
| `st_QueueWorker stWorker[]` | `Box<[st_QueueWorker]> stWorker` |

`stWorker[]` is a C flexible-array member. Rust owns it as a boxed slice rather
than reproducing the C descriptor allocation layout. `pszBuf` uses an uninitialized
raw allocation, matching C `malloc` more closely and avoiding an unnecessary full
buffer zero-fill during queue creation

`ullWrapLimit` remains non-atomic to preserve the source structure. The wrapping
producer writes it while `ullWritePos` carries `WRAP_LOCK_BIT`, then clears that
bit with a Release store. The consumer waits in `STABLE_CALL` and observes
`ullWritePos` with Acquire before reading the limit.

Two intentional memory-order strengthenings exist. C stores `ullReadPos` with
`memory_order_relaxed` in `Release_RingQueue`; Rust uses `Ordering::Release` so
consumer payload reads happen-before a producer that sees the advanced read
position with Acquire and reuses the same storage. C also deactivates workers and
checks `isActive` with Relaxed operations. Rust uses Release on worker deactivation
and Acquire in the consumer so skipping an inactive worker still observes payload
writes completed before that worker was released.

The queue contract remains multi-producer / single-consumer. Each worker must be
owned by at most one producer at a time.

## Rocky Linux 9 setup

From the project directory:

```bash
./scripts/bootstrap-rocky9.sh
source "$HOME/.cargo/env"
./scripts/verify.sh
```

The bootstrap installs the native GNU linker/toolchain with `dnf`, then installs
Rust through `rustup`. `rust-toolchain.toml` pins Rust 1.98.1, the minimal profile,
`rustfmt`, `clippy`, and the `x86_64-unknown-linux-gnu` target. `Cargo.toml`
uses the matching Rust 1.98 MSRV instead of claiming an unverified older compiler. he bootstrap does
not change the user's global rustup default toolchain.

If Rust is already installed through rustup, the bootstrap is idempotent and only
ensures the requested components/target are present. `verify.sh` invokes Cargo via
`rustup run`, so a distro-provided `/usr/bin/cargo` cannot accidentally bypass the
pinned toolchain or hide an installed `cargo-clippy`.

## Normal builds

```bash
cargo build
cargo test --all-targets
cargo build --release
```

or:

```bash
make check
make release
```

Release output is under `target/release/`.

## Payload access

The original API returns offsets into `pszBuf`; therefore this direct port exposes
`st_RingQueue::pszBuf_ptr()` as `unsafe`. The caller must keep the original
protocol:

```text
producer:
    llAcquire_RingQueue
    -> write only the returned reservation
    -> Produce_RingQueue

consumer (single logical consumer only):
    unsafe { llConsume_RingQueue(...) }
    -> read only the returned range
    -> unsafe { Release_RingQueue(...) }
```

The integration tests show the intended usage, including wrap-around and a
4-producer / 1-consumer smoke test.

## Optional Rocky 9 container verification

A `Containerfile.rocky9` is included for a clean Rocky 9-family verification:

```bash
podman build -f Containerfile.rocky9 -t nonblock-ring-queue-rocky9 .
```

If an exact `rockylinux:9.0` image tag is available in your registry, use:

```bash
podman build \
  --build-arg ROCKY_TAG=9.0 \
  -f Containerfile.rocky9 \
  -t nonblock-ring-queue-rocky9.0 .
```

The x86_64 GNU target uses `gcc` explicitly as the linker through
`.cargo/config.toml`, matching the Rocky 9 GNU userspace/toolchain setup.
