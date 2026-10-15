/**
 * Benchmark: RocksDB throughput with and without Hemu cache.
 *
 * Uses real workload traces:
 *   - Twitter cluster traces  (/data/cluster<d>)
 *   - YCSB run traces          (/data/YCSB_4096_1_256/ycsb_run_workload<x>)
 *
 * Data formats (same as the main Hemu benchmark):
 *   twitter_workload: { op, timestamp, ttl, key_len:10, value_len:22, key[] }
 *   ycsb_workload:     { op:2, value_len:30, key:size_t }  (packed)
 *
 * The trace contains both insert (SET/ADD) and read (GET) operations.
 * For "rocksdb-only" mode, every op goes directly to RocksDB.
 * For "hemu+rocksdb" mode:
 *   - SET/ADD: write to RocksDB and Hemu
 *   - GET:     check Hemu first; on hit return; on miss read RocksDB
 *              and back-fill Hemu.
 *
 * Usage:
 *   ./rocksdb_bench --datafile=<name> [options]
 *
 *   --datafile=<name>      "cluster10" | "ycsba" | "ycsbb" | ...
 *   --threads=<T>          number of worker threads             (default 1)
 *   --hemu-capacity=<bytes> Hemu cache capacity in bytes          (default 2147483648)
 *   --db-path=<path>       RocksDB data directory              (default ./rocksdb_data)
 *   --mode=<mode>          "rocksdb" | "hemu" | "both"          (default both)
 *   --hemu=<0|1|2>         0=RocksDB only, 1=Hemu+RocksDB, 2=Hemu only (default: use --mode)
 *   --esm=<1|2>            Hemu eSM mode: 1=File/DAX, 2=NUMA   (default 2)
 *   --cxl-node=<N>         NUMA node for CXL memory            (default 1)
 *   --start=<0..1>         trace fraction to start from        (default 0.0)
 *   --end=<0..1>           trace fraction to end at           (default 1.0)
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/write_batch.h>

#include "Hemu.h"

// ── Workload data structures (must match benchmark.h) ───────────────

enum op_type { GET, GETS, SET, ADD, REPLACE, CAS, APPEND, PREPEND, DELETE, INCR, DECR };

struct twitter_workload {
  uint32_t op;
  uint32_t timestamp;
  uint32_t ttl;
  uint32_t key_len : 10;
  uint32_t value_len : 22;
  char* get_key_addr() { return reinterpret_cast<char*>(this) + sizeof(twitter_workload); }
};

#pragma pack(push, 1)
struct ycsb_workload {
  uint32_t op : 2;        // 0 = GET, 1 = ADD
  uint32_t value_len : 30;
  size_t key;
};
#pragma pack(pop)

// ── Args ────────────────────────────────────────────────────────────

struct Args {
  std::string datafile;
  size_t threads       = 1;
  size_t hemu_cap_bytes = 2147483648ULL;  // 2 GB default
  std::string db_path  = "./rocksdb_data";
  std::string result_path = "./result";
  std::string mode     = "both";
  int  hemu_enabled    = -1;  // -1 = use --mode, 0 = RocksDB only, 1 = Hemu+RocksDB, 2 = Hemu only
  int  esm              = 2;
  int  cxl_node         = 1;
  float start           = 0.0f;
  float end             = 1.0f;
};

static Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    auto pos = arg.find('=');
    if (pos == std::string::npos) continue;
    std::string key = arg.substr(0, pos);
    std::string val = arg.substr(pos + 1);
    if      (key == "--datafile")       a.datafile    = val;
    else if (key == "--threads")        a.threads     = std::stoull(val);
    else if (key == "--hemu-capacity")  a.hemu_cap_bytes = std::stoull(val);
    else if (key == "--db-path")        a.db_path     = val;
    else if (key == "--result-path")    a.result_path = val;
    else if (key == "--mode")           a.mode        = val;
    else if (key == "--hemu")           a.hemu_enabled= std::stoi(val);
    else if (key == "--esm")            a.esm         = std::stoi(val);
    else if (key == "--cxl-node")       a.cxl_node    = std::stoi(val);
    else if (key == "--start")          a.start       = std::stof(val);
    else if (key == "--end")            a.end         = std::stof(val);
  }
  return a;
}

// ── Workload loader (mmap-based, same as main benchmark) ────────────

class Workload {
 protected:
  int   thread_num;
  float start_frac;
  float end_frac;
  int   fd = -1;
  char* addr = nullptr;
  size_t len = 0;
  std::vector<size_t> run_data;  // byte offsets of each record

 public:
  std::string datafile;
  bool is_ycsb = false;

  Workload(std::string file, int _thread_num, float _start, float _end)
      : thread_num(_thread_num), start_frac(_start), end_frac(_end), datafile(file) {}

  virtual ~Workload() {
    if (addr) { munmap(addr, len); close(fd); }
  }

  void open_file() {
    fd = open(datafile.c_str(), O_RDONLY);
    if (fd < 0) { perror("open datafile"); exit(1); }
    len = lseek(fd, 0, SEEK_END);
    std::cout << "Data File: " << datafile << "  size: " << (len / 1e9) << " GB" << std::endl;
    addr = (char*)mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) { perror("mmap"); exit(1); }
    madvise(addr, len, MADV_HUGEPAGE | MADV_SEQUENTIAL);
  }

  size_t get_item_count() { return run_data.size(); }

  // Returns pointer to the i-th record.
  char* get_record(size_t global_index) {
    if (global_index >= run_data.size()) return nullptr;
    return addr + run_data[global_index];
  }

  // Thread-strided iteration: thread t gets indices t, t+T, t+2T, ...
  // Returns nullopt when done.
  std::optional<size_t> next_index(size_t thread_id, size_t& local_idx) {
    // Clamp begin/end to the record count: float precision can round
    // end_frac * run_data.size() slightly ABOVE run_data.size() (e.g.
    // 1.0f * 820307312 → 820307328), which used to make the loop read
    // past the last record and segfault.
    size_t n = run_data.size();
    size_t begin = std::min(static_cast<size_t>(start_frac * n), n);
    size_t end   = std::min(static_cast<size_t>(end_frac   * n), n);
    size_t idx = begin + thread_id + local_idx * thread_num;
    local_idx++;
    if (idx >= end) return std::nullopt;
    return idx;
  }
};

class Twitter : public Workload {
 public:
  Twitter(std::string file, int tn, float s = 0, float e = 1)
      : Workload(file, tn, s, e) {
    is_ycsb = false;
    open_file();
    size_t offset = 0;
    while (offset < len) {
      auto st = reinterpret_cast<twitter_workload*>(addr + offset);
      run_data.emplace_back(offset);
      offset += sizeof(twitter_workload) + st->key_len;
    }
    std::cout << "Twitter records: " << run_data.size() << std::endl;
  }
};

class YCSB : public Workload {
 public:
  YCSB(std::string file, int tn, float s = 0, float e = 1)
      : Workload(file, tn, s, e) {
    is_ycsb = true;
    open_file();
    size_t offset = 0;
    while (offset < len) {
      run_data.emplace_back(offset);
      offset += sizeof(ycsb_workload);
    }
    std::cout << "YCSB records: " << run_data.size() << std::endl;
  }
};

// ── Item accessors ──────────────────────────────────────────────────

static inline op_type item_op(const char* rec, bool is_ycsb) {
  if (is_ycsb) {
    auto y = reinterpret_cast<const ycsb_workload*>(rec);
    return y->op == 0 ? op_type::GET : op_type::ADD;
  } else {
    auto t = reinterpret_cast<const twitter_workload*>(rec);
    return static_cast<op_type>(t->op);
  }
}

static inline const char* item_key(const char* rec, bool is_ycsb) {
  if (is_ycsb)
    return reinterpret_cast<const char*>(
        &reinterpret_cast<const ycsb_workload*>(rec)->key);
  else
    return reinterpret_cast<const char*>(rec) + sizeof(twitter_workload);
}

static inline size_t item_key_len(const char* rec, bool is_ycsb) {
  if (is_ycsb) return sizeof(size_t);
  return reinterpret_cast<const twitter_workload*>(rec)->key_len;
}

static inline size_t item_value_len(const char* rec, bool is_ycsb) {
  if (is_ycsb) return reinterpret_cast<const ycsb_workload*>(rec)->value_len;
  return reinterpret_cast<const twitter_workload*>(rec)->value_len;
}

static inline uint32_t item_ttl(const char* rec, bool is_ycsb) {
  if (is_ycsb) return 1024 * 1024;
  return reinterpret_cast<const twitter_workload*>(rec)->ttl;
}

// ── RocksDB wrapper ──────────────────────────────────────────────────

class RocksDBWrapper {
 public:
  rocksdb::DB* db = nullptr;
  rocksdb::Options options;

  bool open(const std::string& path, bool fresh = true) {
    if (fresh) {
      std::filesystem::remove_all(path);
      std::filesystem::create_directories(path);
    }
    options.create_if_missing = true;
    options.IncreaseParallelism(static_cast<int>(std::thread::hardware_concurrency()));
    options.OptimizeLevelStyleCompaction(256ULL * 1024 * 1024);
    options.write_buffer_size = 256ULL * 1024 * 1024;
    options.max_write_buffer_number = 4;
    options.level0_file_num_compaction_trigger = 4;
    options.target_file_size_base = 64ULL * 1024 * 1024;
    options.compression = rocksdb::kSnappyCompression;

    rocksdb::Status s = rocksdb::DB::Open(options, path, &db);
    if (!s.ok()) {
      std::cerr << "RocksDB open error: " << s.ToString() << std::endl;
      return false;
    }
    return true;
  }

  bool put(const char* key, size_t klen, const char* val, size_t vlen) {
    rocksdb::WriteOptions wo;
    wo.disableWAL = true;
    return db->Put(wo, rocksdb::Slice(key, klen), rocksdb::Slice(val, vlen)).ok();
  }

  bool get(const char* key, size_t klen, std::string& value) {
    return db->Get(rocksdb::ReadOptions(), rocksdb::Slice(key, klen), &value).ok();
  }

  void close() {
    if (db) {
      db->Flush(rocksdb::FlushOptions());
      delete db;
      db = nullptr;
    }
  }

  ~RocksDBWrapper() { close(); }
};

// ── Counter ──────────────────────────────────────────────────────────

struct Counter {
  std::atomic_size_t ops_count{0};
  std::atomic_size_t get_count{0};
  std::atomic_size_t set_count{0};
  std::atomic_size_t add_count{0};
  std::atomic_size_t hit_count{0};
  std::atomic_size_t miss_count{0};
  std::atomic_size_t rocksdb_not_found{0};

  void add(const Counter& c) {
    ops_count  += c.ops_count.load();
    get_count  += c.get_count.load();
    set_count  += c.set_count.load();
    add_count  += c.add_count.load();
    hit_count  += c.hit_count.load();
    miss_count += c.miss_count.load();
    rocksdb_not_found += c.rocksdb_not_found.load();
  }
};

// ── Benchmark result ────────────────────────────────────────────────

struct BenchResult {
  double throughput_mops;
  double runtime_sec;
  size_t total_ops;
  size_t get_ops;
  size_t set_ops;
  size_t hemu_hits;
  size_t hemu_misses;
  double hemu_hit_ratio;
  size_t rocksdb_not_found;
};

// ── Benchmark ────────────────────────────────────────────────────────

class Benchmark {
 public:
  const Args& args;
  Workload*  workload = nullptr;
  RocksDBWrapper rdb;
  Hemu_Cache::Hemu* hemu = nullptr;
  char** value_buf = nullptr;  // per-thread value buffers
  static constexpr size_t MAX_VAL = 4 * 1024 * 1024;

  Benchmark(const Args& a) : args(a) {
    value_buf = new char*[args.threads];
    for (size_t i = 0; i < args.threads; i++) {
      value_buf[i] = new char[MAX_VAL];
      for (size_t j = 0; j < MAX_VAL; j++) value_buf[i][j] = 'A' + j % 26;
    }
  }

  ~Benchmark() {
    if (hemu) { delete hemu; hemu = nullptr; }
    if (workload) { delete workload; workload = nullptr; }
    if (value_buf) {
      for (size_t i = 0; i < args.threads; i++) delete[] value_buf[i];
      delete[] value_buf;
    }
  }

  void init_workload() {
    if (args.datafile.find("cluster") != std::string::npos) {
      std::string path = "/data/" + args.datafile;
      workload = new Twitter(path, args.threads, args.start, args.end);
    } else if (args.datafile.find("ycsb") != std::string::npos) {
      // "ycsba" → workloada
      char w = args.datafile.back();
      std::string path = "/data/YCSB_4096_1_256/ycsb_run_workload" + std::string(1, w);
      workload = new YCSB(path, args.threads, args.start, args.end);
    } else {
      std::cerr << "Unknown datafile: " << args.datafile << std::endl;
      exit(1);
    }
  }

  void init_rocksdb() {
    if (!rdb.open(args.db_path, true)) {
      std::cerr << "Failed to open RocksDB" << std::endl;
      exit(1);
    }
  }

  void init_hemu() {
    Hemu_Cache::config.esm_allocator_type =
        (args.esm == 1) ? Hemu_Cache::AllocatorType::FILE
                        : Hemu_Cache::AllocatorType::NUMA;
    Hemu_Cache::config.eSM_NODE = args.cxl_node;
    Hemu_Cache::config.CacheCapacity = args.hemu_cap_bytes;
    Hemu_Cache::config.CacheStrategy = 1;
    Hemu_Cache::config.DRAM_PERCENT = 0;
    Hemu_Cache::config.FILE_POOL_DIR = "/mnt/cxlpool/hemu_rocksdb_bench";
    hemu = new Hemu_Cache::Hemu();
  }

  // ── Run: RocksDB only ──────────────────────────────────────────────
  BenchResult run_rocksdb_only() {
    std::cout << "\n══ Run: RocksDB-only ══" << std::endl;
    Counter global_counter;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> workers;
    for (size_t t = 0; t < args.threads; t++) {
      workers.emplace_back([&, t]() {
        Counter local;
        std::string rval;
        rval.reserve(4096);
        size_t local_idx = 0;
        while (true) {
          auto idx_opt = workload->next_index(t, local_idx);
          if (!idx_opt) break;
          char* rec = workload->get_record(*idx_opt);
          if (!rec) break;
          op_type op = item_op(rec, workload->is_ycsb);
          const char* key = item_key(rec, workload->is_ycsb);
          size_t klen = item_key_len(rec, workload->is_ycsb);
          size_t vlen = item_value_len(rec, workload->is_ycsb);

          if (op == SET || op == ADD) {
            rdb.put(key, klen, value_buf[t], vlen);
            if (op == SET) local.set_count++;
            else local.add_count++;
          } else if (op == GET) {
            local.get_count++;
            bool found = rdb.get(key, klen, rval);
            if (!found) {
              // GET failed: key not in RocksDB. Insert it if value_len > 0.
              local.rocksdb_not_found++;
              if (vlen > 0)
                rdb.put(key, klen, value_buf[t], vlen);
            }
          }
          local.ops_count++;
        }
        global_counter.add(local);
      });
    }
    for (auto& th : workers) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    BenchResult r;
    r.total_ops     = global_counter.ops_count.load();
    r.get_ops       = global_counter.get_count.load();
    r.set_ops       = global_counter.set_count.load() + global_counter.add_count.load();
    r.runtime_sec   = sec;
    r.throughput_mops = (r.total_ops / 1e6) / sec;
    r.hemu_hits     = 0;
    r.hemu_misses   = 0;
    r.hemu_hit_ratio= 0.0;
    r.rocksdb_not_found = global_counter.rocksdb_not_found.load();
    return r;
  }

  // ── Run: Hemu only (no RocksDB) ──────────────────────────────────
  BenchResult run_hemu_only() {
    std::cout << "\n══ Run: Hemu-only ══" << std::endl;
    init_hemu();

    Counter global_counter;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> workers;
    for (size_t t = 0; t < args.threads; t++) {
      workers.emplace_back([&, t]() {
        Counter local;
        char vbuf[MAX_VAL];
        size_t local_idx = 0;
        while (true) {
          auto idx_opt = workload->next_index(t, local_idx);
          if (!idx_opt) break;
          char* rec = workload->get_record(*idx_opt);
          if (!rec) break;
          op_type op = item_op(rec, workload->is_ycsb);
          const char* key = item_key(rec, workload->is_ycsb);
          size_t klen = item_key_len(rec, workload->is_ycsb);
          size_t vlen = item_value_len(rec, workload->is_ycsb);
          uint32_t ttl = item_ttl(rec, workload->is_ycsb);

          if (op == SET || op == ADD) {
            hemu->set(const_cast<char*>(key), klen, value_buf[t], vlen, ttl);
            if (op == SET) local.set_count++;
            else local.add_count++;
          } else if (op == GET) {
            local.get_count++;
            size_t vlen_out = 0;
            int ret = hemu->get(const_cast<char*>(key), klen, vbuf, vlen_out);
            if (ret == static_cast<int>(Hemu_Cache::OP_states::SUCC)) {
              local.hit_count++;
            } else {
              // Miss: insert into Hemu only if value_len > 0.
              local.miss_count++;
              if (vlen > 0)
                hemu->set(const_cast<char*>(key), klen, value_buf[t], vlen, ttl);
            }
          }
          local.ops_count++;
        }
        global_counter.add(local);
      });
    }
    for (auto& th : workers) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    BenchResult r;
    r.total_ops     = global_counter.ops_count.load();
    r.get_ops       = global_counter.get_count.load();
    r.set_ops       = global_counter.set_count.load() + global_counter.add_count.load();
    r.runtime_sec   = sec;
    r.throughput_mops = (r.total_ops / 1e6) / sec;
    r.hemu_hits     = global_counter.hit_count.load();
    r.hemu_misses   = global_counter.miss_count.load();
    r.hemu_hit_ratio= (r.get_ops > 0) ? double(r.hemu_hits) / r.get_ops : 0.0;
    r.rocksdb_not_found = 0;
    return r;
  }

  // ── Run: Hemu + RocksDB ─────────────────────────────────────────────
  BenchResult run_hemu_rocksdb() {
    std::cout << "\n══ Run: Hemu + RocksDB ══" << std::endl;
    init_hemu();

    Counter global_counter;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> workers;
    for (size_t t = 0; t < args.threads; t++) {
      workers.emplace_back([&, t]() {
        Counter local;
        std::string rval;
        rval.reserve(4096);
        char vbuf[MAX_VAL];
        size_t local_idx = 0;
        while (true) {
          auto idx_opt = workload->next_index(t, local_idx);
          if (!idx_opt) break;
          char* rec = workload->get_record(*idx_opt);
          if (!rec) break;
          op_type op = item_op(rec, workload->is_ycsb);
          const char* key = item_key(rec, workload->is_ycsb);
          size_t klen = item_key_len(rec, workload->is_ycsb);
          size_t vlen = item_value_len(rec, workload->is_ycsb);
          uint32_t ttl = item_ttl(rec, workload->is_ycsb);

          if (op == SET || op == ADD) {
            // Write to RocksDB and Hemu.
            rdb.put(key, klen, value_buf[t], vlen);
            hemu->set(const_cast<char*>(key), klen, value_buf[t], vlen, ttl);
            if (op == SET) local.set_count++;
            else local.add_count++;
          } else if (op == GET) {
            local.get_count++;
            // 1. Check Hemu first.
            size_t vlen_out = 0;
            int ret = hemu->get(const_cast<char*>(key), klen, vbuf, vlen_out);
            if (ret == static_cast<int>(Hemu_Cache::OP_states::SUCC)) {
              local.hit_count++;
            } else {
              // 2. Miss: read from RocksDB.
              local.miss_count++;
              bool found = rdb.get(key, klen, rval);
              if (!found) {
                // GET failed: key not in RocksDB either.
                local.rocksdb_not_found++;
                // Insert into both RocksDB and Hemu only if value_len > 0.
                if (vlen > 0) {
                  rdb.put(key, klen, value_buf[t], vlen);
                  hemu->set(const_cast<char*>(key), klen, value_buf[t], vlen, ttl);
                }
              } else {
                // 3. Back-fill Hemu with the value from RocksDB,
                //    only if the returned value is non-empty.
                if (!rval.empty())
                  hemu->set(const_cast<char*>(key), klen,
                            const_cast<char*>(rval.data()), rval.size(), ttl);
              }
            }
          }
          local.ops_count++;
        }
        global_counter.add(local);
      });
    }
    for (auto& th : workers) th.join();
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    BenchResult r;
    r.total_ops     = global_counter.ops_count.load();
    r.get_ops       = global_counter.get_count.load();
    r.set_ops       = global_counter.set_count.load() + global_counter.add_count.load();
    r.runtime_sec   = sec;
    r.throughput_mops = (r.total_ops / 1e6) / sec;
    r.hemu_hits     = global_counter.hit_count.load();
    r.hemu_misses   = global_counter.miss_count.load();
    r.hemu_hit_ratio= (r.get_ops > 0) ? double(r.hemu_hits) / r.get_ops : 0.0;
    r.rocksdb_not_found = global_counter.rocksdb_not_found.load();
    return r;
  }

  void print_result(const std::string& name, const BenchResult& r) {
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  [" << name << "]\n"
              << "    Throughput:       " << r.throughput_mops << " Mops/s\n"
              << "    Total ops:       " << r.total_ops << "\n"
              << "    Get ops:         " << r.get_ops << "\n"
              << "    Set/Add ops:     " << r.set_ops << "\n"
              << "    Runtime:         " << std::setprecision(2) << r.runtime_sec << " s\n"
              << "    RocksDB not-found:" << r.rocksdb_not_found << "\n";
    if (r.hemu_hits > 0 || r.hemu_misses > 0) {
      std::cout << "    Hemu hits:       " << r.hemu_hits << "\n"
                << "    Hemu misses:     " << r.hemu_misses << "\n"
                << "    Hemu hit ratio:  " << std::setprecision(4)
                << r.hemu_hit_ratio * 100.0 << " %\n";
    }
    std::cout << std::endl;
  }

  void run() {
    init_workload();
    init_rocksdb();

    BenchResult r_rocks, r_hemu;
    bool do_rocks, do_hemu;
    bool do_hemu_only = false;
    if (args.hemu_enabled >= 0) {
      // --hemu flag: 0 = RocksDB only, 1 = Hemu+RocksDB, 2 = Hemu only.
      do_rocks     = (args.hemu_enabled == 0);
      do_hemu      = (args.hemu_enabled == 1);
      do_hemu_only = (args.hemu_enabled == 2);
    } else {
      do_rocks     = (args.mode == "rocksdb" || args.mode == "both");
      do_hemu      = (args.mode == "hemu"   || args.mode == "both");
      do_hemu_only = false;
    }

    if (do_rocks) {
      r_rocks = run_rocksdb_only();
      print_result("RocksDB-only", r_rocks);
    }
    // Reopen RocksDB fresh to ensure fair comparison.
    if (do_rocks && do_hemu) {
      rdb.close();
      init_rocksdb();
    }
    if (do_hemu) {
      r_hemu = run_hemu_rocksdb();
      print_result("Hemu + RocksDB", r_hemu);
    }
    if (do_hemu_only) {
      r_hemu = run_hemu_only();
      print_result("Hemu-only", r_hemu);
    }

    // Summary comparison
    if (do_rocks && do_hemu) {
      std::cout << "══════════════════════════════════════════════════\n";
      std::cout << " Summary (" << args.datafile << ")\n";
      std::cout << "══════════════════════════════════════════════════\n";
      std::cout << std::fixed << std::setprecision(4);
      std::cout << "  RocksDB-only:     " << r_rocks.throughput_mops << " Mops/s\n";
      std::cout << "  Hemu + RocksDB:   " << r_hemu.throughput_mops  << " Mops/s\n";
      double speedup = (r_rocks.throughput_mops > 0)
                       ? r_hemu.throughput_mops / r_rocks.throughput_mops : 0.0;
      std::cout << "  Speedup:          " << std::setprecision(2) << speedup << "x\n";
      std::cout << "  Hemu hit ratio:   " << std::setprecision(4)
                << r_hemu.hemu_hit_ratio * 100.0 << " %\n";
      std::cout << "══════════════════════════════════════════════════\n";
    }

    // Write result file (always, for whichever modes ran)
    std::ofstream out(args.result_path, std::fstream::out | std::fstream::app);
    if (do_rocks) {
      out << args.datafile << " "
          << "rocksdb-only " << r_rocks.throughput_mops << " "
          << r_rocks.runtime_sec << " " << args.threads << " "
          << r_rocks.total_ops << "\n";
    }
    if (do_hemu) {
      out << args.datafile << " "
          << "hemu+rocksdb " << r_hemu.throughput_mops << " "
          << r_hemu.runtime_sec << " " << args.threads << " "
          << r_hemu.total_ops << " " << r_hemu.hemu_hit_ratio << "\n";
    }
    if (do_hemu_only) {
      out << args.datafile << " "
          << "hemu-only " << r_hemu.throughput_mops << " "
          << r_hemu.runtime_sec << " " << args.threads << " "
          << r_hemu.total_ops << " " << r_hemu.hemu_hit_ratio << "\n";
    }
    out.close();

    rdb.close();
    if (hemu) { delete hemu; hemu = nullptr; }
    std::filesystem::remove_all(args.db_path);
  }
};

int main(int argc, char** argv) {
  Args args = parse_args(argc, argv);

  if (args.datafile.empty()) {
    std::cerr << "Error: --datafile is required (e.g. cluster10, ycsba)" << std::endl;
    std::cerr << "  Twitter: cluster3, cluster10, cluster22, cluster24, ..." << std::endl;
    std::cerr << "  YCSB:    ycsba, ycsbb, ycsbc, ycsbd, ycsbe, ycsbf" << std::endl;
    return 1;
  }

  std::cout << "╔══════════════════════════════════════════════════════════╗\n";
  std::cout << "║  Hemu + RocksDB Throughput Benchmark                     ║\n";
  std::cout << "╚══════════════════════════════════════════════════════════╝\n";
  std::cout << "  datafile:      " << args.datafile << "\n";
  std::cout << "  threads:       " << args.threads << "\n";
  std::cout << "  hemu_capacity: " << args.hemu_cap_bytes << " bytes\n";
  std::cout << "  db_path:       " << args.db_path << "\n";
  std::cout << "  mode:          " << args.mode << "\n";
  if (args.hemu_enabled >= 0) {
    const char* hemu_desc =
      (args.hemu_enabled == 0) ? " (RocksDB only)" :
      (args.hemu_enabled == 1) ? " (Hemu+RocksDB)" :
      (args.hemu_enabled == 2) ? " (Hemu only)" : "";
    std::cout << "  hemu:          " << args.hemu_enabled << hemu_desc << "\n";
  }
  std::cout << "  esm:           " << args.esm
            << (args.esm == 1 ? " (File/DAX)" : " (NUMA/CXL)") << "\n";
  std::cout << "  cxl_node:      " << args.cxl_node << "\n";
  std::cout << "  start:         " << args.start << "\n";
  std::cout << "  end:           " << args.end << "\n";
  std::cout << std::endl;

  Benchmark bench(args);
  bench.run();

  print_DRAM_usage();
  return 0;
}
