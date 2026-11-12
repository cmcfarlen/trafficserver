# Lock ledger

The lock ledger records `ProxyMutex` contention in a running `traffic_server`: who held a mutex, for how long,
and which call site failed to get it while someone else held it. It is a diagnostic build option, off by default.

## Building

```
cmake -B build -DENABLE_LOCK_LEDGER=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo ...
```

The option also turns on lock call-site tracking (`SourceLocation` on every lock macro), which debug builds
already have. In a release build, only recorded mutexes pay for timestamps.

## What is recorded

Only mutexes opted in with `LOCK_LEDGER_NAME(mutex, "name")` are recorded. The cache stripe mutex is opted in as
`stripe`. To record every `ProxyMutex`, which is a lot of data and slows the server noticeably, set
`TS_LOCK_LEDGER=all` in the environment of `traffic_server`. `TS_LOCK_LEDGER=off` records nothing.

Each thread writes `lockledger.<pid>.<thread>.bin` to `$TS_LOCK_LEDGER_DIR` (default `/tmp`). A background thread
does the writes; data reaches disk about once a second per thread, and up to the last second of each event
thread is lost when the process exits.

Three kinds of entries are written:

| Op | Written when | `site` | `other_*` | `duration` |
|---|---|---|---|---|
| `MISS` | a try-lock fails | the waiter's call site | the holder at that moment | — |
| `WAIT` | a blocking lock had to wait | the waiter's call site | the holder when the wait began | wait, ns |
| `RELEASE` | the outermost unlock | where the lock was acquired | — | hold, ns |

Every entry also carries the dispatching context: the continuation an event was delivered to, or the net event
being serviced. For cache reads that is usually the `CacheVC`, so it separates concurrent readers.

The holder recorded on a miss is read without synchronisation from the mutex as the miss happens; it can be one
acquisition stale.

## Analysing

```
tools/lock_ledger/lock_ledger.py --dir /tmp summary --name stripe
tools/lock_ledger/lock_ledger.py --dir /tmp timeline --mutex 0x600003e1c000 --start-ms 1800 --end-ms 1850
tools/lock_ledger/lock_ledger.py --dir /tmp csv --name stripe > stripe.csv
```

`summary` lists the most-missed mutexes, hold-time percentiles by acquiring site, and which waiter sites missed
behind which holder sites and threads.

## File format

All integers are little-endian. A file starts with

```
char[4] "LKL2"; u32 version (1); u32 pid; u32 thread id; u16 length; char[] thread name
```

followed by chunks:

```
char[4] "CHNK"; u32 definitions length; u32 entry count; u32 chunks dropped so far (whole process)
definitions; entries
```

Definitions are tagged records: `'S' u32 site id, u32 line, str file, str function` and
`'M' u64 mutex address, str name`, where `str` is a `u16` length and bytes. Site and thread ids are global to
the process, so a `MISS` in one file can refer to a holder site defined in another; load all files of a pid
together.

Each entry is 40 bytes: `i64 timestamp_ns, u64 mutex, u64 context, u32 site, u32 other_site, u32 duration_ns,
u16 op, u16 other_thread`.
