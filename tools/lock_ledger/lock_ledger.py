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
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

FILE_MAGIC = b"LKL2"
CHUNK_MAGIC = b"CHNK"
FILE_VERSION = 1

# Must match lock_ledger::Entry in src/iocore/eventsystem/LockLedger.cc.
ENTRY = struct.Struct("<qQQIIIHH")
assert ENTRY.size == 40

OP_MISS, OP_WAIT, OP_RELEASE = 1, 2, 3
OP_NAMES = {OP_MISS: "MISS", OP_WAIT: "WAIT", OP_RELEASE: "RELEASE"}

FILE_RE = re.compile(r"lockledger\.(\d+)\.(\d+)\.bin$")


@dataclass(slots=True)
class Entry:
    timestamp: int
    mutex: int
    context: int
    site: int
    other_site: int
    duration: int
    op: int
    other_thread: int
    thread: int


@dataclass
class Ledger:
    sites: dict[int, tuple[str, str, int]] = field(default_factory=dict)
    mutex_names: dict[int, str] = field(default_factory=dict)
    threads: dict[int, str] = field(default_factory=dict)
    entries: list[Entry] = field(default_factory=list)
    dropped_chunks: int = 0

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


def read_str(buf: bytes, off: int) -> tuple[str, int]:
    (n,) = struct.unpack_from("<H", buf, off)
    off += 2
    return buf[off : off + n].decode("utf-8", "replace"), off + n


def parse_defs(ledger: Ledger, defs: bytes) -> None:
    off = 0
    while off < len(defs):
        tag = defs[off : off + 1]
        off += 1
        if tag == b"S":
            site_id, line = struct.unpack_from("<II", defs, off)
            off += 8
            file, off = read_str(defs, off)
            func, off = read_str(defs, off)
            ledger.sites[site_id] = (file, func, line)
        elif tag == b"M":
            (addr,) = struct.unpack_from("<Q", defs, off)
            off += 8
            name, off = read_str(defs, off)
            ledger.mutex_names[addr] = name
        else:
            raise ValueError(f"unknown definition tag {tag!r} at offset {off - 1}")


def parse_file(ledger: Ledger, path: Path) -> None:
    data = path.read_bytes()
    if data[:4] != FILE_MAGIC:
        raise ValueError(f"{path}: not a lock ledger file (magic {data[:4]!r})")
    version, _pid, tid = struct.unpack_from("<III", data, 4)
    if version != FILE_VERSION:
        raise ValueError(f"{path}: unsupported version {version}")
    name, off = read_str(data, 16)
    ledger.threads[tid] = name or f"thread {tid}"
    while off + 16 <= len(data):
        if data[off : off + 4] != CHUNK_MAGIC:
            raise ValueError(f"{path}: bad chunk magic at offset {off}")
        defs_len, count, dropped = struct.unpack_from("<III", data, off + 4)
        off += 16
        end = off + defs_len + count * ENTRY.size
        if end > len(data):
            print(f"warning: {path}: truncated chunk at offset {off - 16}, ignoring the rest", file=sys.stderr)
            break
        parse_defs(ledger, data[off : off + defs_len])
        off += defs_len
        for fields in ENTRY.iter_unpack(data[off : off + count * ENTRY.size]):
            ledger.entries.append(Entry(*fields, thread=tid))
        off += count * ENTRY.size
        ledger.dropped_chunks = max(ledger.dropped_chunks, dropped)


def load(directory: Path, pid: int | None) -> Ledger:
    files: dict[int, list[Path]] = defaultdict(list)
    for p in directory.iterdir():
        if m := FILE_RE.search(p.name):
            files[int(m.group(1))].append(p)
    if not files:
        sys.exit(f"no lockledger.<pid>.<thread>.bin files in {directory}")
    if pid is None:
        pid = max(files, key=lambda k: max(f.stat().st_mtime for f in files[k]))
    elif pid not in files:
        sys.exit(f"no ledger files for pid {pid} in {directory} (have {sorted(files)})")
    ledger = Ledger()
    for p in sorted(files[pid]):
        parse_file(ledger, p)
    ledger.entries.sort(key=lambda e: e.timestamp)
    print(f"pid {pid}: {len(files[pid])} thread files, {len(ledger.entries):,} entries", file=sys.stderr)
    if ledger.dropped_chunks:
        print(f"warning: the writer dropped {ledger.dropped_chunks} chunks; counts are lower bounds", file=sys.stderr)
    return ledger


def pct(values: list[int], q: float) -> int:
    if not values:
        return 0
    s = sorted(values)
    return s[min(len(s) - 1, int(q * len(s)))]


def us(ns: int) -> str:
    return f"{ns / 1000:.1f}"


def filtered(ledger: Ledger, name: str | None) -> list[Entry]:
    if name is None:
        return ledger.entries
    return [e for e in ledger.entries if ledger.mutex_names.get(e.mutex) == name]


def summary(ledger: Ledger, args: argparse.Namespace) -> None:
    entries = filtered(ledger, args.name)
    if not entries:
        sys.exit("no entries match")
    span_ns = entries[-1].timestamp - entries[0].timestamp
    top = args.top

    releases = [e for e in entries if e.op == OP_RELEASE]
    misses = [e for e in entries if e.op == OP_MISS]
    waits = [e for e in entries if e.op == OP_WAIT]
    print(f"span {span_ns / 1e9:.3f} s; {len(releases):,} holds, {len(misses):,} try-lock misses, {len(waits):,} blocking waits")
    if releases:
        print(f"miss rate: {len(misses) / (len(releases) + len(misses)):.2%} of try/lock attempts")

    per_mutex: dict[int, list[int]] = defaultdict(lambda: [0, 0, 0, 0])  # holds, hold_ns, misses, wait_ns
    for e in entries:
        row = per_mutex[e.mutex]
        if e.op == OP_RELEASE:
            row[0] += 1
            row[1] += e.duration
        elif e.op == OP_MISS:
            row[2] += 1
        else:
            row[3] += e.duration
    print(f"\n== Mutexes by misses (top {top}) ==")
    print(f"{'mutex':<28} {'holds':>10} {'held %':>7} {'misses':>8} {'wait ms':>9}")
    for addr, (holds, hold_ns, nmiss, wait_ns) in sorted(per_mutex.items(), key=lambda kv: (-kv[1][2], -kv[1][1]))[:top]:
        held = hold_ns / span_ns if span_ns else 0
        print(f"{ledger.mutex(addr):<28} {holds:>10,} {held:>7.1%} {nmiss:>8,} {wait_ns / 1e6:>9.2f}")

    holds_by_site: dict[int, list[int]] = defaultdict(list)
    for e in releases:
        holds_by_site[e.site].append(e.duration)
    print(f"\n== Hold time by acquiring site, µs (top {top} by total) ==")
    print(f"{'count':>10} {'total ms':>9} {'p50':>8} {'p99':>8} {'max':>9}  site")
    for site, durs in sorted(holds_by_site.items(), key=lambda kv: -sum(kv[1]))[:top]:
        print(
            f"{len(durs):>10,} {sum(durs) / 1e6:>9.2f} {us(pct(durs, 0.5)):>8} {us(pct(durs, 0.99)):>8} {us(max(durs)):>9}  {ledger.site(site)}"
        )

    if misses:
        print(f"\n== Try-lock misses: waiter site <- holder site (top {top}) ==")
        pairs = Counter((e.site, e.other_site) for e in misses)
        for (waiter, holder), n in pairs.most_common(top):
            print(f"{n:>8,}  {ledger.site(waiter)}\n          <- {ledger.site(holder)}")
        print(f"\n== Try-lock misses by thread pair (top {top}) ==")
        threads = Counter((e.thread, e.other_thread) for e in misses)
        for (waiter, holder), n in threads.most_common(top):
            print(f"{n:>8,}  {ledger.thread(waiter)} <- {ledger.thread(holder)}")

    if waits:
        print(f"\n== Blocking waits: waiter site <- holder site (top {top} by total wait) ==")
        print(f"{'count':>8} {'total ms':>9} {'max µs':>9}")
        wait_pairs: dict[tuple[int, int], list[int]] = defaultdict(list)
        for e in waits:
            wait_pairs[(e.site, e.other_site)].append(e.duration)
        for (waiter, holder), durs in sorted(wait_pairs.items(), key=lambda kv: -sum(kv[1]))[:top]:
            print(
                f"{len(durs):>8,} {sum(durs) / 1e6:>9.2f} {us(max(durs)):>9}  {ledger.site(waiter)}\n"
                f"{'':>29}<- {ledger.site(holder)}"
            )


def timeline(ledger: Ledger, args: argparse.Namespace) -> None:
    if not ledger.entries:
        sys.exit("no entries")
    t0 = ledger.entries[0].timestamp
    mutex = int(args.mutex, 0)
    start = t0 + int(args.start_ms * 1e6) if args.start_ms is not None else None
    end = t0 + int(args.end_ms * 1e6) if args.end_ms is not None else None
    for e in ledger.entries:
        if e.mutex != mutex or (start is not None and e.timestamp < start) or (end is not None and e.timestamp > end):
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
    for e in filtered(ledger, args.name):
        w.writerow(
            [
                e.timestamp,
                ledger.thread(e.thread),
                OP_NAMES[e.op],
                f"{e.mutex:#x}",
                ledger.mutex_names.get(e.mutex, ""),
                ledger.site(e.site),
                e.duration,
                ledger.thread(e.other_thread) if e.op != OP_RELEASE else "",
                ledger.site(e.other_site) if e.op != OP_RELEASE else "",
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
    t.add_argument("--start-ms", type=float, help="offset from the first entry")
    t.add_argument("--end-ms", type=float)
    c = sub.add_parser("csv", help="all entries with names resolved")
    c.add_argument("--name", help="only mutexes with this ledger name")
    args = parser.parse_args()

    ledger = load(args.dir, args.pid)
    {"summary": summary, "timeline": timeline, "csv": dump_csv}[args.cmd](ledger, args)


if __name__ == "__main__":
    main()
