# Hemu

**Hemu: A Scalable In-Memory Key-Value Cache for Modern Storages** (SIGMOD 2027)

Hemu is a caching system designed for modern memory architectures (DRAM + CXL memory / PMem) that delivers high throughput, scalability, memory efficiency, and fast recovery. It is implemented as a **header-only C++ library** with no external dependencies beyond `libpmem` and `libnuma`.

## Key Features

- **Hash index + log-structured storage layout** — 64-byte cache-line-aligned buckets with chaining; items are sequentially appended into pages for O(1) allocation and zero external fragmentation.
- **Tiered memory management (DRAM + CXL)** — the cache space is partitioned into uniform 4 MB pages: DRAM-resident pages (*DPages*) hold hot data, CXL-resident pages (*EPages*) provide large persistent capacity. Pages migrate transparently between tiers without hash-table updates.
- **Dynamic DRAM balancer** — a latency-based feedback loop periodically probes CXL load latency and adjusts the DRAM proportion (±1% every 100 ms, clamped to 0–50%) to relieve CXL bandwidth pressure, without relying on privileged hardware counters.
- **Adaptive Cache Recycling (ACR)** — during page reclamation, ACR combines page-level hotness (access density over a sliding window of evicted pages, mapped through a sigmoid) with item-level hotness (access count / size) to decide *how many* and *which* items to retain in DRAM.
- **Cacheline-granularity locking** — per-page arrays of 1-bit locks allow fine-grained concurrent reads/writes while page-level operations (migration, reclamation) acquire all locks of a page.
- **Crash consistency & fast recovery** — page-granular consistency: only *archived* pages are recovered after a reboot; the volatile hash table and page table are rebuilt by scanning archived EPages in parallel. This preserves pre-failure hit ratios and eliminates the cold-start problem of traditional volatile caches.

## Architecture

```
┌─────────────────────────── DRAM ───────────────────────────┐
│  Hash Index   Tiered Memory Mgr   DRAM Balancer   ACR      │
│  DPages (hot items)    Page Table    Guard Thread          │
└────────────────────────────┬───────────────────────────────┘
                             │ page migration / reclamation
┌────────────────────────────▼───────────────────────────────┐
│              CXL Memory / PMem: EPages (persistent KV)     │
└─────────────────────────────────────────────────────────────┘
```

## Requirements

- Linux with a CXL memory NUMA node (or a PMem/DAX device; can also run in pure DRAM/FILE mode for testing)
- GCC 12+ with C++20 support (`-std=c++2a`)
- [PMDK](https://github.com/pmem/pmdk) (`libpmem`), `libnuma`

## Quick Start

Hemu is used as an embedded library via in-process function calls (`set`, `add`, `get`, `delete`) — no network overhead. Configure `Hemu_Cache::config` **before** constructing the `Hemu` instance:

```cpp
#include "Hemu.h"

int main() {
  Hemu_Cache::config.esm_allocator_type = Hemu_Cache::AllocatorType::NUMA; // NUMA (CXL RAM mode) or FILE (DAX/PMem)
  Hemu_Cache::config.eSM_NODE = 1;              // CXL NUMA node id (NUMA mode only)
  Hemu_Cache::config.CacheCapacity = 512 * 1024 * 1024ULL;
  Hemu_Cache::config.CacheStrategy = 1;         // enable ACR
  Hemu_Cache::config.DRAM_PERCENT = 0;          // initial DRAM proportion
  Hemu_Cache::config.FILE_POOL_DIR = "/mnt/cxlpool"; // DAX pool dir (FILE mode) / recovery dir

  Hemu_Cache::Hemu hemu;

  char key[] = "hello", val[] = "world";
  hemu.set(key, strlen(key), val, strlen(val), /*ttl=*/3600);

  char buf[4096]; size_t vlen = 0;
  hemu.get(key, strlen(key), buf, vlen);
  return 0;
}
```

Build the playground example:

```bash
cd Hemu && make        # produces ./Hemu from main.cpp
```

### Configuration Options

| Option | Default | Description |
|---|---|---|
| `CacheCapacity` | — (required) | Total cache capacity in bytes |
| `Ini_Hashtable_Capacity` | 16M | Initial hash-table bucket count |
| `esm_allocator_type` | `NUMA` | `NUMA` (CXL RAM mode), `FILE` (DAX/PMem), or `DRAM` (testing) |
| `eSM_NODE` | 1 | NUMA node of the CXL memory |
| `FILE_POOL_DIR` | — | DAX pool directory; also scanned for recovery on startup |
| `Balancer_ON` | true | Enable the dynamic DRAM balancer |
| `Balancer_Interval` | 100 ms | Balancer probing interval |
| `CacheStrategy` | 1 | Enable (1) / disable (0) ACR hot-item retention |
| `DRAM_PERCENT` | 0.0 | Initial DRAM proportion (0–0.5) |
| `ACR_WINDOW_SIZE` | 64 | ACR sliding window size (n evicted pages) |
| `PAGE_SIZE` | 4 MB | Page size (configurable at initialization) |
| `Access_Granularity` | 64 | Locking granularity in bytes |

Options can also be passed as `Hemu.key=value` tokens through the benchmark CLI.

## Benchmarks

The `benchmark/` directory contains a unified benchmark harness comparing Hemu against CacheLib, Redis, Memcached, Segcache, PaperCache, Viper, and Plush on Twitter production traces and YCSB workloads.

```bash
cd benchmark && make           # builds ./benchmark (requires third-party systems in third/)

# Run via the unified script:
cd scripts
./run.sh throughput            # multi-cache throughput (Twitter traces + YCSB)
./run.sh dram-sweep            # DRAM percentage sweep (0 → 1.0)
./run.sh acr-sweep             # ACR sliding-window size sweep
./run.sh page-sweep            # page size sweep
./run.sh all                   # everything

# Cold-start vs. warm-recovery experiment:
./cold_start.sh [cluster|ycsb|all]
```

Workload traces are expected under `/data/` (`/data/cluster<d>` for Twitter, `/data/ycsb_run_workload<letter>` for YCSB).

Direct invocation example:

```bash
numactl -N 0 ./benchmark --cache Hemu --datafile /data/cluster3 --threads 16 \
    --capacity 400031744 --esm 2 --cxl-node 1 \
    Hemu.Balancer_ON=1 Hemu.DRAM_PERCENT=0.0
```

## Repository Layout

```
Hemu/            Header-only cache library (Hemu.h) + playground (main.cpp)
benchmark/       Unified benchmark harness for all compared systems
scripts/         Experiment runner scripts (run.sh, cold_start.sh)
third/           Third-party baselines (CacheLib, Segcache, PaperCache, Plush, viper)
Twitter/         Twitter trace tooling
YCSB/            YCSB workload tooling
rocksdb_bench/   RocksDB facilitation experiment
```

## Evaluation Highlights

Evaluated on an AMD EPYC 9335 server (32 cores) with 128 GB DDR5 and 256 GB CXL 2.0 memory (Type-3 expander over PCIe 5.0), using Twitter production traces and YCSB workloads, Hemu outperforms state-of-the-art designs across hit ratio, throughput, tail latency, and scalability, and recovers pre-failure hit ratios quickly after a crash. See the paper for details.

## Citation

```bibtex
@article{hemu2026,
  title   = {Hemu: A Scalable In-Memory Key-Value Cache for Modern Storages},
  author  = {Hu, Daokun and Chen, Zhiwen and Che, Wenkui and Sun, Jianhua and
             Chen, Hao and Xu, Quanqing and Yang, Chuanhui and Chen, Gang},
  journal = {Proceedings of the ACM on Management of Data (PACMMOD)},
  volume  = {4},
  number  = {6 (SIGMOD)},
  year    = {2026},
  doi     = {10.1145/3856350}
}
```
