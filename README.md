# Distributed KV Store

A distributed key-value store built from scratch in C: a custom wire
protocol over raw POSIX sockets, an LSM-tree storage engine (memtable +
SSTables + compaction), pthread-based concurrent client handling, TTL
expiry, and single-primary/single-replica replication over the same
custom protocol.

## What this is

- **Custom binary wire protocol** — fixed 7-byte frame header (magic,
  opcode, length, flags) over TCP, hand-parsed with explicit handling of
  partial/fragmented reads (a frame split across `recv()` calls, or
  multiple frames landing in one `recv()` call). 
- **LSM-tree storage engine** — in-memory memtable, immutable sorted
  SSTable files, background compaction. Chosen over a disk-backed B-tree
  specifically because its failure modes are contained (a compaction bug
  degrades read performance; it doesn't corrupt the on-disk structure the
  way a torn B-tree page split can) 
- **GET / SET / DELETE**, plus a range scan that exercises the LSM
  index's sorted iteration.
- **Real concurrency** — pthreads per client connection, real locking
  around the index/storage layer.
- **Crash-safe persistence** tied to the LSM design (WAL + SSTables), with
  explicit crash-recovery tests.
- **TTL/expiry** — lazy (checked on read) and an active background sweep.
- **Leader-follower replication** — one primary, one replica, write
  propagation over the same wire protocol, plus honest handling of
  replica lag and disconnect/resync
- **A minimal CLI client** speaking the same protocol.


## Stack

- Language: C11
- Networking: raw POSIX sockets (`sys/socket.h`), no framework
- Concurrency: pthreads, real mutex/rwlock usage around shared state
- Build: Makefile (`gcc`, `-Wall -Wextra -Werror -std=c11 -pthread`)
- Memory safety: `valgrind --leak-check=full` (or ASan/UBSan as a
  secondary check) as part of "done" for any stage touching allocation
- Test harness: a small header-only C assertion framework
  ([`tests/test_framework.h`](tests/test_framework.h)) plus one test
  binary per stage — chosen over Unity/Check to avoid pulling in an
  external dependency for what a ~40-line header covers

## Environment
 Build and test everything inside **WSL2 Ubuntu**:

```powershell
wsl --install -d Ubuntu
```

Inside the Ubuntu shell:

```bash
sudo apt update
sudo apt install -y build-essential valgrind
```

(`/mnt/d/Riya/PROJECTS/C_kv_store`) mounted into WSL — the Makefile and
tests don't care which side authored the files, only that `gcc`/`make`/
`valgrind` run inside the Linux (WSL) shell, not Git Bash/PowerShell.

## Getting started

```bash
cd /mnt/d/Riya/PROJECTS/C_kv_store   # from inside WSL
make test-p1                          # build + run every phase 1 stage test
make valgrind-p1                      # same, under valgrind --leak-check=full
```

## Benchmarks

Real ops/sec, latency-under-load, compaction overhead, and replication
lag numbers get logged as they're actually measured in [`docs/metrics.md`](docs/metrics.md).
