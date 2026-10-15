#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <regex>
#include <set>
#include <stack>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <emmintrin.h>
#include <immintrin.h>
#include <libpmem.h>
#include <numa.h>
#include <sys/wait.h>
#include <unistd.h>

void print_DRAM_usage() {
  {
    auto pid = getpid();
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen(("cat /proc/" + std::to_string(pid) + "/status").c_str(), "r"), pclose);
    if (!pipe) {
      throw std::runtime_error("popen() failed!");
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      std::string result = buffer.data();
      if (result.find("VmRSS") != std::string::npos) {
        std::string mem_ocp =
            std::regex_replace(result, std::regex("[^0-9]*([0-9]+).*"), std::string("$1"));
        printf("DRAM consumption: %.1f GB.\n", stof(mem_ocp) / 1024 / 1024);
        break;
      }
    }
  }
}

namespace Hemu_Cache {
const int CACHELINE_SIZE = 64;
enum AllocatorType { DRAM, FILE, NUMA };
enum eSMDevice { PMem = 1, CXL = 2 };
class Config {
 public:
  // You must set these parameters before creating Hemu instance
  size_t CacheCapacity = 0;
  size_t Ini_Hashtable_Capacity = 16 * 1024 * 1024ULL;  // Bucket num, default 16M
  AllocatorType hashtable_allocator = AllocatorType::DRAM;
  AllocatorType esm_allocator_type = AllocatorType::NUMA;  // set to NUMA if not using DAX mode.
  int eSM_NODE = 1;                                        // set only if allocator_type is NUMA
  std::string FILE_POOL_DIR;

  // Cache strategy
  bool Balancer_ON = true;
  int CacheStrategy = 1;     // 1:On 0:Off
  float DRAM_PERCENT = 0.0;  // Percentage of cache allocated to DRAM
  float max_dram_percent = 0.5;
  float highest_dram_percent = 0.0;
  float avg_dram_percent_sum = 0.0;
  float avg_dram_percent_count = 0.0;
  // Counters
  int CPU_CORE_NUM = 0;
  std::atomic_size_t MEMORY_MANAGER_NUM{0};
  int64_t PAGE_NUM = 0;

  // Do not change these parameters unless you know what you are doing
  size_t Balancer_Interval = 100;  // ms
  size_t Access_Granularity = 64;
  int ACR_WINDOW_SIZE = 64;            // Acr sliding window size (n)
  size_t PAGE_SIZE = 4 * 1024 * 1024;  // 1MB

  bool set_option(const std::string &key, const std::string &val) {
    if (key == "CacheStrategy")
      CacheStrategy = std::stoi(val);
    else if (key == "PAGE_SIZE")
      PAGE_SIZE = std::stoull(val);
    else if (key == "Balancer_ON")
      Balancer_ON = std::stoi(val) != 0;
    else if (key == "Balancer_Interval")
      Balancer_Interval = std::stoull(val);
    else if (key == "Access_Granularity")
      Access_Granularity = std::stoull(val);
    else if (key == "ACR_WINDOW_SIZE")
      ACR_WINDOW_SIZE = std::stoi(val);
    else if (key == "Ini_Hashtable_Capacity")
      Ini_Hashtable_Capacity = std::stoull(val);
    else if (key == "DRAM_PERCENT")
      DRAM_PERCENT = std::stof(val);
    else
      return false;
    return true;
  }
};

inline Config config;

enum class OP_states : int { SUCC = 1, FAIL = -3, NOT_FOUND = -2, TTL_OUT = -1 };
struct BWMonitor {
  float DRAM_READ_BW;
  float DRAM_WRITE_BW;
  float eSM_READ_BW;
  float eSM_WRITE_BW;
};

struct Counter {
  std::atomic_size_t hit_count{0};
  std::atomic_size_t miss_count{0};
  void hit_count_inc() {
    static thread_local size_t counter = 0;
    if (counter++ % 1000 == 0) {
      hit_count++;
    }
  }
  void miss_count_inc() {
    static thread_local size_t counter = 0;
    if (counter++ % 1000 == 0) {
      miss_count++;
    }
  }
  float get_miss_ratio() {
    size_t total = hit_count + miss_count;
    if (total == 0) return 0.0;
    auto m = static_cast<float>(miss_count) / total;
    // std::cout << "hit_count: " << hit_count << ", miss_count: " << miss_count
    //           << ", miss_ratio: " << m << std::endl;
    hit_count = 0;
    miss_count = 0;
    return m;
  }
};
inline Counter global_counter;

constexpr size_t INVALID_ADDR = 0;
inline std::atomic_size_t current_time{0};  // For TTL

namespace Timer {
template <typename T>
struct is_duration : std::false_type {};

template <class Rep, std::intmax_t Num, std::intmax_t Denom>
struct is_duration<std::chrono::duration<Rep, std::ratio<Num, Denom>>> : std::true_type {};

/**
 * @brief Wrapper that implements stopwatch functionality.
 *
 */
class Timer {
 public:
  Timer() noexcept {}
  ~Timer() noexcept {}

  /**
   * @brief Start time counter.
   *
   */
  void start() noexcept { start_ = std::chrono::high_resolution_clock::now(); }

  /**
   * @brief Clear time counter.
   *
   */
  void clear() noexcept { start_ = {}; }

  /**
   * @brief Returns amount of elapsed time.
   *
   * @tparam T unit used to return.
   * @return float
   */
  template <typename T>
  float elapsed() noexcept {
    static_assert(is_duration<T>::value);

    auto stop = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, typename T::period> e = stop - start_;
    return e.count();
  }

  /**
   * @brief Returns true if given duration is elapsed since last call.
   *
   * @tparam T unit used by duration.
   * @param d duration to be checked.
   * @return true
   * @return false
   */
  template <typename T>
  bool is_elapsed(const T &d) noexcept {
    static_assert(is_duration<T>::value);

    static auto prev = start_;

    auto stop = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float, typename T::period> e = stop - prev;

    if (e >= d) {
      prev = stop;
      return true;
    } else
      return false;
  }

 private:
  /// Time point when the stopwatched started.
  std::chrono::high_resolution_clock::time_point start_;
};

}  // namespace Timer

class spinlock {
 private:
  std::atomic<bool> lock_ = {0};

 public:
  void lock() noexcept {
    for (;;) {
      // Optimistically assume the lock is free on the first try
      if (!lock_.exchange(true, std::memory_order_acquire)) {
        return;
      }
      // Wait for lock to be released without generating cache misses
      while (lock_.load(std::memory_order_relaxed)) {
        // Issue X86 PAUSE or ARM YIELD instruction to reduce contention between
        // hyper-threads
        __builtin_ia32_pause();
      }
    }
  }

  bool try_lock() noexcept {
    // First do a relaxed load to check if lock is free in order to prevent
    // unnecessary cache misses if someone does while(!try_lock())
    return !lock_.load(std::memory_order_relaxed) &&
           !lock_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept { lock_.store(false, std::memory_order_release); }
  auto load() noexcept { return lock_.load(); }
};

class ReadWriteLock {
 private:
  std::atomic<char> readers;
  std::atomic<bool> writer;
  std::atomic<bool> writeRequest;

 public:
  ReadWriteLock() : readers(0), writer(false), writeRequest(false) {}
  void ini() {
    readers = 0;
    writer = false;
  }
  void readLock() {
    while ((true)) {
      while (writer.load(std::memory_order_acquire) ||
             writeRequest.load(std::memory_order_acquire)) {
        // while (writer.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      readers.fetch_add(1, std::memory_order_relaxed);
      if (writer.load(std::memory_order_acquire)) {
        readers.fetch_sub(1, std::memory_order_release);
      } else {
        break;
      }
    }
  }

  void readUnlock() { readers.fetch_sub(1, std::memory_order_release); }

  void writeLock() {
    writeRequest.store(true, std::memory_order_release);
    while (writer.exchange(true, std::memory_order_acquire)) {
      std::this_thread::yield();
      // __builtin_ia32_pause();
    }
    while (readers.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
      // __builtin_ia32_pause();
    }
    writeRequest.store(false, std::memory_order_release);
  }

  void writeUnlock() { writer.store(false, std::memory_order_release); }
};
template <typename LockT>
class Lock {
 private:
  LockT lock;

 public:
  void ini() {
    if constexpr (std::is_same_v<decltype(lock), ReadWriteLock>)
      lock.ini();
    else if constexpr (std::is_same_v<decltype(lock), spinlock>)
      lock.unlock();
  }
  void readLock() {
    if constexpr (std::is_same_v<decltype(lock), ReadWriteLock>)
      lock.readLock();
    else if constexpr (std::is_same_v<decltype(lock), spinlock>)
      lock.lock();
  }

  void readUnlock() {
    if constexpr (std::is_same_v<decltype(lock), ReadWriteLock>)
      lock.readUnlock();
    else if constexpr (std::is_same_v<decltype(lock), spinlock>)
      lock.unlock();
  }

  void writeLock() {
    if constexpr (std::is_same_v<decltype(lock), ReadWriteLock>)
      lock.writeLock();
    else if constexpr (std::is_same_v<decltype(lock), spinlock>)
      lock.lock();
  }

  void writeUnlock() {
    if constexpr (std::is_same_v<decltype(lock), ReadWriteLock>)
      lock.writeUnlock();
    else if constexpr (std::is_same_v<decltype(lock), spinlock>)
      lock.unlock();
  }
};

// Hash function.
static inline size_t hash_func(const void *k, size_t _len,
                               size_t _seed = static_cast<size_t>(0xc70f6907UL)) {
  return std::_Hash_bytes(k, _len, _seed);
}

namespace Allocator {

class Allocator {
 protected:
  AllocatorType allocator_type;
  std::list<char *> free_page_list;
  spinlock lock;
  bool add_free_page(char *addr) {
    bool r = false;
    lock.lock();
    if (free_page_list.size() < static_cast<size_t>(config.MEMORY_MANAGER_NUM)) {
      free_page_list.push_back(addr);
      r = true;
    }
    lock.unlock();
    return r;
  }
  void get_free_page(char *&addr, size_t size) {
    if (size == config.PAGE_SIZE) {
      lock.lock();
      if (!free_page_list.empty()) {
        addr = free_page_list.front();
        free_page_list.pop_front();
      } else {
        addr = nullptr;
      }
      lock.unlock();
    }
  }

 public:
  Allocator() {}
  virtual ~Allocator() {}
  virtual char *recover(std::string name, size_t size) { return nullptr; }
  virtual void *alloc(size_t size, std::string name = "") = 0;
  virtual void free(char *addr = nullptr, size_t size = 0) = 0;
  virtual AllocatorType get_allocator_type() { return allocator_type; }
};

class AllocatorDRAM : public Allocator {
 public:
  AllocatorDRAM() { this->allocator_type = AllocatorType::DRAM; }
  virtual void *alloc(size_t size, std::string name = "") override { return new char[size]; }
  virtual void free(char *addr = nullptr, size_t size = 0) override {
    assert(addr != nullptr);
    delete addr;
  }
};

class AllocatorFile : public Allocator {
 private:
  spinlock page_name_map_lock;
  std::map<void *, std::string> page_name_map;

 public:
  AllocatorFile() {
    this->allocator_type = AllocatorType::FILE;
    std::cout << "PMem Allocator Initialized, PM Path: " << config.FILE_POOL_DIR << std::endl;
  }
  ~AllocatorFile() { std::cout << "page_name_map: " << page_name_map.size() << std::endl; }
  char *recover(std::string name, size_t size) {
    void *ptr;
    size_t mapped_len;
    int is_pmem;
    if ((ptr = pmem_map_file(name.c_str(), size, PMEM_FILE_CREATE, 0666, &mapped_len, &is_pmem)) ==
        NULL) {
      perror("pmem_map_file");
    }
    assert(ptr != nullptr);
    page_name_map_lock.lock();
    page_name_map[ptr] = name;
    page_name_map_lock.unlock();
    return static_cast<char *>(ptr);
  }

  virtual void *alloc(size_t size, std::string name = "") override {
    if (size == config.PAGE_SIZE) {
      char *addr = nullptr;
      get_free_page(addr, size);
      if (addr != nullptr) {
        return addr;
      }
    }
    name = config.FILE_POOL_DIR + name;
    void *ptr;
    size_t mapped_len;
    int is_pmem;
    if ((ptr = pmem_map_file(name.c_str(), size, PMEM_FILE_CREATE, 0666, &mapped_len, &is_pmem)) ==
        NULL) {
      perror("pmem_map_file");
    }
    assert(ptr != nullptr);
    page_name_map_lock.lock();
    page_name_map[ptr] = name;
    page_name_map_lock.unlock();
    return ptr;
  }
  virtual void free(char *addr = nullptr, size_t size = 0) override {
    assert(addr != nullptr);
    if (size == config.PAGE_SIZE) {
      bool r = add_free_page(addr);
      if (r) return;
    }
    pmem_unmap(addr, size);
    page_name_map_lock.lock();
    auto name = page_name_map[addr];
    page_name_map.erase(addr);
    page_name_map_lock.unlock();
    std::filesystem::remove(name);
  }
};
class AllocatorNUMA : public Allocator {
 public:
  AllocatorNUMA() {
    this->allocator_type = AllocatorType::NUMA;
    std::cout << "CXL Allocator Initialized, Allocate CXL on node " << config.eSM_NODE << std::endl;
  }
  virtual void *alloc(size_t size, std::string name = "") override {
    if (size == config.PAGE_SIZE) {
      char *addr = nullptr;
      get_free_page(addr, size);
      if (addr != nullptr) {
        return addr;
      }
    }
    auto ptr = numa_alloc_onnode(size, config.eSM_NODE);
    assert(ptr != nullptr);

    return ptr;
  }
  virtual void free(char *addr = nullptr, size_t size = 0) override {
    assert(addr != nullptr);
    if (size == config.PAGE_SIZE) {
      bool r = add_free_page(addr);
      if (r) return;
    }
    numa_free(addr, size);
  }
};

Allocator *new_alloctor(AllocatorType _allocator_type) {
  if (_allocator_type == AllocatorType::NUMA)
    return new AllocatorNUMA();
  else if (_allocator_type == AllocatorType::FILE)
    return new AllocatorFile();
  else if (_allocator_type == AllocatorType::DRAM)
    return new AllocatorDRAM();
  else {
    std::cerr << "Unsupported allocator type!" << std::endl;
    exit(1);
  }
}

inline Allocator *allocator_DRAM = nullptr;
inline Allocator *allocator_eSM = nullptr;
inline Allocator *allocator_bucket = nullptr;

}  // namespace Allocator

void persist(void *addr, const size_t &size) { pmem_persist(addr, size); }

inline float measure_cxl_load_latency() noexcept {
  static void *addr = Allocator::allocator_eSM->alloc(1024 * 1024, "test");
  _mm_clflush(const_cast<void *>(addr));
  auto start = std::chrono::high_resolution_clock::now();
  volatile const uint64_t value = *reinterpret_cast<volatile const uint64_t *>(addr);
  auto stop = std::chrono::high_resolution_clock::now();
  (void)value;
  std::chrono::duration<float, std::nano> elapsed = stop - start;
  return elapsed.count();
}

class Hashtable;

namespace Page {
inline std::atomic_size_t PAGE_ID{1};
inline size_t LOCK_SIZE = 256;

enum PageType { DRAM, eSM, InvalidPageType };
struct PageHead {
  size_t ID;
  size_t data_size : 56;
  size_t type : 4;
  size_t state : 4;
};

enum PageState { FREE, ALLOCATING, ARCHIVED, RECLAIMING };
struct Page {
  PageType page_type;
  char *addr;
  size_t ID;
  size_t local_offset;
  Lock<ReadWriteLock> *locks;

  Page(size_t _ID, PageType _page_type)
      : page_type(_page_type), ID(_ID), local_offset(sizeof(PageHead)) {
    switch (_page_type) {
      case PageType::DRAM:
        addr = static_cast<char *>(
            Allocator::allocator_DRAM->alloc(config.PAGE_SIZE, "Page_" + std::to_string(ID)));
        break;
      case PageType::eSM:
        addr = static_cast<char *>(
            Allocator::allocator_eSM->alloc(config.PAGE_SIZE, "Page_" + std::to_string(ID)));
        break;
      default:
        std::runtime_error("Error PageType");
    }
    size_t num = config.PAGE_SIZE / LOCK_SIZE + 1;
    locks = static_cast<Lock<ReadWriteLock> *>(Allocator::allocator_DRAM->alloc(
        sizeof(Lock<ReadWriteLock>) * num, "Lock_" + std::to_string(ID)));

    memset(locks, 0, sizeof(Lock<ReadWriteLock>) * num);
  }
  Page(char *addr) {
    this->addr = addr;
    auto head = reinterpret_cast<PageHead *>(addr);
    this->ID = head->ID;
    this->page_type = static_cast<PageType>(head->type);
    this->local_offset = head->data_size;
    size_t num = config.PAGE_SIZE / LOCK_SIZE + 1;
    locks = static_cast<Lock<ReadWriteLock> *>(Allocator::allocator_DRAM->alloc(
        sizeof(Lock<ReadWriteLock>) * num, "Lock_" + std::to_string(ID)));
    memset(locks, 0, sizeof(Lock<ReadWriteLock>) * num);
  }
  ~Page() {
    memset(addr, 0, sizeof(PageHead));
    persist(addr, sizeof(PageHead));
    switch (page_type) {
      case PageType::DRAM:
        Allocator::allocator_DRAM->free(addr);
        break;
      case PageType::eSM:
        Allocator::allocator_eSM->free(addr, config.PAGE_SIZE);
        break;
    }
    Allocator::allocator_DRAM->free(
        reinterpret_cast<char *>(locks),
        sizeof(Lock<ReadWriteLock>) * (config.PAGE_SIZE / LOCK_SIZE + 1));
  }
  PageState get_state() {
    auto head = reinterpret_cast<PageHead *>(addr);
    return PageState(head->state);
  }
  void set_state(PageState state) {
    auto head = reinterpret_cast<PageHead *>(addr);
    head->state = state;
    persist(head, sizeof(*head));
  }
  void reset(size_t id) {
    ID = id;
    local_offset = sizeof(PageHead);
    auto head = reinterpret_cast<PageHead *>(addr);
    head->ID = ID;
    head->data_size = 0;
    persist(head, sizeof(*head));
  }
  void move_to(PageType target_type) {
    PageType srcpage_type = page_type;
    char *new_addr, *old_addr;
    switch (target_type) {
      case PageType::DRAM:
        new_addr = static_cast<char *>(Allocator::allocator_DRAM->alloc(
            config.PAGE_SIZE, "Page_" + std::to_string(PAGE_ID++)));
        break;
      case PageType::eSM:
        new_addr = static_cast<char *>(
            Allocator::allocator_eSM->alloc(config.PAGE_SIZE, "Page_" + std::to_string(PAGE_ID++)));
        break;
      default:
        std::runtime_error("Error PageType");
    }
    old_addr = addr;
    readLock(0, config.PAGE_SIZE);
    memcpy(new_addr, addr, local_offset);
    persist(new_addr, local_offset);
    addr = new_addr;
    page_type = target_type;
    archive();
    readUnlock(0, config.PAGE_SIZE);

    writeLock(0, config.PAGE_SIZE);
    memset(old_addr, 0, sizeof(PageHead));
    persist(old_addr, sizeof(PageHead));
    switch (srcpage_type) {
      case PageType::DRAM:
        Allocator::allocator_DRAM->free(old_addr);
        break;
      case PageType::eSM:
        Allocator::allocator_eSM->free(old_addr, config.PAGE_SIZE);
        break;
    }
    writeUnlock(0, config.PAGE_SIZE);
  }

  size_t get_data_size() { return reinterpret_cast<PageHead *>(addr)->data_size; }

  static size_t get_head_size() { return sizeof(PageHead); }
  void archive() {
    auto head = reinterpret_cast<PageHead *>(addr);
    head->ID = ID;
    head->data_size = local_offset;
    head->type = page_type;
    persist(head, sizeof(*head));
    head->state = PageState::ARCHIVED;
    persist(head, sizeof(*head));
  }
  void readLock(const size_t &start, const size_t &size = 1) {
    size_t ls = start / LOCK_SIZE;
    size_t rs = (start + size - 1) / LOCK_SIZE;

    for (size_t i = ls; i <= rs; i++) {
      locks[i].readLock();
    }
  }
  void writeLock(const size_t &start, const size_t &size = 1) {
    size_t ls = start / LOCK_SIZE;
    size_t rs = (start + size - 1) / LOCK_SIZE;

    for (size_t i = ls; i <= rs; i++) {
      locks[i].writeLock();
    }
  }
  void readUnlock(const size_t &start, const size_t &size = 1) {
    size_t ls = start / LOCK_SIZE;
    size_t rs = (start + size - 1) / LOCK_SIZE;

    for (size_t i = ls; i <= rs; i++) {
      locks[i].readUnlock();
    }
  }
  void writeUnlock(const size_t &start, const size_t &size = 1) {
    size_t ls = start / LOCK_SIZE;
    size_t rs = (start + size - 1) / LOCK_SIZE;

    for (size_t i = ls; i <= rs; i++) {
      locks[i].writeUnlock();
    }
  }
};
}  // namespace Page

namespace PageTable {
template <typename Key, typename Value, typename = std::enable_if_t<sizeof(Key) == 8>,
          typename = std::enable_if_t<sizeof(Value) == 8>>
class Hashmap {
 protected:
  struct alignas(CACHELINE_SIZE) Bucket {
    spinlock lock;
    Key key[3];
    Value value[3];
    Bucket *next;
  };

  size_t numbuckets;
  Bucket *buckets;

 public:
  Hashmap(size_t N = 16 * 1024) {
    numbuckets = N;
    buckets = new Bucket[numbuckets];
    memset(buckets, 0, sizeof(Bucket) * numbuckets);
  }
  ~Hashmap() { delete[] buckets; }

  void set(Key key, Value value) {
    size_t khash = hash_func(&key, sizeof(key));
    size_t bin = khash % (numbuckets - 1);
    Bucket *bucket = &buckets[bin];
    auto lock = &bucket->lock;
    lock->lock();
    while (bucket != nullptr) {
      Key *empty_key = nullptr;
      Value *empty_value = nullptr;
      for (int i = 0; i < 3; i++) {
        if (bucket->key[i] == key) {
          bucket->value[i] = value;
          lock->unlock();
          return;
        }
        if (bucket->key[i] == 0) {
          empty_key = &bucket->key[i];
          empty_value = &bucket->value[i];
        }
      }
      if (bucket->next == nullptr) {
        if (empty_key == nullptr) {
          bucket->next = new Bucket();
          bucket = bucket->next;
          empty_key = &bucket->key[0];
          empty_value = &bucket->value[0];
        }
        *empty_key = key;
        *empty_value = value;
        lock->unlock();
        return;
      }
      bucket = bucket->next;
    }
    lock->unlock();
  }
  OP_states get(Key key, Value &value, bool _lock = false) {
    size_t khash = hash_func(&key, sizeof(key));
    size_t bin = khash % (numbuckets - 1);
    Bucket *bucket = &buckets[bin];
    auto lock = &bucket->lock;
    OP_states r = OP_states::NOT_FOUND;
    while (bucket != nullptr) {
      for (int i = 0; i < 3; i++) {
        if (bucket->key[i] == key) {
          value = bucket->value[i];
          r = OP_states::SUCC;
          goto RETURN;
        }
      }
      bucket = bucket->next;
    }
  RETURN:
    return r;
  }

  void erase(Key key) {
    size_t khash = hash_func(&key, sizeof(key));
    size_t bin = khash % (numbuckets - 1);
    Bucket *bucket = &buckets[bin];
    auto lock = &bucket->lock;
    lock->lock();
    while (bucket != nullptr) {
      for (int i = 0; i < 3; i++) {
        if (bucket->key[i] == key) {
          bucket->key[i] = 0;
          lock->unlock();
          return;
        }
      }
      bucket = bucket->next;
    }
    lock->unlock();
  }
  void readAll(std::vector<std::pair<Key, Value>> &vecs) {
    for (size_t i = 0; i < numbuckets; i++) {
      Bucket *bucket = &buckets[i];
      auto lock = &bucket->lock;
      lock->lock();
      while (bucket != nullptr) {
        for (int j = 0; j < 3; j++) {
          if (bucket->key[j] != 0) {
            vecs.push_back({bucket->key[j], bucket->value[j]});
            bucket->key[j] = 0;
          }
        }
        bucket = bucket->next;
      }
      lock->unlock();
    }
  }
};

class PageTable : public Hashmap<size_t, Page::Page *> {
 public:
  using Hashmap<size_t, Page::Page *>::get;
  ~PageTable() {}
  void print() {
    size_t count = 0;
    for (size_t i = 0; i < numbuckets; i++) {
      Bucket *bucket = &(this->buckets[i]);
      auto lock = &bucket->lock;
      while (bucket != nullptr) {
        for (int i = 0; i < 3; i++) {
          if (bucket->key[i] != 0) {
            count++;
          }
        }
        bucket = bucket->next;
      }
    }
    std::cout << "Page Count:" << count << " Total size:" << count * config.PAGE_SIZE << std::endl;
  }
  void recover() {}

  Page::Page *get(size_t page_id) {
    Page::Page *s = nullptr;
    this->get(page_id, s);
    return s;
  }
  void free(size_t page_id) {
    Page::Page *page = nullptr;
    page = get(page_id);
    this->erase(page_id);
    delete page;
  }
};
inline PageTable PAGE_TABLE;
}  // namespace PageTable
namespace PageManager {
class LoadLatencyWindow {
 public:
  static constexpr size_t kWindowSize = 16;

  LoadLatencyWindow() noexcept : index_(0), count_(0), sum_(0) {
    std::fill(buffer_, buffer_ + kWindowSize, 0);
  }

  void add(int load_latency) noexcept {
    int old_value = buffer_[index_];
    buffer_[index_] = load_latency;
    if (count_ < kWindowSize) {
      ++count_;
      sum_ += load_latency;
    } else {
      sum_ += static_cast<int64_t>(load_latency) - static_cast<int64_t>(old_value);
    }
    index_ = (index_ + 1) % kWindowSize;
  }

  double average() const noexcept {
    return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
  }

 private:
  int buffer_[kWindowSize];
  size_t index_;
  size_t count_;
  int64_t sum_;
};

class PageManager {
 public:
  spinlock page_list_lock;
  std::list<Page::Page *> DRAM_page_list;
  std::list<Page::Page *> eSM_page_list;
  int max_dram_page_num = 0;
  int max_esm_page_num = 0;
  size_t thread0_dram_page_count = 0;
  size_t thread1_dram_page_count = 0;
  LoadLatencyWindow load_latency_window;

 public:
  PageManager() {}
  ~PageManager() {
    std::cout << "DRAM_page_list size: " << DRAM_page_list.size() << std::endl;
    std::cout << "eSM_page_list size: " << eSM_page_list.size() << std::endl;
    std::cout << "Highest DRAM Percent: " << config.highest_dram_percent << std::endl;
  }

  double get_load_latency_average(int load_latency) {
    load_latency_window.add(load_latency);
    return load_latency_window.average();
  }
  double get_load_latency_average() { return load_latency_window.average(); }

  void update_dram_percent() {
    auto curr = get_load_latency_average(measure_cxl_load_latency());
    auto m = global_counter.get_miss_ratio();
    if (config.Balancer_ON && m < 0.7) {
      if (curr > 450) {
        config.DRAM_PERCENT += 0.01;
      } else if (curr < 350) {
        config.DRAM_PERCENT -= 0.01;
      }
    }
    config.DRAM_PERCENT = std::min(std::max(config.DRAM_PERCENT, 0.0f), config.max_dram_percent);
    config.highest_dram_percent = std::max(config.highest_dram_percent, config.DRAM_PERCENT);
    config.avg_dram_percent_count++;
    config.avg_dram_percent_sum += config.DRAM_PERCENT;
    update_max_page_num();
    // std::cout << DRAM_page_list.size() << " " << eSM_page_list.size() << " "
    //           << config.MEMORY_MANAGER_NUM << std::endl;
  }
  void update_max_page_num() {
    max_dram_page_num = config.DRAM_PERCENT * config.PAGE_NUM - config.MEMORY_MANAGER_NUM;
    max_dram_page_num = std::max(0, max_dram_page_num);
    max_esm_page_num = config.PAGE_NUM - max_dram_page_num - 2 * config.MEMORY_MANAGER_NUM;
  }

  void archive(Page::Page *page) {
    page->archive();
    page_list_lock.lock();
    if (page->page_type == Page::PageType::DRAM) {
      DRAM_page_list.push_back(page);
    } else {
      eSM_page_list.push_back(page);
    }
    page_list_lock.unlock();
  }

  Page::Page *pop_DRAM() {
    Page::Page *page = nullptr;
    if (DRAM_page_list.size() > max_dram_page_num) {
      page_list_lock.lock();
      if (DRAM_page_list.size() > max_dram_page_num && DRAM_page_list.size() > 0) {
        for (auto it = DRAM_page_list.begin(); it != DRAM_page_list.end(); ++it) {
          Page::Page *candidate = *it;
          page = candidate;
          DRAM_page_list.erase(it);
          break;
        }
      }
      page_list_lock.unlock();
    }
    return page;
  }
  Page::Page *pop_eSM() {
    Page::Page *page = nullptr;
    if (eSM_page_list.size() > max_esm_page_num) {
      page_list_lock.lock();
      if (eSM_page_list.size() > max_esm_page_num) {
        page = eSM_page_list.front();
        eSM_page_list.pop_front();
      }
      page_list_lock.unlock();
    }
    return page;
  }
  Page::Page *promote_esm_page() {
    Page::Page *page = nullptr;

    if (DRAM_page_list.size() < max_dram_page_num * 0.9) {
      page_list_lock.lock();
      if (eSM_page_list.size()) {
        page = eSM_page_list.front();
        eSM_page_list.pop_front();
      }
      page_list_lock.unlock();
    }
    return page;
  }

  Page::Page *recover_page(char *addr) {
    Page::PageHead *head = reinterpret_cast<Page::PageHead *>(addr);
    if (head->state != Page::PageState::ARCHIVED) {
      Allocator::allocator_eSM->free(addr, config.PAGE_SIZE);
      return nullptr;
    }
    Page::Page *page = new Page::Page(addr);
    PageTable::PAGE_TABLE.set(page->ID, page);
    page_list_lock.lock();
    eSM_page_list.push_back(page);
    page_list_lock.unlock();
    return page;
  }

  Page::Page *recover_from_page(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      std::cerr << "Failed to open page file for recovery: " << path << std::endl;
      return nullptr;
    }

    char *addr = static_cast<char *>(
        Allocator::allocator_eSM->alloc(config.PAGE_SIZE, path.filename().string()));
    std::memset(addr, 0, config.PAGE_SIZE);
    file.read(addr, static_cast<std::streamsize>(config.PAGE_SIZE));
    if (file.bad()) {
      std::cerr << "Failed to read page file for recovery: " << path << std::endl;
      Allocator::allocator_eSM->free(addr, config.PAGE_SIZE);
      return nullptr;
    }

    return recover_page(addr);
  }

  void sort_eSM_page_list() {
    page_list_lock.lock();
    eSM_page_list.sort([](const Page::Page *a, const Page::Page *b) { return a->ID < b->ID; });
    page_list_lock.unlock();
  }

  Page::Page *create_DRAM_page(size_t ID = Page::PAGE_ID++) {
    auto newpage = new Page::Page(ID, Page::PageType::DRAM);
    PageTable::PAGE_TABLE.set(ID, newpage);
    return newpage;
  }
  Page::Page *create_eSM_page(size_t ID = Page::PAGE_ID++) {
    auto newpage = new Page::Page(ID, Page::PageType::eSM);
    PageTable::PAGE_TABLE.set(ID, newpage);
    return newpage;
  }

  void free_page(Page::Page *p) {
    PageTable::PAGE_TABLE.erase(p->ID);
    if (p == nullptr) return;
    p->set_state(Page::PageState::FREE);
    delete p;
  }
};
inline PageManager Page_Manager;
}  // namespace PageManager

namespace Entry {
struct Entry_ADDR {
  size_t valid_flag : 1;
  size_t count : 15;
  size_t addr : 48;
  Entry_ADDR() : valid_flag(0), count(0), addr(0) {}
  Entry_ADDR(size_t _addr) : valid_flag(1), count(0), addr(_addr) {}
  void invalidate() {
    valid_flag = 0;
    count = 0;
    addr = 0;
  }
  bool check() { return valid_flag == 1; }
  bool operator==(const Entry_ADDR &a) const { return a.addr == addr; }
  bool operator!=(const Entry_ADDR &a) const { return !(*this == a); }
};
struct Entry {
  uint32_t key_len : 8;
  uint32_t value_len : 24;
  uint32_t valid : 1;
  uint32_t endtime : 31;
  void print() {
    // std::cout << "key_len:" << key_len << " value_len:" << value_len << " key_hash:" <<
    // key_hash
    // << " bucket:" << bucket
    // << std::endl;
  }
  bool check_ttl() {
    // std::cout << "endtime:" << endtime << " current_time:" << current_time.load() << std::endl;
    return endtime > current_time.load();
  }
  static void write(Entry_ADDR &dst, const char *key, const uint32_t &key_len, const char *value,
                    const uint32_t &value_len, uint32_t endtime, bool move = false) {
    Page::Page *page = PageTable::PAGE_TABLE.get(dst.addr / config.PAGE_SIZE);
    if (page == nullptr) return;
    size_t offset = dst.addr % config.PAGE_SIZE;
    page->writeLock(offset);
    char *paddr = page->addr + offset;
    auto toLog = reinterpret_cast<Entry *>(paddr);
    toLog->key_len = key_len;
    toLog->value_len = value_len;
    toLog->valid = 1;
    if (move == true)
      toLog->endtime = endtime;
    else
      toLog->endtime = current_time.load() + endtime;
    memcpy(paddr + sizeof(Entry), key, key_len);
    memcpy(paddr + sizeof(Entry) + key_len, value, value_len);
    if (page->page_type == Page::PageType::eSM) persist(paddr, sizeof(Entry) + key_len + value_len);
    page->writeUnlock(offset);
  }
  size_t get_entry_size() { return sizeof(Entry) + key_len + value_len; }
  static size_t size(size_t k_len, size_t v_len) { return k_len + v_len + sizeof(Entry); }
  char *get_key_addr() {
    char *addr = reinterpret_cast<char *>(this);
    return addr + sizeof(Entry);
  }
  char *get_value_addr() {
    char *addr = reinterpret_cast<char *>(this);
    return addr + sizeof(Entry) + key_len;
  }
  static size_t get_entry_size(Entry_ADDR addr) {
    Page::Page *page = PageTable::PAGE_TABLE.get(addr.addr / config.PAGE_SIZE);
    if (page == nullptr) return 0;
    size_t offset = addr.addr % config.PAGE_SIZE;
    char *paddr = page->addr + offset;
    page->readLock(offset);
    auto l = reinterpret_cast<Entry *>(paddr);
    size_t size = 0;
    size = sizeof(Entry) + l->key_len + l->value_len;
    page->readUnlock(offset);
    return size;
  }

  static OP_states remove(const char *&key, const size_t &k_len, const Entry_ADDR &_addr) {
    OP_states r = OP_states::NOT_FOUND;
    Page::Page *page = PageTable::PAGE_TABLE.get(_addr.addr / config.PAGE_SIZE);
    if (page == nullptr) {
      std::cerr << "Page ID: " << _addr.addr / config.PAGE_SIZE
                << " not found. This shuold not happen!" << std::endl;
      return OP_states::NOT_FOUND;
    }
    size_t offset = _addr.addr % config.PAGE_SIZE;
    page->writeLock(offset);
    char *paddr = page->addr + offset;
    Entry *e = reinterpret_cast<Entry *>(paddr);
    if (k_len == e->key_len) {
      if (memcmp(key, e->get_key_addr(), e->key_len) == 0) {
        e->valid = 0;
        persist(e, sizeof(Entry));
        r = OP_states::SUCC;
      } else {
        std::cerr << "Key mismatch during remove operation." << std::endl;
        r = OP_states::NOT_FOUND;
      }
    }
    page->writeUnlock(offset);
    return r;
  }

  static OP_states get(const char *&key, const size_t &k_len, char *value, size_t &v_len,
                       const size_t &_khash, Entry_ADDR &_addr) {
    OP_states r = OP_states::NOT_FOUND;
    if (!_addr.check()) return r;
    Page::Page *page = nullptr;
    page = PageTable::PAGE_TABLE.get(_addr.addr / config.PAGE_SIZE);
    if (page == nullptr || page->ID != _addr.addr / config.PAGE_SIZE) {
      // std::cerr << "Accessing reclaimed page: " << _addr.addr / config.PAGE_SIZE << std::endl;
      return OP_states::NOT_FOUND;
    }
    size_t offset = _addr.addr % config.PAGE_SIZE;
    page->readLock(offset);
    char *paddr = page->addr + offset;
    auto l = reinterpret_cast<Entry *>(page->addr + offset);
    if (k_len == l->key_len && memcmp(key, paddr + sizeof(Entry), k_len) == 0) {
      auto tl = l->key_len + l->value_len;
      if (l->endtime < current_time.load()) {
        r = OP_states::TTL_OUT;
        goto RETURN;
      }
      if ((config.CacheStrategy == 1)) {
        if (_addr.count < (1 << 15) - 1) _addr.count++;
      }
      {
        memcpy(value, l->get_value_addr(), l->value_len);
      }
      v_len = l->value_len;
      r = OP_states::SUCC;
    }
  RETURN:
    page->readUnlock(offset);
    return r;
  }
  static OP_states compare(const char *&key, const size_t &k_len, Entry_ADDR &_addr) {
    OP_states r = OP_states::NOT_FOUND;
    Page::Page *page = PageTable::PAGE_TABLE.get(_addr.addr / config.PAGE_SIZE);
    if (page == nullptr || page->ID != _addr.addr / config.PAGE_SIZE) {
      // std::cerr << "Accessing reclaimed page: " << _addr.addr / config.PAGE_SIZE << std::endl;
      return OP_states::NOT_FOUND;
    }
    size_t offset = _addr.addr % config.PAGE_SIZE;
    page->readLock(offset);
    char *paddr = page->addr + offset;
    auto l = reinterpret_cast<Entry *>(page->addr + offset);
    if (k_len == l->key_len && memcmp(key, l->get_key_addr(), k_len) == 0) {
      if (!l->check_ttl()) {
        r = OP_states::TTL_OUT;
        goto RETURN;
      }
      r = OP_states::SUCC;
    }
  RETURN:
    page->readUnlock(offset);
    return r;
  }
};
}  // namespace Entry

namespace ThreadPool {
class ThreadPool {
 public:
  ThreadPool(size_t thread_count) : is_shutdown(false) {
    for (size_t i = 0; i < thread_count; ++i) {
      workers.emplace_back([this] {
        while (true) {
          std::function<void()> task;

          {
            std::unique_lock<std::mutex> lock(queue_mutex);
            // 等待条件：有任务或线程池已关闭
            condition.wait(lock, [this] { return is_shutdown || !tasks.empty(); });

            // 如果已关闭且任务队列为空，退出线程
            if (is_shutdown && tasks.empty()) {
              return;
            }

            // 取出任务
            task = std::move(tasks.front());
            tasks.pop();
          }

          task();
        }
      });
    }
  }

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  ~ThreadPool() { shutdown(); }

  void shutdown() {
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      if (is_shutdown) return;
      is_shutdown = true;
    }
    condition.notify_all();

    for (std::thread &worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  template <class F, class... Args>
  void enqueue(F &&f, Args &&...args) {
    {
      std::unique_lock<std::mutex> lock(queue_mutex);

      if (is_shutdown) {
        throw std::runtime_error("enqueue on shutdown ThreadPool");
      }

      tasks.emplace(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    }
    condition.notify_one();
  }

 private:
  std::vector<std::thread> workers;
  std::queue<std::function<void()>> tasks;

  std::mutex queue_mutex;
  std::condition_variable condition;
  bool is_shutdown;
};

ThreadPool *THREAD_POOL = nullptr;
}  // namespace ThreadPool

namespace CacheStrategy {
struct ItemInfo {
  size_t loc;
  bool is_hot;
};

// 定义键值对结构
struct KeyValue {
  size_t offset;
  size_t accessCount;
  size_t size;
};

// 定义块结构
struct Block {
  size_t totalAccessCount;
  size_t totalTTLoutCount;
  size_t kvCount;
  std::vector<KeyValue> kvs;
};

struct ItemsCollection {
  std::vector<size_t> hot_items;
  std::vector<size_t> cold_items;
  std::vector<ItemInfo> items;
  Block current_block;
};

struct HistoricalRecord {
  double density;  // Accessed count / KV count
};

class CacheRecycler {
 private:
  int maxHistoryCount;
  std::vector<HistoricalRecord> history;

  double alpha = 1;
  double epsilon = 1e-6;

  double sigmoid(double x) const { return 1.0 / (1.0 + exp(-x)); }

  double calculateDensity(const Block &block) const {
    if (block.kvCount <= 0) return 0.0;
    return static_cast<double>(block.totalAccessCount) / (block.kvCount);
  }

  double calculateRobustDensityMean() const {
    if (history.empty()) return 0.0;

    double robustMean = 0.0;
    size_t sum = history.size() * (1 + history.size()) / 2;
    for (size_t i = 0; i < history.size(); i++) {
      robustMean += 1.0 * history[i].density * (i + 1) / sum;
    }

    // {
    //   std::ofstream log_file("cache_recycler_log.txt", std::ios::app);
    //   log_file << robustMean << std::endl;
    // }
    return robustMean;
  }

  size_t calculateOriginalK(const Block &currentBlock, double robustMean) const {
    if (currentBlock.kvCount == 0) return 0;

    double currentDensity = calculateDensity(currentBlock);
    double delta = (currentDensity - robustMean) / (robustMean + epsilon);
    double sigmoidValue = sigmoid(alpha * delta);
    // std::cout << "sigmoidValue: " << sigmoidValue << " currentDensity: " << currentDensity
    // << " robustMean: " << robustMean << " delta: " << delta << std::endl;
    size_t K = static_cast<size_t>(round(currentBlock.kvCount * sigmoidValue));
    return std::max(static_cast<size_t>(1), std::min(K, currentBlock.kvCount));
  }

 public:
  CacheRecycler(int maxHistory) : maxHistoryCount(maxHistory) {}

  void processCurrentBlock(ItemsCollection *items) {
    auto &currentBlock = items->current_block;

    double robustMean = calculateRobustDensityMean();

    size_t originalK = calculateOriginalK(currentBlock, robustMean);

    size_t K = originalK;

    std::vector<double> scores;
    for (const auto &kv : currentBlock.kvs) {
      size_t correctedSize = std::max(kv.size, static_cast<size_t>(1));
      double score = (kv.accessCount) / static_cast<double>(correctedSize);
      scores.push_back(score);
    }

    std::vector<int> indices(currentBlock.kvs.size());
    std::iota(indices.begin(), indices.end(), 0);

    std::sort(indices.begin(), indices.end(),
              [&scores](int i, int j) { return scores[i] > scores[j]; });

    ItemsCollection *result = items;

    size_t total_hot_size = 0;
    int i = 0;
    for (; i < K && i < currentBlock.kvs.size(); ++i) {
      if (currentBlock.kvs[indices[i]].accessCount == 0) {
        break;
      }
      result->hot_items.push_back(currentBlock.kvs[indices[i]].offset);
      total_hot_size += currentBlock.kvs[indices[i]].size;
    }

    // log
    // {
    //   std::ofstream log_file("cache_recycler_log.txt", std::ios::app);
    //   log_file << 1.0 * total_hot_size / config.PAGE_SIZE << " "
    //            << 1.0 * result->hot_items.size() / currentBlock.kvCount << std::endl;
    // }

    for (; i < currentBlock.kvs.size(); ++i) {
      result->cold_items.push_back(currentBlock.kvs[indices[i]].offset);
    }

    std::sort(result->hot_items.begin(), result->hot_items.end());
    std::sort(result->cold_items.begin(), result->cold_items.end());

    HistoricalRecord record;
    record.density = calculateDensity(currentBlock);

    history.push_back(record);
    if (history.size() > static_cast<size_t>(maxHistoryCount)) {
      history.erase(history.begin());
    }
  }

  int getHistoryCount() const { return history.size(); }
  void clearHistory() { history.clear(); }
};

inline thread_local CacheRecycler cacherecycler(config.ACR_WINDOW_SIZE);
void get_page_info(Page::Page *_reclaim, ItemsCollection *items);

ItemsCollection get_hot_items(Page::Page *_reclaim, Hashtable *hashtable);

void archive_dram_page(Page::Page *p) {
  if (p != nullptr) {
    p->move_to(Page::PageType::eSM);
    PageManager::Page_Manager.archive(p);
  }
}
void promote_esm_page(Page::Page *p) {
  if (p != nullptr) {
    p->move_to(Page::PageType::DRAM);
    PageManager::Page_Manager.archive(p);
  }
}
void reclaim_eSM(Page::Page *p, Hashtable *hashtable);
}  // namespace CacheStrategy

namespace MemoryManager {
inline std::atomic_int global_threadid{0};

class ThreadID {
 public:
  explicit ThreadID() { id_ = global_threadid.fetch_add(1, std::memory_order_relaxed); }
  size_t get() const { return id_; }

 private:
  int id_;
};

inline thread_local ThreadID threadID;

class MemoryManager {
 public:
  explicit MemoryManager() {
    curr_eSM_page = PageManager::Page_Manager.create_eSM_page(Page::PAGE_ID++);
    curr_DRAM_page = PageManager::Page_Manager.create_DRAM_page(Page::PAGE_ID++);
    config.MEMORY_MANAGER_NUM++;
  }
  ~MemoryManager() {
    delete curr_eSM_page;
    delete curr_DRAM_page;
  }

  OP_states alloc_dram(const size_t &size, Entry::Entry_ADDR &entry_addr) {
    lock.lock();
    if (size + curr_DRAM_page->local_offset >= config.PAGE_SIZE) {
      assert(size + Page::Page::get_head_size() < config.PAGE_SIZE);
      PageManager::Page_Manager.archive(curr_DRAM_page);
      curr_DRAM_page = PageManager::Page_Manager.create_DRAM_page(Page::PAGE_ID++);
    }
    entry_addr =
        Entry::Entry_ADDR{curr_DRAM_page->ID * config.PAGE_SIZE + curr_DRAM_page->local_offset};
    curr_DRAM_page->local_offset += size;
    lock.unlock();
    return OP_states::SUCC;
  }
  OP_states alloc_eSM(const size_t &size, Entry::Entry_ADDR &entry_addr) {
    lock.lock();
    if (size + curr_eSM_page->local_offset >= config.PAGE_SIZE) {
      assert(size + Page::Page::get_head_size() < config.PAGE_SIZE);
      PageManager::Page_Manager.archive(curr_eSM_page);
      curr_eSM_page = PageManager::Page_Manager.create_eSM_page(Page::PAGE_ID++);
    }
    entry_addr =
        Entry::Entry_ADDR{curr_eSM_page->ID * config.PAGE_SIZE + curr_eSM_page->local_offset};
    curr_eSM_page->local_offset += size;
    lock.unlock();
    return OP_states::SUCC;
  }
  OP_states alloc(const size_t &size, Entry::Entry_ADDR &entry_addr) {
    lock.lock();
    OP_states r = OP_states::FAIL;
    // if (size < config.Access_Granularity) {
    //   if (size + curr_DRAM_page->local_offset >= config.PAGE_SIZE) {
    //     assert(size + Page::Page::get_head_size() < config.PAGE_SIZE);
    //     PageManager::Page_Manager.archive(curr_DRAM_page);
    //     // ThreadPool::THREAD_POOL->enqueue(CacheStrategy::reclaim_DRAM1, curr_DRAM_page);
    //     curr_DRAM_page = PageManager::Page_Manager.create_DRAM_page(Page::PAGE_ID++);
    //   }
    //   entry_addr =
    //       Entry::Entry_ADDR{curr_DRAM_page->ID * config.PAGE_SIZE +
    //       curr_DRAM_page->local_offset};
    //   curr_DRAM_page->local_offset += size;
    //   r = OP_states::SUCC;
    // } else
    {
      if (size + curr_eSM_page->local_offset > config.PAGE_SIZE) {
        assert(size + Page::Page::get_head_size() < config.PAGE_SIZE);
        PageManager::Page_Manager.archive(curr_eSM_page);
        curr_eSM_page = PageManager::Page_Manager.create_eSM_page(Page::PAGE_ID++);
      }
      entry_addr =
          Entry::Entry_ADDR{curr_eSM_page->ID * config.PAGE_SIZE + curr_eSM_page->local_offset};
      curr_eSM_page->local_offset += size;
      r = OP_states::SUCC;
    }
    lock.unlock();
    return r;
  }
  void *testaddr() { return curr_eSM_page->addr; }

 private:
  Page::Page *curr_eSM_page;
  Page::Page *curr_DRAM_page;
  spinlock lock;
};

class MemoryManagerGlobal {
 public:
  MemoryManagerGlobal() { managers_.resize(config.CPU_CORE_NUM, nullptr); }

  ~MemoryManagerGlobal() {
    std::lock_guard<std::mutex> lock(managers_mutex_);
    for (auto *m : managers_) {
      if (!m) continue;
      m->~MemoryManager();
      ::operator delete(m);
    }
  }

  Entry::Entry_ADDR alloc(size_t size) {
    Entry::Entry_ADDR addr;

    auto idx = threadID.get() % config.CPU_CORE_NUM;
    if (idx >= managers_.size()) throw std::runtime_error("Thread Index out of range");
    MemoryManager *m = nullptr;
    if (managers_[idx] == nullptr) {
      std::lock_guard<std::mutex> lock(managers_mutex_);
      if (managers_[idx] != nullptr) m = managers_[idx];
      MemoryManager *manager = static_cast<MemoryManager *>(::operator new(sizeof(MemoryManager)));
      new (manager) MemoryManager();
      managers_[idx] = manager;
    }
    m = managers_[idx];
    if (m->alloc(size, addr) != OP_states::SUCC) {
      throw std::runtime_error("Allocate Error!");
    }
    return addr;
  }
  Entry::Entry_ADDR move_alloc_DRAM(size_t size) {
    Entry::Entry_ADDR addr;

    auto idx = threadID.get() % config.MEMORY_MANAGER_NUM;
    if (idx >= managers_.size()) throw std::runtime_error("Thread Index out of range");
    auto m = managers_[idx];
    if (m->alloc_dram(size, addr) != OP_states::SUCC) {
      std::runtime_error("Allocate Error!");
    }
    return addr;
  }

 private:
  std::vector<MemoryManager *> managers_;
  std::mutex managers_mutex_;
};

inline MemoryManagerGlobal *MM;

}  // namespace MemoryManager

//================================================

struct BucketAddr {
  size_t valid : 1;
  size_t addr : 63;
};

struct bucket_entry {
  size_t khash;
  Entry::Entry_ADDR addr;
  void set(size_t a, Entry::Entry_ADDR b) {
    khash = a;
    addr = b;
  }
};

const size_t ENTRIES_PER_BUCKET = 3;

struct alignas(CACHELINE_SIZE) Bucket {
  Lock<ReadWriteLock> lock;
  bucket_entry tos[ENTRIES_PER_BUCKET];
  BucketAddr next;
};
inline const size_t MAX_PAGE_NUM = 1 * 1024 * 1024;
inline const size_t HASHTABLE_PAGE_SIZE = 32 * 1024 * 1024;
class Hemu;
class Hashtable {
  friend class Hemu;

 public:
  Hashtable(std::vector<std::pair<char *, size_t>> pages = {})
      : num_buckets(config.Ini_Hashtable_Capacity) {
    memset(PAGE_TABLE, 0, sizeof(char *) * MAX_PAGE_NUM);

    load_factor.store(0);
    OFFSET_OF_PAGE_main = -1;
    OFFSET_OF_PAGE_chain = MAX_PAGE_NUM / 2 - 1;
    OFFSET_IN_PAGE_chain = HASHTABLE_PAGE_SIZE + 1;
    OFFSET_IN_PAGE_main = HASHTABLE_PAGE_SIZE + 1;

    for (size_t i = 0; i < num_buckets; i++) new_bucket(OFFSET_IN_PAGE_main, OFFSET_OF_PAGE_main);
    std::cout << "Create hashtable: " << num_buckets << " Bucket size:" << sizeof(Bucket)
              << std::endl;
  }
  ~Hashtable() {
    for (size_t i = 0; i < MAX_PAGE_NUM; i++) {
      if (PAGE_TABLE[i] != nullptr)
        Allocator::allocator_bucket->free(PAGE_TABLE[i], HASHTABLE_PAGE_SIZE);
    }
  }
  void recover(Page::Page *page) {
    auto addr = page->addr;
    Page::PageHead *head = reinterpret_cast<Page::PageHead *>(addr);
    size_t start = sizeof(Page::PageHead);
    for (; start < head->data_size;) {
      Entry::Entry *e = reinterpret_cast<Entry::Entry *>(addr + start);
      if (e->valid == 0) {
        start += e->get_entry_size();
        continue;
      }
      auto r = recover_index(e->get_key_addr(), e->key_len,
                             Entry::Entry_ADDR(page->ID * config.PAGE_SIZE + start));
      if (r != OP_states::SUCC) {
        std::cerr << "Recover Error!" << std::endl;
        exit(-1);
      }
      start += e->get_entry_size();
    }
  }
  void print_load_factor() {
    size_t count_elem = 0;
    size_t count_bucket = 0;
    for (size_t i = 0; i < num_buckets; i++) {
      BucketAddr bucket_addr = BucketAddr{.valid = 1, .addr = i * sizeof(Bucket)};
      Bucket *bucket = get_bucket_addr(bucket_addr);
      auto lock = &bucket->lock;
      while (bucket != nullptr) {
        for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
          if (bucket->tos[j].addr.check()) count_elem++;
        }
        count_bucket++;
        bucket = get_bucket_addr(bucket->next);
      }
    }
    std::cout << "Elem: " << count_elem << " Buckets:" << count_bucket
              << " Load fator: " << count_elem * 1.0 / (count_bucket * 3) * 100 << "% "
              << load_factor.load() << std::endl;
    std::cout << "Num buckets: " << load_factor.load() << std::endl;
    std::cout << "Memory Consumption:" << load_factor.load() * sizeof(Bucket) / 1024 / 1024 << " MB"
              << std::endl;
    size_t count = 0;
    for (size_t i = 0; i < MAX_PAGE_NUM; i++) {
      if (PAGE_TABLE[i] != nullptr) {
        count++;
      }
    }
    std::cout << "Hashtable Page Count:" << count << " Total size:" << count * HASHTABLE_PAGE_SIZE
              << std::endl;
  }

  void visit_all() {
    size_t total = 0;
    size_t missed = 0;
    size_t valid = 0;
    for (size_t i = 0; i < num_buckets; i++) {
      BucketAddr bucket_addr = BucketAddr{.valid = 1, .addr = i * sizeof(Bucket)};
      Bucket *bucket = get_bucket_addr(bucket_addr);
      assert(bucket != nullptr);
      auto lock = &bucket->lock;
      while (bucket != nullptr) {
        for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
          if (bucket->tos[j].addr.check()) {
            valid++;
            auto _addr = bucket->tos[j].addr.addr;
            Page::Page *page = nullptr;
            page = PageTable::PAGE_TABLE.get(_addr / config.PAGE_SIZE);
            if (page == nullptr) std::cerr << "Page is null!" << std::endl;
            size_t offset = _addr % config.PAGE_SIZE;
            char *paddr = page->addr + offset;
            auto l = reinterpret_cast<Entry::Entry *>(page->addr + offset);
            if (bucket->tos[j].khash != hash_func(l->get_key_addr(), l->key_len))
              std::cerr << "Hash mismatch!" << std::endl;
            total++;
          }
        }
        bucket = get_bucket_addr(bucket->next);
      }
    }
    std::cout << "valid:" << valid << " total:" << total << " missed:" << missed << std::endl;
  }

  std::pair<BucketAddr, Bucket *> new_bucket(size_t &offset_in_page, size_t &offset_of_page) {
    new_bucket_lck.lock();
    auto r = offset_in_page;
    offset_in_page += sizeof(Bucket);
    if (offset_in_page > HASHTABLE_PAGE_SIZE) {
      offset_in_page = sizeof(Bucket);
      r = 0;
      offset_of_page += 1;
      PAGE_TABLE[offset_of_page] = (char *)Allocator::allocator_bucket->alloc(
          HASHTABLE_PAGE_SIZE, "Bucket_" + std::to_string(offset_of_page));
    }
    auto offset = offset_of_page * HASHTABLE_PAGE_SIZE + r;
    auto addr = PAGE_TABLE[offset_of_page] + r;
    memset(addr, 0, sizeof(Bucket));
    new_bucket_lck.unlock();
    load_factor++;

    return {BucketAddr{.valid = 1, .addr = offset}, reinterpret_cast<Bucket *>(addr)};
  }

  Bucket *get_bucket_addr(BucketAddr _offset) {
    if (_offset.valid == 0) return nullptr;
    size_t offset = _offset.addr;
    if (offset % sizeof(Bucket) != 0) return nullptr;
    size_t pageid = offset / HASHTABLE_PAGE_SIZE;
    size_t pageoff = offset % HASHTABLE_PAGE_SIZE;
    if (pageid >= MAX_PAGE_NUM) return nullptr;
    if (PAGE_TABLE[pageid] == nullptr) return nullptr;
    return reinterpret_cast<Bucket *>(PAGE_TABLE[pageid] + offset % HASHTABLE_PAGE_SIZE);
  }

  size_t hash_to_bucket(size_t key, size_t _num_buckets) { return key & (_num_buckets - 1); }

  OP_states move(Page::Page *src, size_t loc, size_t &size, Page::PageType type) {
    OP_states r{OP_states::FAIL};
    auto l = reinterpret_cast<Entry::Entry *>(src->addr + loc);
    auto khash = hash_func(l->get_key_addr(), l->key_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};
    Bucket *bucket = get_bucket_addr(_curr_bucket);
    auto &lock = bucket->lock;
    lock.writeLock();
    src->readLock(loc);
    while (bucket != nullptr) {
      for (size_t i = 0; i < ENTRIES_PER_BUCKET; i++) {
        if (bucket->tos[i].khash == khash) {
          auto src_addr = Entry::Entry_ADDR(src->ID * config.PAGE_SIZE + loc);
          if (src_addr != bucket->tos[i].addr) {
            goto RETURN;
          }

          if (!l->check_ttl()) {
            bucket->tos[i].addr.invalidate();
            bucket->tos[i].khash = 0;
            goto RETURN;
          }
          auto _size = l->get_entry_size();
          Entry::Entry_ADDR addr;
          if (type == Page::PageType::DRAM)
            addr = MemoryManager::MM->move_alloc_DRAM(_size);
          else if (type == Page::PageType::eSM) {  // addr = MemoryManager::MM->alloc_eSM(_size);
            std::runtime_error("eSM allocation is not supported in move operation.");
          }

          Entry::Entry::write(addr, l->get_key_addr(), l->key_len, l->get_value_addr(),
                              l->value_len, l->endtime, true);
          bucket->tos[i].addr = addr;
          size = _size;
          r = OP_states::SUCC;
          goto RETURN;
        }
      }
      bucket = get_bucket_addr(bucket->next);
    }
  RETURN:
    src->readUnlock(loc);
    lock.writeUnlock();
    return r;
  }
  OP_states recover_index(char *key, size_t k_len, Entry::Entry_ADDR addr) {
    auto khash = hash_func(key, k_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};
    Bucket *bucket = get_bucket_addr(_curr_bucket);

    auto &lock = bucket->lock;
    lock.writeLock();

    bucket_entry *empty = nullptr;
    OP_states succeed = OP_states::FAIL;
    bool exist = false;
    while (bucket != nullptr) {
      for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
        if (khash == bucket->tos[j].khash) {
          empty = &bucket->tos[j];
          bucket->tos[j].addr.count = 0;
          exist = true;
          break;
        } else if (!bucket->tos[j].addr.check()) {
          empty = &bucket->tos[j];
        }
      }
      if (bucket->next.valid == 0 || exist) {
        if (empty == NULL) {
          auto b = new_bucket(OFFSET_IN_PAGE_chain, OFFSET_OF_PAGE_chain);
          bucket->next = b.first;
          _curr_bucket = bucket->next;
          empty = &b.second->tos[0];
        }
        empty->set(khash, addr);
        succeed = OP_states::SUCC;
        goto RETURN;
      }
      if (succeed == OP_states::SUCC) {
        break;
      } else {
        _curr_bucket = bucket->next;
        bucket = get_bucket_addr(bucket->next);
      }
    }
  RETURN:
    lock.writeUnlock();
    return succeed;
  }
  OP_states remove(const char *key, const size_t &k_len) {
    OP_states r{OP_states::FAIL};
    auto khash = hash_func(key, k_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};
    Bucket *bucket = get_bucket_addr(_curr_bucket);
    auto &lock = bucket->lock;
    lock.writeLock();
    while (bucket != nullptr) {
      for (size_t i = 0; i < ENTRIES_PER_BUCKET; i++) {
        if (bucket->tos[i].khash == khash) {
          Entry::Entry::remove(key, k_len, bucket->tos[i].addr);
          bucket->tos[i].addr.invalidate();
          bucket->tos[i].khash = 0;
          r = OP_states::SUCC;
          goto RETURN;
        }
      }
      bucket = get_bucket_addr(bucket->next);
    }
  RETURN:
    lock.writeUnlock();
    return r;
  }

  OP_states set(const char *key, const size_t &k_len, const char *value, const size_t &v_len,
                const uint32_t &etime) {
    auto khash = hash_func(key, k_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};
    Bucket *bucket = get_bucket_addr(_curr_bucket);

    auto &lock = bucket->lock;
    lock.writeLock();

    bucket_entry *empty = nullptr;
    bool *empty_state = nullptr;
    OP_states succeed = OP_states::FAIL;
    bool exist = false;
    while (bucket != nullptr) {
      for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
        if (khash == bucket->tos[j].khash) {
          Entry::Entry::remove(key, k_len, bucket->tos[j].addr);
          empty = &bucket->tos[j];
          bucket->tos[j].addr.invalidate();
          exist = true;
          break;
        } else if (!bucket->tos[j].addr.check()) {
          empty = &bucket->tos[j];
        }
      }
      if (bucket->next.valid == 0 || exist) {
        if (empty == NULL) {
          auto b = new_bucket(OFFSET_IN_PAGE_chain, OFFSET_OF_PAGE_chain);
          bucket->next = b.first;
          _curr_bucket = bucket->next;
          empty = &b.second->tos[0];
        }
        Entry::Entry_ADDR addr = MemoryManager::MM->alloc(k_len + v_len + sizeof(Entry::Entry));
        Entry::Entry::write(addr, key, k_len, value, v_len, etime);
        empty->set(khash, addr);

        succeed = OP_states::SUCC;
        goto RETURN;
      }
      if (succeed == OP_states::SUCC) {
        break;
      } else {
        _curr_bucket = bucket->next;
        bucket = get_bucket_addr(bucket->next);
      }
    }
  RETURN:
    lock.writeUnlock();
    return succeed;
  }
  OP_states add(const char *key, const size_t &k_len, const char *value, const size_t &v_len,
                const uint32_t &etime) {
    auto khash = hash_func(key, k_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};

    Bucket *bucket = get_bucket_addr(_curr_bucket);

    auto &lock = bucket->lock;
    lock.writeLock();

    bucket_entry *empty = nullptr;
    bool *empty_state = nullptr;
    OP_states succeed = OP_states::FAIL;

    bool exist = false;
    while (bucket != nullptr) {
      bool found = true;
      for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
        if (khash == bucket->tos[j].khash) {
          auto r = Entry::Entry::compare(key, k_len, bucket->tos[j].addr);
          if (r == OP_states::SUCC)
            goto RETURN;
          else {
            empty = &bucket->tos[j];
            found = false;
            break;
          }
        } else if (!bucket->tos[j].addr.check()) {
          empty = &bucket->tos[j];
        }
      }
      if (bucket->next.valid == 0 || !found) {
        if (empty == NULL) {
          auto b = new_bucket(OFFSET_IN_PAGE_chain, OFFSET_OF_PAGE_chain);
          bucket->next = b.first;
          _curr_bucket = b.first;
          empty = &b.second->tos[0];
        }
        Entry::Entry_ADDR addr = MemoryManager::MM->alloc(k_len + v_len + sizeof(Entry::Entry));
        Entry::Entry::write(addr, key, k_len, value, v_len, etime);
        empty->set(khash, addr);
        succeed = OP_states::SUCC;
        goto RETURN;
      }
      _curr_bucket = bucket->next;
      bucket = get_bucket_addr(bucket->next);
    }
  RETURN:
    lock.writeUnlock();
    return succeed;
  }

  OP_states get(const char *key, const size_t &k_len, char *value, size_t &value_len,
                size_t _num_buckets) {
    auto khash = hash_func(key, k_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};
    Bucket *bucket = get_bucket_addr(_curr_bucket);

    auto lock = &bucket->lock;
    lock->readLock();
    OP_states r = OP_states::NOT_FOUND;
    do {
      for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
        if (khash == bucket->tos[j].khash) {
          Entry::Entry_ADDR &addr = bucket->tos[j].addr;
          if (!addr.check()) {
            bucket->tos[j].khash = 0;
            bucket->tos[j].addr.invalidate();
            goto RETURN;
          }
          r = Entry::Entry::get(key, k_len, value, value_len, khash, addr);
          if (r == OP_states::NOT_FOUND) {
            bucket->tos[j].addr.invalidate();
            bucket->tos[j].khash = 0;
            goto RETURN;
          } else if (r == OP_states::TTL_OUT) {
            bucket->tos[j].khash = 0;
            bucket->tos[j].addr.invalidate();
            goto RETURN;
          } else if (r == OP_states::SUCC) {
            goto RETURN;
          } else {
            goto RETURN;
          }
        }
      }
      bucket = get_bucket_addr(bucket->next);
    } while (bucket != nullptr);
  RETURN:
    lock->readUnlock();
    return r;
  }
  int get_access_count(const char *key, const size_t &k_len) {
    auto khash = hash_func(key, k_len);
    size_t bin = hash_to_bucket(khash, num_buckets);
    BucketAddr _curr_bucket = BucketAddr{.valid = 1, .addr = bin * sizeof(Bucket)};
    Bucket *bucket = get_bucket_addr(_curr_bucket);
    OP_states r = OP_states::NOT_FOUND;
    do {
      for (size_t j = 0; j < ENTRIES_PER_BUCKET; j++) {
        if (khash == bucket->tos[j].khash) {
          return bucket->tos[j].addr.count;
        }
      }
      bucket = get_bucket_addr(bucket->next);
    } while (bucket != nullptr);
    return 0;
  }

 private:
  // void snapshot(Bucket *bucket, const BucketAddr &bucket_addr) {
  //   if (!config.SNAPSHOT) return;
  //   for (size_t i = 0; i < ENTRIES_PER_BUCKET; i++) {
  //     if (!bucket->state[i]) return;
  //   }
  //   for (size_t i = 0; i < ENTRIES_PER_BUCKET; i++) {
  //     bucket->state[i] = false;
  //   }
  //   persist(bucket_addr);
  // }
  // void persist(BucketAddr _bucket_addr) {
  //   size_t bucket_addr = _bucket_addr.addr;
  //   char *dst = PPAGE_TABLE[bucket_addr / HASHTABLE_PAGE_SIZE] + bucket_addr %
  //   HASHTABLE_PAGE_SIZE; char *src = DPAGE_TABLE[bucket_addr / HASHTABLE_PAGE_SIZE] +
  //   bucket_addr % HASHTABLE_PAGE_SIZE; memcpy(dst, src, sizeof(Bucket));
  // }
  std::atomic_size_t load_factor;
  spinlock new_bucket_lck;
  size_t num_buckets;
  Bucket *PM_table;
  size_t OFFSET_OF_PAGE_chain;
  size_t OFFSET_IN_PAGE_chain;
  size_t OFFSET_OF_PAGE_main;
  size_t OFFSET_IN_PAGE_main;
  std::thread check_ttl_thread;
  std::thread resize_main_thread;
  char *PAGE_TABLE[MAX_PAGE_NUM];
};

namespace CacheStrategy {
void get_page_info(Page::Page *_reclaim, ItemsCollection *items, Hashtable *hashtable) {
  auto head = reinterpret_cast<Page::PageHead *>(_reclaim->addr);
  size_t total_access = 0;
  size_t kv_count = 0;
  size_t data_size = _reclaim->get_data_size();
  for (size_t _loc = _reclaim->get_head_size();
       _loc < data_size && _loc + sizeof(Entry::Entry) < config.PAGE_SIZE;) {
    Entry::Entry *curr = reinterpret_cast<Entry::Entry *>(_reclaim->addr + _loc);
    size_t c = hashtable->get_access_count(curr->get_key_addr(), curr->key_len);

    if (curr->check_ttl()) {
      items->current_block.kvs.emplace_back(
          KeyValue{.offset = _loc,
                   .accessCount = c,
                   .size = static_cast<size_t>(curr->key_len + curr->value_len)});
    } else {
      items->cold_items.emplace_back(_loc);
    }
    total_access += c;
    kv_count++;
    _loc += curr->get_entry_size();
  }
  items->current_block.kvCount = kv_count;
  items->current_block.totalAccessCount = total_access;
}

ItemsCollection get_hot_items(Page::Page *_reclaim, Hashtable *hashtable) {
  ItemsCollection items;
  get_page_info(_reclaim, &items, hashtable);
  if (items.current_block.kvCount == 0) return items;
  cacherecycler.processCurrentBlock(&items);
  return items;
}
void reclaim_eSM(Page::Page *p, Hashtable *hashtable) {
  if (p != nullptr) {
    ItemsCollection items = get_hot_items(p, hashtable);
    auto hot_items = items.hot_items;
    if (!hot_items.empty()) {
      for (auto &&loc : hot_items) {
        auto l = reinterpret_cast<Entry::Entry *>(p->addr + loc);
        if (!l->check_ttl()) {
          hashtable->remove(l->get_key_addr(), l->key_len);
          continue;
        }
        size_t size = 0;
        hashtable->move(p, loc, size, Page::PageType::DRAM);
      }
    }
    auto cold_items = items.cold_items;
    if (!cold_items.empty()) {
      for (auto &&loc : cold_items) {
        auto l = reinterpret_cast<Entry::Entry *>(p->addr + loc);
        hashtable->remove(l->get_key_addr(), l->key_len);
      }
    }
    // PageTable::PAGE_TABLE.erase(p->ID);
    PageManager::Page_Manager.free_page(p);
  }
}

}  // namespace CacheStrategy

class CommandMonitor {
 public:
  CommandMonitor(const std::vector<std::string> &command) : command_(command), is_running_(false) {}

  ~CommandMonitor() { stop(); }

  void start() {
    is_running_ = true;
    monitor_thread_ = std::thread(&CommandMonitor::monitorLoop, this);
  }

  void stop() {
    if (!is_running_) return;
    is_running_ = false;
    if (monitor_thread_.joinable()) monitor_thread_.join();
  }

  std::string getLastLine() {
    std::lock_guard<std::mutex> lock(line_mutex_);
    return last_line_;
  }

 private:
  void monitorLoop() {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
      std::cerr << "Error creating pipe: " << std::strerror(errno) << std::endl;
      return;
    }

    pid_t pid = fork();
    if (pid == -1) {
      std::cerr << "Error forking process: " << std::strerror(errno) << std::endl;
      close(pipefd[0]);
      close(pipefd[1]);
      return;
    }

    if (pid == 0) {
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);

      std::vector<char *> argv;
      for (const auto &arg : command_) {
        argv.push_back(const_cast<char *>(arg.c_str()));
      }
      argv.push_back(nullptr);

      execvp(argv[0], argv.data());
      std::cerr << "Error executing command: " << std::strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }

    close(pipefd[1]);
    char buffer[256];
    bool sleep = false;
    while (is_running_) {
      ssize_t bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1);
      if (bytes_read > 0) {
        std::lock_guard<std::mutex> lock(line_mutex_);
        last_line_.assign(buffer, bytes_read);
        if (last_line_.find("Total Mem Bw (GB/s)") != std::string::npos) sleep = true;
      }
      if (sleep) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
  }

  std::vector<std::string> command_;
  std::string last_line_;
  std::thread monitor_thread_;
  std::atomic<bool> is_running_;
  std::mutex line_mutex_;
};

inline BWMonitor parse_bandwidth_output(const std::string &input, char delimiter = ',') {
  std::vector<std::string> result;
  size_t start = 0;
  size_t end = input.find(delimiter);

  while (end != std::string::npos) {
    std::string token = input.substr(start, end - start);
    if (!token.empty()) result.push_back(token);
    start = end + 1;
    end = input.find(delimiter, start);
  }

  if (result.size() >= 8) {
    try {
      return BWMonitor{std::stof(result[1]), std::stof(result[2]), std::stof(result[6]),
                       std::stof(result[7])};
    } catch (const std::invalid_argument &e) {
      return BWMonitor{-1.0f, -1.0f, -1.0f, -1.0f};
    }
  }
  return BWMonitor{-1.0f, -1.0f, -1.0f, -1.0f};
}

inline std::vector<std::pair<double, double>> test_cxl_avg_latency_by_read_size(
    std::string AMDuProf_Path = "/root/AMDuProf_Linux_x64_5.1.701/bin/AMDuProfPcm") {
  int test_duration_sec = 3;
  constexpr size_t read_size = CACHELINE_SIZE;
  constexpr int max_reader_threads = 32;
  size_t buffer_size = 16ULL * 1024 * 1024 * 1024;
  std::vector<std::pair<double, double>> results;
  char *buffer = nullptr;
  bool use_esm_allocator = false;
  std::vector<std::string> command = {AMDuProf_Path, "-m", "cxl", "-m", "memory", "-d", "6000"};
  CommandMonitor monitor(command);

  if (Allocator::allocator_eSM != nullptr) {
    buffer = static_cast<char *>(Allocator::allocator_eSM->alloc(buffer_size, "cxl_latency_test"));
    use_esm_allocator = true;
  } else {
    assert(false &&
           "eSM allocator is not initialized. Please initialize it before running the test.");
  }
  if (buffer == nullptr) return results;
  std::memset(buffer, 0xAB, buffer_size);

  std::cout << "\n=== CXL Avg Latency vs Thread Count ===" << std::endl;
  std::cout << "threads, read_size(bytes), BWMonitor.eSM_READ_BW(GB/s), avg_latency(ns)"
            << std::endl;
  monitor.start();

  size_t thread_buffer_size = buffer_size / static_cast<size_t>(max_reader_threads);
  thread_buffer_size = (thread_buffer_size / CACHELINE_SIZE) * CACHELINE_SIZE;
  assert(thread_buffer_size >= read_size &&
         "buffer_size / max_reader_threads must be >= read_size");

  for (int reader_threads = 1; reader_threads <= max_reader_threads; reader_threads += 2) {
    std::atomic_bool stop{false};
    std::vector<std::thread> workers;
    workers.reserve(reader_threads);

    for (int tid = 0; tid < reader_threads; ++tid) {
      workers.emplace_back([&, tid]() {
        char *thread_buffer = buffer + static_cast<size_t>(tid) * thread_buffer_size;
        size_t offset = 0;
        uint64_t sink = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          if (offset + read_size >= thread_buffer_size) offset = 0;
          for (size_t inner = 0; inner < read_size; inner += CACHELINE_SIZE) {
            sink += *reinterpret_cast<volatile uint64_t *>(thread_buffer + offset + inner);
          }
          offset += read_size;
        }
      });
    }

    double latency_sum = 0.0;
    size_t sample_count = 0;
    double bandwidth_sum = 0.0;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
               .count() < test_duration_sec * 1000) {
      latency_sum += measure_cxl_load_latency();
      ++sample_count;
      usleep(10000);
    }
    BWMonitor bw = parse_bandwidth_output(monitor.getLastLine());

    stop.store(true, std::memory_order_relaxed);
    for (auto &worker : workers) {
      if (worker.joinable()) worker.join();
    }

    double avg_latency = sample_count == 0 ? 0.0 : latency_sum / sample_count;

    results.push_back({bw.eSM_READ_BW, avg_latency});
    std::cout << reader_threads << ", " << read_size << ", " << bw.eSM_READ_BW << ", "
              << avg_latency << std::endl;
  }

  monitor.stop();

  if (use_esm_allocator && Allocator::allocator_eSM != nullptr) {
    Allocator::allocator_eSM->free(buffer, buffer_size);
  } else {
    delete[] buffer;
  }
  return results;
}

inline std::atomic_bool finished{false};

// CXL延迟测试实例（可选）
// inline Hemu_Cache::CXLLatencyTester* cxl_latency_tester = nullptr;

void guard(Hashtable *hashtable) {
  Timer::Timer t;
  t.start();

  std::vector<std::string> command = {"/root/AMDuProf_Linux_x64_5.1.701/bin/AMDuProfPcm",
                                      "-m",
                                      "cxl",
                                      "-m",
                                      "memory",
                                      "-d",
                                      "6000"};
  CommandMonitor monitor(command);
  monitor.start();
  std::ofstream ofs("bandwidth_log.txt");
  while (!finished.load()) {
    if (t.elapsed<std::chrono::milliseconds>() > config.Balancer_Interval) {
      t.start();
      PageManager::Page_Manager.update_dram_percent();
      auto bw = parse_bandwidth_output(monitor.getLastLine());
      auto cdp = config.DRAM_PERCENT;
      ofs << bw.DRAM_READ_BW << " " << bw.eSM_READ_BW << " "<<bw.eSM_WRITE_BW<<" "
          << PageManager::Page_Manager.get_load_latency_average() << " " << cdp << std::endl;

      // move esm page to dram.
      {
        Page::Page *p = PageManager::Page_Manager.promote_esm_page();
        if (p != nullptr) {
          ThreadPool::THREAD_POOL->enqueue(CacheStrategy::promote_esm_page, p);
        }
      }
    }

    // move page from dram to esm
    {
      Page::Page *p = PageManager::Page_Manager.pop_DRAM();
      if (p != nullptr) {
        ThreadPool::THREAD_POOL->enqueue(CacheStrategy::archive_dram_page, p);
      }
    }

    // reclaim esm page and move hot items to dram
    {
      Page::Page *p = PageManager::Page_Manager.pop_eSM();
      if (p != nullptr) {
        ThreadPool::THREAD_POOL->enqueue(CacheStrategy::reclaim_eSM, p, hashtable);
      }
    }
  }
  ThreadPool::THREAD_POOL->shutdown();
}

class Hemu {
 private:
 public:
  Hemu() {
    config.CPU_CORE_NUM = std::thread::hardware_concurrency();
    ThreadPool::THREAD_POOL = new ThreadPool::ThreadPool(config.CPU_CORE_NUM);
    config.PAGE_NUM = config.CacheCapacity / config.PAGE_SIZE;
    PageManager::Page_Manager.update_max_page_num();
    // Allocator init
    Allocator::allocator_DRAM = Allocator::new_alloctor(AllocatorType::DRAM);  // DRAM allocator
    Allocator::allocator_eSM = Allocator::new_alloctor(
        config.esm_allocator_type);  // eSM allocator, FILE mode or NUMA mode. Change to DRAM if
                                     // you want to test on DRAM

    Allocator::allocator_bucket = Allocator::new_alloctor(
        config.hashtable_allocator);  // Place hash table on eSM if you use CXL memory; Place
                                      // hash table on DRAM if you use PMem/CXL NVM device

    // std::cout << "CXL latency:" << measure_cxl_load_latency << std::endl;
    // test_cxl_avg_latency_by_read_size();

    hashtable = new Hashtable();
    if (std::filesystem::exists(config.FILE_POOL_DIR) &&
        std::filesystem::is_directory(config.FILE_POOL_DIR)) {
      ThreadPool::ThreadPool threads(64);
      Timer::Timer t;
      t.start();
      for (const std::filesystem::directory_entry &entry :
           std::filesystem::directory_iterator(config.FILE_POOL_DIR)) {
        if (std::filesystem::is_regular_file(entry.status())) {
          std::string filename = entry.path().filename().string();
          if (filename.find("Page_") != std::string::npos) {
            Page::Page *page = nullptr;
            if (Allocator::allocator_eSM->get_allocator_type() == AllocatorType::FILE) {
              char *addr = static_cast<char *>(
                  Allocator::allocator_eSM->recover(entry.path().string(), config.PAGE_SIZE));
              page = PageManager::Page_Manager.recover_page(addr);
            } else {
              page = PageManager::Page_Manager.recover_from_page(entry.path());
            }
            if (page != nullptr && config.hashtable_allocator == AllocatorType::DRAM) {
              Page::PAGE_ID = Page::PAGE_ID.load() < page->ID ? page->ID + 1 : Page::PAGE_ID.load();
              hashtable->recover(page);
            }
          }
        }
      }
      // PageManager::Page_Manager.sort_eSM_page_list();
      std::cout << "Recovered " << PageManager::Page_Manager.eSM_page_list.size()
                << " Pages cost: " << t.elapsed<std::chrono::milliseconds>() / 1000.0 << " s"
                << std::endl;
    }
    hashtable->print_load_factor();
    hashtable->visit_all();

    MemoryManager::MM = new MemoryManager::MemoryManagerGlobal();
    guard_thread = std::thread(guard, hashtable);
    std::cout << "config.CacheCapacity: " << config.CacheCapacity << std::endl;
    std::cout << "config.CPU_CORE_NUM: " << config.CPU_CORE_NUM << std::endl;
    std::cout << "config.PAGE_SIZE: " << config.PAGE_SIZE << std::endl;
    std::cout << "config.PAGE_NUM: " << config.PAGE_NUM << std::endl;
    std::cout << "config.CacheStrategy: " << config.CacheStrategy << std::endl;
    std::cout << "config.Balancer_on: " << config.Balancer_ON << std::endl;
    std::cout << "config.Balancer_Interval: " << config.Balancer_Interval << std::endl;
    std::cout << "sizeof(Entry_ADDR): " << sizeof(Entry::Entry_ADDR) << std::endl;
    std::cout << "sizeof(Bucket):" << sizeof(Bucket) << std::endl;
    std::cout << "sizeof(Entry):" << sizeof(Entry::Entry) << std::endl;
    std::cout << "sizeof(Lock<ReadWriteLock>):" << sizeof(Lock<ReadWriteLock>) << std::endl;
  }
  void print() {
    // hashtable->print_load_factor();
    // PageTable::PAGE_TABLE.print();
  }
  ~Hemu() {
    print_DRAM_usage();
    std::cout << "Hemu shutting down..." << std::endl;
    std::cout << "Final Page List State:" << std::endl;
    std::cout << "----------------------" << std::endl;
    // PageManager::Page_Manager.print();

    finished.store(true);
    if (guard_thread.joinable()) guard_thread.join();
    hashtable->print_load_factor();
    // PageTable::PAGE_TABLE.print();
    print_DRAM_usage();
    if (hashtable != nullptr) delete hashtable;
    print_DRAM_usage();
    if (MemoryManager::MM != nullptr) delete MemoryManager::MM;
    if (ThreadPool::THREAD_POOL != nullptr) delete ThreadPool::THREAD_POOL;
    if (Allocator::allocator_DRAM != nullptr) delete Allocator::allocator_DRAM;
    if (Allocator::allocator_eSM != nullptr) delete Allocator::allocator_eSM;
    if (Allocator::allocator_bucket != nullptr) delete Allocator::allocator_bucket;
    print_DRAM_usage();
    std::cout << "Hemu shut down successfully." << std::endl;
  }

  void update_time(size_t time) {
    if (time > current_time.load()) current_time.store(time);
  }
  size_t get_time() { return current_time.load(); }

  bool set(char *key, size_t k_len, char *value, size_t v_len, uint32_t etime) {
    static thread_local size_t set_counter = 0;
    auto r = hashtable->set(key, k_len, value, v_len, etime);
    return r == OP_states::SUCC ? true : false;
  }

  bool add(char *key, size_t k_len, char *value, size_t v_len, uint32_t etime) {
    auto r = hashtable->add(key, k_len, value, v_len, etime);
    return r == OP_states::SUCC ? true : false;
  }
  int get(char *key, size_t k_len, char *value, size_t &value_len) {
    auto r = hashtable->get(key, k_len, value, value_len, hashtable->num_buckets);
    if (r == OP_states::SUCC)
      global_counter.hit_count_inc();
    else
      global_counter.miss_count_inc();
    return static_cast<int>(r);
  }

  void print_load_factor() { hashtable->print_load_factor(); }
  void wait() {}

 private:
  Hashtable *hashtable;
  std::thread guard_thread;
};
}  // namespace Hemu_Cache