#!/usr/bin/env python3
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""
Summarize lock ledger files written by a traffic_server built with ENABLE_LOCK_LEDGER.

Usage:
    lock_ledger.py summary [--dir /tmp] [--pid N] [--name stripe] [--top 15]
    lock_ledger.py timeline --mutex 0x... [--start-ms A] [--end-ms B]
    lock_ledger.py csv > entries.csv

Entries are streamed, so a run of hundreds of millions of entries fits in memory; percentiles come
from log-scale histograms and are accurate to about 10%.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import struct
import sys
from collections import Counter, defaultdict
from collections.abc import Iterator
from pathlib import Path
from typing import NamedTuple

FILE_MAGIC = b"LKL2"
CHUNK_MAGIC = b"CHNK"
FILE_VERSION = 1

# Must match lock_ledger::Entry in src/iocore/eventsystem/LockLedger.cc.
ENTRY = struct.Struct("<qQQIIIHH")
assert ENTRY.size == 40

OP_MISS, OP_WAIT, OP_RELEASE = 1, 2, 3
OP_NAMES = {OP_MISS: "MISS", OP_WAIT: "WAIT", OP_RELEASE: "RELEASE"}

FILE_RE = re.compile(r"lockledger\.(\d+)\.(\d+)\.bin$")


class Entry(NamedTuple):
    timestamp: int
    mutex: int
    context: int
    site: int
    other_site: int
    duration: int
    op: int
    other_thread: int
    thread: int


class Ledger:
    """Definitions from every file of one process; entries are read on demand with entries()."""

    def __init__(self, files: list[Path]) -> None:
        self.files = files
        self.sites: dict[int, tuple[str, str, int]] = {}
        self.mutex_names: dict[int, str] = {}
        self.threads: dict[int, str] = {}
        self.dropped_chunks = 0
        for path in files:
            for _ in self._chunks(path, defs_only=True):
                pass

    def site(self, site_id: int) -> str:
        if site_id == 0:
            return "(unknown)"
        file, func, line = self.sites.get(site_id, ("?", "?", 0))
        return f"{os.path.basename(file)}:{line} ({func})"

    def thread(self, tid: int) -> str:
        if tid == 0:
            return "(unknown)"
        return self.threads.get(tid, f"thread {tid}")

    def mutex(self, addr: int) -> str:
        name = self.mutex_names.get(addr)
        return f"{name}@{addr:#x}" if name else f"{addr:#x}"

    def entries(self, name: str | None = None) -> Iterator[Entry]:
        """Entries file by file, each file in time order (not globally sorted)."""
        for path in self.files:
            for tid, data in self._chunks(path, defs_only=False):
                for fields in ENTRY.iter_unpack(data):
                    if name is None or self.mutex_names.get(fields[1]) == name:
                        yield Entry(*fields, tid)

    def _chunks(self, path: Path, defs_only: bool) -> Iterator[tuple[int, memoryview]]:
        data = memoryview(path.read_bytes())
        if bytes(data[:4]) != FILE_MAGIC:
            raise ValueError(f"{path}: not a lock ledger file")
        version, _pid, tid = struct.unpack_from("<III", data, 4)
        if version != FILE_VERSION:
            raise ValueError(f"{path}: unsupported version {version}")
        name, off = read_str(data, 16)
        self.threads[tid] = name or f"thread {tid}"
        while off + 16 <= len(data):
            if bytes(data[off : off + 4]) != CHUNK_MAGIC:
                raise ValueError(f"{path}: bad chunk magic at offset {off}")
            defs_len, count, dropped = struct.unpack_from("<III", data, off + 4)
            off += 16
            end = off + defs_len + count * ENTRY.size
            if end > len(data):
                print(f"warning: {path}: truncated chunk at offset {off - 16}, ignoring the rest", file=sys.stderr)
                break
            if defs_only:
                self._parse_defs(data[off : off + defs_len])
                self.dropped_chunks = max(self.dropped_chunks, dropped)
            else:
                yield tid, data[off + defs_len : end]
            off = end

    def _parse_defs(self, defs: memoryview) -> None:
        off = 0
        while off < len(defs):
            tag = bytes(defs[off : off + 1])
            off += 1
            if tag == b"S":
                site_id, line = struct.unpack_from("<II", defs, off)
                off += 8
                file, off = read_str(defs, off)
                func, off = read_str(defs, off)
                self.sites[site_id] = (file, func, line)
            elif tag == b"M":
                (addr,) = struct.unpack_from("<Q", defs, off)
                off += 8
                name, off = read_str(defs, off)
                self.mutex_names[addr] = name
            else:
                raise ValueError(f"unknown definition tag {tag!r} at offset {off - 1}")


def read_str(buf: memoryview, off: int) -> tuple[str, int]:
    (n,) = struct.unpack_from("<H", buf, off)
    off += 2
    return bytes(buf[off : off + n]).decode("utf-8", "replace"), off + n


def load(directory: Path, pid: int | None) -> Ledger:
    files: dict[int, list[Path]] = defaultdict(list)
    for p in directory.iterdir():
        m = FILE_RE.search(p.name)
        if m:
            files[int(m.group(1))].append(p)
    if not files:
        sys.exit(f"no lockledger.<pid>.<thread>.bin files in {directory}")
    if pid is None:
        pid = max(files, key=lambda k: max(f.stat().st_mtime for f in files[k]))
    elif pid not in files:
        sys.exit(f"no ledger files for pid {pid} in {directory} (have {sorted(files)})")
    ledger = Ledger(sorted(files[pid]))
    size = sum(f.stat().st_size for f in files[pid])
    print(f"pid {pid}: {len(files[pid])} thread files, {size / 1e6:,.0f} MB", file=sys.stderr)
    if ledger.dropped_chunks:
        print(f"warning: the writer dropped {ledger.dropped_chunks} chunks; counts are lower bounds", file=sys.stderr)
    return ledger


class Histogram:
    """Durations in log-scale buckets (8 per doubling) for approximate percentiles over any count."""

    PER_OCTAVE = 8

    def __init__(self) -> None:
        self.buckets: Counter[int] = Counter()
        self.count = 0
        self.total = 0
        self.max = 0

    def add(self, ns: int) -> None:
        self.buckets[int(math.log2(ns) * self.PER_OCTAVE) if ns > 0 else -1] += 1
        self.count += 1
        self.total += ns
        self.max = max(self.max, ns)

    def pct(self, q: float) -> float:
        target = q * self.count
        seen = 0
        for b in sorted(self.buckets):
            seen += self.buckets[b]
            if seen >= target:
                return 0.0 if b < 0 else 2 ** ((b + 0.5) / self.PER_OCTAVE)
        return float(self.max)


def us(ns: float) -> str:
    return f"{ns / 1000:.1f}"


def summary(ledger: Ledger, args: argparse.Namespace) -> None:
    top = args.top
    first = last = None
    per_mutex: dict[int, list[int]] = defaultdict(lambda: [0, 0, 0, 0])  # holds, hold_ns, misses, wait_ns
    holds: dict[int, Histogram] = defaultdict(Histogram)
    miss_pairs: Counter[tuple[int, int]] = Counter()
    miss_threads: Counter[tuple[int, int]] = Counter()
    waits: dict[tuple[int, int], Histogram] = defaultdict(Histogram)
    n_release = n_miss = n_wait = 0

    for e in ledger.entries(args.name):
        ts = e.timestamp
        if first is None or ts < first:
            first = ts
        if last is None or ts > last:
            last = ts
        row = per_mutex[e.mutex]
        if e.op == OP_RELEASE:
            n_release += 1
            row[0] += 1
            row[1] += e.duration
            holds[e.site].add(e.duration)
        elif e.op == OP_MISS:
            n_miss += 1
            row[2] += 1
            miss_pairs[(e.site, e.other_site)] += 1
            miss_threads[(e.thread, e.other_thread)] += 1
        else:
            n_wait += 1
            row[3] += e.duration
            waits[(e.site, e.other_site)].add(e.duration)

    if first is None:
        sys.exit("no entries match")
    span_ns = last - first
    print(f"span {span_ns / 1e9:.3f} s; {n_release:,} holds, {n_miss:,} try-lock misses, {n_wait:,} blocking waits")
    if n_release:
        print(f"miss rate: {n_miss / (n_release + n_miss):.3%} of try/lock attempts")

    print(f"\n== Mutexes by misses (top {top}) ==")
    print(f"{'mutex':<28} {'holds':>12} {'held %':>7} {'misses':>10} {'wait ms':>9}")
    for addr, (nh, hold_ns, nm, wait_ns) in sorted(per_mutex.items(), key=lambda kv: (-kv[1][2], -kv[1][1]))[:top]:
        held = hold_ns / span_ns if span_ns else 0
        print(f"{ledger.mutex(addr):<28} {nh:>12,} {held:>7.1%} {nm:>10,} {wait_ns / 1e6:>9.2f}")

    print(f"\n== Hold time by acquiring site, µs (top {top} by total) ==")
    print(f"{'count':>12} {'total ms':>10} {'p50':>8} {'p99':>8} {'max':>9}  site")
    for site, h in sorted(holds.items(), key=lambda kv: -kv[1].total)[:top]:
        print(f"{h.count:>12,} {h.total / 1e6:>10.2f} {us(h.pct(0.5)):>8} {us(h.pct(0.99)):>8} {us(h.max):>9}  {ledger.site(site)}")

    if miss_pairs:
        print(f"\n== Try-lock misses: waiter site <- holder site (top {top}) ==")
        for (waiter, holder), n in miss_pairs.most_common(top):
            print(f"{n:>10,}  {ledger.site(waiter)}\n            <- {ledger.site(holder)}")
        print(f"\n== Try-lock misses by thread pair (top {top}) ==")
        for (waiter, holder), n in miss_threads.most_common(top):
            print(f"{n:>10,}  {ledger.thread(waiter)} <- {ledger.thread(holder)}")

    if waits:
        print(f"\n== Blocking waits: waiter site <- holder site (top {top} by total wait) ==")
        print(f"{'count':>10} {'total ms':>9} {'max µs':>9}")
        for (waiter, holder), h in sorted(waits.items(), key=lambda kv: -kv[1].total)[:top]:
            print(f"{h.count:>10,} {h.total / 1e6:>9.2f} {us(h.max):>9}  {ledger.site(waiter)}\n{'':>31}<- {ledger.site(holder)}")


def timeline(ledger: Ledger, args: argparse.Namespace) -> None:
    mutex = int(args.mutex, 0)
    picked = sorted((e for e in ledger.entries() if e.mutex == mutex), key=lambda e: e.timestamp)
    if not picked:
        sys.exit("no entries for that mutex")
    t0 = picked[0].timestamp
    start = t0 + int(args.start_ms * 1e6) if args.start_ms is not None else None
    end = t0 + int(args.end_ms * 1e6) if args.end_ms is not None else None
    for e in picked:
        if (start is not None and e.timestamp < start) or (end is not None and e.timestamp > end):
            continue
        t_ms = (e.timestamp - t0) / 1e6
        line = f"{t_ms:12.3f} ms  {ledger.thread(e.thread):<14} {OP_NAMES[e.op]:<7}"
        if e.op == OP_RELEASE:
            line += f" held {us(e.duration):>9} µs  {ledger.site(e.site)}"
        else:
            if e.op == OP_WAIT:
                line += f" waited {us(e.duration):>7} µs"
            line += f"  {ledger.site(e.site)}  <- {ledger.thread(e.other_thread)} {ledger.site(e.other_site)}"
        print(f"{line}  ctx={e.context:#x}")


def dump_csv(ledger: Ledger, args: argparse.Namespace) -> None:
    w = csv.writer(sys.stdout)
    w.writerow(
        ["timestamp_ns", "thread", "op", "mutex", "mutex_name", "site", "duration_ns", "other_thread", "other_site", "context"]
    )
    for e in ledger.entries(args.name):
        other = e.op != OP_RELEASE
        w.writerow(
            [
                e.timestamp,
                ledger.thread(e.thread),
                OP_NAMES[e.op],
                f"{e.mutex:#x}",
                ledger.mutex_names.get(e.mutex, ""),
                ledger.site(e.site),
                e.duration,
                ledger.thread(e.other_thread) if other else "",
                ledger.site(e.other_site) if other else "",
                f"{e.context:#x}",
            ]
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dir", type=Path, default=Path("/tmp"), help="directory holding the ledger files")
    parser.add_argument("--pid", type=int, help="traffic_server pid (default: the most recently written)")
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary", help="contention report")
    s.add_argument("--name", help="only mutexes with this ledger name, e.g. stripe")
    s.add_argument("--top", type=int, default=15)
    t = sub.add_parser("timeline", help="every event on one mutex, in time order")
    t.add_argument("--mutex", required=True, help="mutex address, e.g. 0x600003e1c000")
    t.add_argument("--start-ms", type=float, help="offset from the mutex's first entry")
    t.add_argument("--end-ms", type=float)
    c = sub.add_parser("csv", help="all entries with names resolved, grouped by thread")
    c.add_argument("--name", help="only mutexes with this ledger name")
    args = parser.parse_args()

    ledger = load(args.dir, args.pid)
    {"summary": summary, "timeline": timeline, "csv": dump_csv}[args.cmd](ledger, args)


if __name__ == "__main__":
    main()
