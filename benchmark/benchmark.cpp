#include "benchmark.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "Hemu.h"
#include "timer.h"

// segcache
extern "C" {
#include "Segcache.h"
}

// plush
#include "Hashtable.h"

// viper
#include "viper.hpp"

// PaperCache
#include "client.hpp"

// redis
#include <hiredis/hiredis.h>

// memcached
#include <libmemcached/memcached.h>

// cachelib
#include "cachelib/allocator/CacheAllocator.h"

#include <libpmem.h>

// timer
#include "timer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr bool LATENCY_E = false;
constexpr bool MISS_MONITOR = false;

int eSM = 2;                              // Mode 1:File/DAX, 2:NUMA
std::string POOL_PATH = "/mnt/cxlpool/";  // If mode=1
int CXL_NODE = 1;                         // If mode=2

static const size_t MAX_THREAD = 128;
char **value;
size_t THREADS_NUM;

class TwitterItem {
 public:
  void *addr;
  explicit TwitterItem(void *addr) : addr(addr) {}
  op_type get_op() const { return op_type(reinterpret_cast<twitter_workload *>(addr)->op); }
  uint32_t get_timestamp() const { return reinterpret_cast<twitter_workload *>(addr)->timestamp; }
  uint32_t get_ttl() const { return reinterpret_cast<twitter_workload *>(addr)->ttl; }
  uint32_t get_key_len() const { return reinterpret_cast<twitter_workload *>(addr)->key_len; }
  uint32_t get_value_len() const { return reinterpret_cast<twitter_workload *>(addr)->value_len; }
  char *get_key_addr() const { return reinterpret_cast<twitter_workload *>(addr)->get_key_addr(); }
};

class YCSBItem {
 public:
  void *addr;
  explicit YCSBItem(void *addr) : addr(addr) {}
  op_type get_op() const {
    // YCSB only encodes GET (0) / ADD (1); default to ADD for anything else
    // so the function has no fall-through-without-return path.
    auto curr = reinterpret_cast<ycsb_workload *>(addr);
    return curr->op == 0 ? op_type::GET : op_type::ADD;
  }
  uint32_t get_timestamp() const { return 0; }             // YCSB workload has no timestamp
  uint32_t get_ttl() const { return 1024 * 1024; }         // Default TTL for YCSB workload
  uint32_t get_key_len() const { return sizeof(size_t); }  // Key is a size_t in YCSB
  uint32_t get_value_len() const { return reinterpret_cast<ycsb_workload *>(addr)->value_len; }
  char *get_key_addr() const {
    return reinterpret_cast<char *>(&(reinterpret_cast<ycsb_workload *>(addr)->key));
  }
};

using WorkloadItem = std::variant<TwitterItem, YCSBItem>;

inline op_type item_op(const WorkloadItem &item) {
  return std::visit([](const auto &it) { return it.get_op(); }, item);
}
inline char *item_key_addr(const WorkloadItem &item) {
  return std::visit([](const auto &it) { return it.get_key_addr(); }, item);
}
inline uint32_t item_key_len(const WorkloadItem &item) {
  return std::visit([](const auto &it) { return it.get_key_len(); }, item);
}
inline uint32_t item_timestamp(const WorkloadItem &item) {
  return std::visit([](const auto &it) { return it.get_timestamp(); }, item);
}
inline uint32_t item_value_len(const WorkloadItem &item) {
  return std::visit([](const auto &it) { return it.get_value_len(); }, item);
}
inline uint32_t item_ttl(const WorkloadItem &item) {
  return std::visit([](const auto &it) { return it.get_ttl(); }, item);
}

class Workload {
 protected:
  int thread_num;
  float start;
  float end;
  int fd = -1;
  char *addr = nullptr;
  size_t len = 0;
  std::vector<size_t> run_data;

 public:
  std::string datafile;
  virtual std::string get_workload_name() { return datafile; }
  size_t get_item_count() { return run_data.size(); }
  Workload(std::string workload, int _thread_num, float _start, float _end)
      : datafile(workload), thread_num(_thread_num), start(_start), end(_end) {
    value = new char *[MAX_THREAD];
    for (size_t i = 0; i < MAX_THREAD; i++) {
      value[i] = new char[4 * 1024 * 1024];
      for (size_t j = 0; j < 4 * 1024 * 1024; j++) {
        value[i][j] = 'A' + j % 26;
      }
    }
  }

  virtual std::optional<WorkloadItem> get_next_run(int thread_id) = 0;
  void open_file() {
    fd = open(datafile.c_str(), O_RDONLY);
    len = lseek(fd, 0, SEEK_END);
    std::cout << "Data File:" << datafile << " Data file size: " << len << std::endl;

    if (fd < 0) {
      perror("fd is invalid");
      exit(EXIT_FAILURE);
    }
    if (len == 0) {
      fprintf(stderr, "len must be greater than 0\n");
      exit(EXIT_FAILURE);
    }

    addr = (char *)mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);

    if (addr == MAP_FAILED) {
      perror("mmap failed");
      close(fd);
      exit(EXIT_FAILURE);
    }

    int madv_ret = madvise(addr, len, MADV_HUGEPAGE | MADV_SEQUENTIAL);
    if (madv_ret == -1) {
      perror("madvise failed");
      fprintf(stderr, "warning: madvise failed, continue...\n");
    }
  }
  virtual ~Workload() {
    if (addr != nullptr) {
      munmap(addr, len);
      close(fd);
    }
    run_data.clear();
    for (size_t i = 0; i < MAX_THREAD; i++) {
      delete[] value[i];
    }
    delete[] value;
  }
};

class Twitter : public Workload {
 public:
  Twitter(std::string workload, int _thread_num, float _start = 0, float _end = 1)
      : Workload(workload, _thread_num, _start, _end) {
    open_file();
    size_t offset = 0;
    size_t w_count = 0;
    size_t r_count = 0;
    while (offset < len) {
      auto curr = addr + offset;
      auto st = reinterpret_cast<twitter_workload *>(curr);
      run_data.emplace_back(offset);

      offset += sizeof(twitter_workload) + st->key_len;
    }
  }
  std::string get_workload_name() override {
    size_t pos = datafile.find("cluster");
    if (pos != std::string::npos) {
      return "C" + datafile.substr(pos + 7);
    }
    return datafile;
  }
  std::optional<WorkloadItem> get_next_run(int thread_id) override {
    static thread_local int index = -thread_id + start * run_data.size();
    index += thread_num;

    int p = run_data.size() / 100;
    // if (p != 0 && index % p == 0) printf("Progress[%.2f]\n", index * 1.0 / run_data.size());
    if (index < run_data.size() && index < end * run_data.size()) {
      return TwitterItem(addr + run_data[index]);
    }
    return std::nullopt;
  }
};

class YCSB : public Workload {
 public:
  YCSB(std::string workload, int _thread_num, float _start = 0, float _end = 1)
      : Workload(workload, _thread_num, _start, _end) {
    open_file();
    size_t offset = 0;
    while (offset < len) {
      auto curr = addr + offset;
      run_data.emplace_back(offset);
      offset += sizeof(ycsb_workload);
    }
  }

  std::string get_workload_name() override {
    size_t pos = datafile.find("workload");
    if (pos != std::string::npos) {
      char w = datafile.back();
      if (w >= 'a' && w <= 'z') w = w - 'a' + 'A';
      return "YCSB-" + std::string(1, w);
    }
    return datafile;
  }

  std::optional<WorkloadItem> get_next_run(int thread_id) override {
    static thread_local int index = -thread_id + start * run_data.size();
    index += thread_num;
    int p = run_data.size() / 100;
    // if (p != 0 && index % p == 0) printf("Progress[%.2f]\n", index * 1.0 / run_data.size());

    if (index < run_data.size() && index < end * run_data.size()) {
      return YCSBItem(addr + run_data[index]);
    }
    return std::nullopt;
  }
};

struct Counter {
  std::atomic_size_t ops_count;
  std::atomic_size_t get_count;
  std::atomic_size_t miss_count;
  std::atomic_size_t set_count;
  std::atomic_size_t add_count;
  std::atomic_size_t hit_count;
  std::atomic_size_t TTL_out_count;
  Counter() { clear(); }
  void clear() {
    ops_count.store(0);
    get_count.store(0);
    miss_count.store(0);
    set_count.store(0);
    add_count.store(0);
    hit_count.store(0);
    TTL_out_count.store(0);
  }
  void add(Counter &counter) {
    ops_count += counter.ops_count;
    get_count += counter.get_count;
    miss_count += counter.miss_count;
    set_count += counter.set_count;
    add_count += counter.add_count;
    hit_count += counter.hit_count;
    TTL_out_count += counter.TTL_out_count;
  }
};

class Cache {
 public:
  Cache() {}
  virtual ~Cache() { std::cout << "Release" << std::endl; }
  virtual void get_instance(int i = 0) = 0;
  virtual void ini(size_t PM_LOG_SIZE, size_t THREADS_NUM,
                   std::string config_file = std::string()) = 0;
  virtual int get(char *key, size_t key_len, char *value, size_t &value_len) = 0;
  virtual bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) = 0;
  virtual bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) = 0;
  virtual void update_time(size_t time) = 0;
  virtual size_t get_time() = 0;
  virtual std::string get_name() = 0;
  virtual void print() {}
};

class Cache_hemu : public Cache {
 public:
  Cache_hemu(size_t capacity, const std::unordered_map<std::string, std::string> &opts = {}) {
    std::cout << "Hemu" << std::endl;
    Hemu_Cache::config.CacheCapacity = capacity;

    // eSM-dependent config: derive allocator type, NUMA node, pool dir and
    // hashtable placement from the benchmark storage mode (eSM).
    switch (eSM) {
      case 1:  // File / DAX (PMem)
        Hemu_Cache::config.esm_allocator_type = Hemu_Cache::AllocatorType::FILE;
        Hemu_Cache::config.FILE_POOL_DIR = POOL_PATH + "Hemu/";
        // PMem/NVM device: keep hash table on DRAM
        break;
      case 2:  // NUMA (CXL)
        Hemu_Cache::config.esm_allocator_type = Hemu_Cache::AllocatorType::NUMA;
        Hemu_Cache::config.eSM_NODE = CXL_NODE;
        // CXL memory: place hash table on eSM (CXL) as well
        break;
      default:
        std::cerr << "Error: invalid eSM mode " << eSM << " (1=File/DAX, 2=NUMA)" << std::endl;
        exit(EXIT_FAILURE);
    }
    std::cout << "  eSM mode=" << eSM
              << " esm_allocator_type=" << Hemu_Cache::config.esm_allocator_type
              << " hashtable_allocator=" << Hemu_Cache::config.hashtable_allocator << std::endl;
    for (auto &[k, v] : opts) {
      if (!Hemu_Cache::config.set_option(k, v)) {
        std::cerr << "Warning: unknown Hemu option '" << k << "'" << std::endl;
      } else {
        std::cout << "  Hemu." << k << " = " << v << std::endl;
      }
    }
    p = new Hemu_Cache::Hemu();
  }

  ~Cache_hemu() {
    std::cout << "Hemu Release" << std::endl;
    delete p;
  }
  void ini(size_t PM_LOG_SIZE, size_t THREADS_NUM, std::string config = std::string()) {}

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    return p->get(key, key_len, value, value_len);
  }
  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return p->add(key, key_len, value, value_len, ttl);
  }
  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return p->set(key, key_len, value, value_len, ttl);
  }
  void update_time(size_t timestamp) { p->update_time(timestamp); }
  size_t get_time() { return p->get_time(); }
  std::string get_name() { return "Hemu"; }
  void get_instance(int i) {
    (void)i;
  }  // Hemu lazily inits per-thread MemoryManager on first alloc
  void print() { p->print(); }

 private:
  Hemu_Cache::Hemu *p;
};

class KV_viper : public Cache {
 public:
  void ini(size_t PM_LOG_SIZE, size_t THREADS_NUM, std::string config_file = std::string()) {
    size_t inital_size = 200ULL * 1024ULL * 1024ULL * 1024ULL;
    std::string index_pool_name = POOL_PATH + "Viper";
    viper::ViperConfig v_config;
    // v_config.enable_reclamation = true;
    if (!std::filesystem::exists(index_pool_name))
      viper_db =
          viper::Viper<std::string, std::string>::create(index_pool_name, inital_size, v_config);
    else
      viper_db = viper::Viper<std::string, std::string>::open(index_pool_name, v_config);
  }

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    std::string _key(key, key_len);
    std::string _value;
    bool r = c->get(_key, &_value);
    return r;
  }
  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    std::string _key(key, key_len);
    std::string _value(value, value_len % 3900);
    c->put(_key, _value);
    return true;
  }
  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return add(key, key_len, value, value_len, ttl);
  }

  std::string get_name() { return "Viper"; }
  size_t get_time() { return 0; }
  void update_time(size_t t) {}
  void get_instance(int i = 0) {
    c = new viper::Viper<std::string, std::string>::Client(viper_db->get_client());
  }
  static thread_local viper::Viper<std::string, std::string>::Client *c;

 private:
  std::unique_ptr<viper::Viper<std::string, std::string>> viper_db;
};
thread_local viper::Viper<std::string, std::string>::Client *KV_viper::c;

class KV_Plush : public Cache {
 public:
  void ini(size_t PM_LOG_SIZE, size_t THREADS_NUM, std::string config_file = std::string()) {
    p = new Hashtable<std::span<const std::byte>, std::span<const std::byte>, PartitionType::Hash>(
        POOL_PATH + "Plush", true);
  }

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    bool r = p->lookup(std::span<std::byte>(reinterpret_cast<std::byte *>(key), key_len),
                       (uint8_t *)value);
    return r;
  }
  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    p->insert(std::span<std::byte>(reinterpret_cast<std::byte *>(key), key_len),
              std::span<std::byte>(reinterpret_cast<std::byte *>(value), value_len));
    return true;
  }
  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return add(key, key_len, value, value_len, ttl);
  }
  std::string get_name() { return "Plush"; }
  size_t get_time() { return 0; }
  void update_time(size_t t) {}
  void get_instance(int i = 0) {}

 private:
  Hashtable<std::span<const std::byte>, std::span<const std::byte>, PartitionType::Hash> *p;
};

class Cache_redis : public Cache {
 public:
  std::string get_name() { return "Redis"; }
  size_t get_time() { return 0; }
  void update_time(size_t t) {}
  void ini(size_t capacity, size_t THREADS_NUM, std::string config_file = std::string()) {
    std::cout << "Redis" << std::endl;
    redisContext *c;
    struct timeval timeout = {10};
    c = redisConnectWithTimeout("127.0.0.1", 6379, timeout);
    {
      std::string cmd = "config set maxmemory " + std::to_string(capacity);
      redisReply *reply = (redisReply *)redisCommand(c, cmd.c_str());
      printf("Set maxmemory: %s\n", reply->str);
      freeReplyObject(reply);
    }
    {
      redisReply *reply = (redisReply *)redisCommand(c, "config set maxmemory-policy allkeys-lru");
      printf("Set maxmemory-policy: %s\n", reply->str);
      freeReplyObject(reply);
    }
    {
      redisReply *reply = (redisReply *)redisCommand(c, "FLUSHALL");
      printf("FLUSHALL %s\n", reply->str);
      freeReplyObject(reply);
    }
    {
      redisReply *reply = (redisReply *)redisCommand(c, "config get maxmemory");
      // printf("PING4: %s\n", reply->str);
      freeReplyObject(reply);
    }
    redisFree(c);
  }
  void get_instance(int i) {
    struct timeval timeout = {1, 500000};  // 1.5 seconds
    c = redisConnectWithTimeout("127.0.0.1", 6379, timeout);
    if (c == nullptr || c->err) {
      if (c) {
        printf("连接错误: %s\n", c->errstr);
        redisFree(c);
        c = nullptr;
      } else {
        printf("无法分配redis上下文\n");
      }
    }
  }

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    redisReply *reply = (redisReply *)redisCommand(c, "GET %b", key, key_len);
    int r = -2;  // 默认未找到
    if (reply) {
      if (reply->type != REDIS_REPLY_NIL) {
        r = 1;
      }
      freeReplyObject(reply);
    }
    return r;
  }
  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    redisReply *reply =
        (redisReply *)redisCommand(c, "SET %b %b EX %d", key, key_len, value, value_len, ttl);
    freeReplyObject(reply);

    return true;
  }
  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return add(key, key_len, value, value_len, ttl);
  }
  static thread_local redisContext *c;
};
thread_local redisContext *Cache_redis::c = nullptr;

class Cache_Memcached : public Cache {
 public:
  ~Cache_Memcached() {
    if (memc != nullptr) {
      memcached_free(memc);
      memc = nullptr;
    }
  }
  std::string get_name() { return "Memcached"; }
  size_t get_time() { return 0; }
  void update_time(size_t t) {}
  void ini(size_t capacity, size_t THREADS_NUM, std::string config_file = std::string()) {}
  void get_instance(int i) {
    memc = memcached_create(NULL);
    memcached_return_t rc = memcached_server_add(memc, server_host, server_port);
    if (rc != MEMCACHED_SUCCESS) {
      fprintf(stderr, "连接服务器失败: %s\n", memcached_strerror(memc, rc));
      memcached_free(memc);
    }
  }

  // Memcached keys cannot contain control chars / whitespace, so encode
  // binary keys (e.g. YCSB size_t keys) as hex strings.
  static std::string key_to_hex(const char *key, size_t key_len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(key_len * 2);
    for (size_t i = 0; i < key_len; i++) {
      unsigned char c = static_cast<unsigned char>(key[i]);
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xf]);
    }
    return out;
  }

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    std::string hex_key = key_to_hex(key, key_len);
    uint32_t flags = 0;
    memcached_return_t rc;
    int r = -2;
    size_t retrieved_len = 0;
    char *retrieved_value =
        memcached_get(memc, hex_key.data(), hex_key.size(), &retrieved_len, &flags, &rc);
    if (rc == MEMCACHED_SUCCESS) {
      if (retrieved_len > 0) {
        memcpy(value, retrieved_value, retrieved_len);
        value_len = retrieved_len;
        r = 1;
      }
      free(retrieved_value);
    }
    return r;
  }

  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    std::string hex_key = key_to_hex(key, key_len);
    memcached_return_t rc =
        memcached_add(memc, hex_key.data(), hex_key.size(), value, value_len, ttl, 0);
    return true;
  }

  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    std::string hex_key = key_to_hex(key, key_len);
    memcached_return_t rc =
        memcached_set(memc, hex_key.data(), hex_key.size(), value, value_len, ttl, 0);
    if (rc != MEMCACHED_SUCCESS) {
      fprintf(stderr, "set操作失败: %s\n", memcached_strerror(memc, rc));
      return false;
    }
    return true;
  }
  static thread_local memcached_st *memc;
  const char *server_host = "localhost";
  in_port_t server_port = 11211;
};
thread_local memcached_st *Cache_Memcached::memc = nullptr;

class Cache_Paper : public Cache {
 public:
  std::string get_name() { return "PaperCache"; }
  size_t get_time() { return 0; }
  void update_time(size_t t) {}
  void ini(size_t capacity, size_t THREADS_NUM, std::string config_file = std::string()) {
    std::cout << "PaperCache" << std::endl;

    paper::client client("paper://127.0.0.1:3145");
    auto p = client.wipe();
    if (!p->is_ok) {
      std::cout << "error" << std::endl;
      exit(0);
    }
    auto p1 = client.resize(capacity);
    if (!p1->is_ok) {
      std::cout << "error" << std::endl;
      exit(0);
    }
  }
  void get_instance(int i) {
    c = new paper::client("paper://127.0.0.1:3145");
    auto res = c->ping();
    if (!res->is_ok) {
      std::cout << "error" << std::endl;
      exit(0);
    }
  }

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    auto r = c->get(std::string(key, key_len));
    if (!r->is_ok) {
      return -2;
    }
    return 1;
  }
  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    auto r = c->set(std::string(key, key_len), std::string(value, value_len), ttl);
    return true;
  }
  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return add(key, key_len, value, value_len, ttl);
  }
  static thread_local paper::client *c;
};
thread_local paper::client *Cache_Paper::c = nullptr;

class Cache_Segcache : public Cache {
 public:
  std::string get_name() { return "Segcache"; }
  void ini(size_t capacity, size_t THREADS_NUM, std::string config_file) {
    _seg_init(capacity, THREADS_NUM, config_file.c_str());
  }

  //-1:TTL out, -2:Not found, 1:Found
  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    return _seg_get(key, key_len, value, &value_len);
  }
  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    _seg_add(key, key_len, value, value_len, ttl);
    return true;
  }
  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    _seg_set(key, key_len, value, value_len, ttl);
    return true;
  }
  void update_time(size_t time) { _seg_update_time(time); }
  size_t get_time() { return _seg_get_time(); }
  void get_instance(int i) { _seg_get_instance(i); }
};

class Cache_Cachelib : public Cache {
 public:
  using LruCache = facebook::cachelib::LruAllocator;

  Cache_Cachelib() {}

  ~Cache_Cachelib() {
    cache_.reset();
    if (mapped_addr_ != nullptr && mapped_len_ > 0) {
      pmem_unmap(mapped_addr_, mapped_len_);
    }
    std::cout << "Cachelib Release" << std::endl;
  }

  void ini(size_t capacity, size_t THREADS_NUM, std::string config_file = std::string()) {
    std::cout << "Cachelib capacity: " << capacity << std::endl;
    LruCache::Config config;
    config.setCacheName("hemu-bench")
        .setAccessConfig({25, 10})  // 2^25 buckets, 2^10 locks
        .setCacheSize(capacity);
    auto p = "/mnt/cxlpool/cachelib";
    // CacheLib SlabAllocator requires the memory start address to be aligned
    // to sizeof(Slab) == Slab::kSize (4 MB).  pmem_map_file only guarantees
    // page (4 KB) alignment, so we map one extra slab of headroom and round
    // the address up to the next 4 MB boundary.
    constexpr size_t kSlabSize = facebook::cachelib::Slab::kSize;
    size_t mapped_len = 0;
    int is_pmem = 0;
    void *addr =
        pmem_map_file(p, capacity + kSlabSize, PMEM_FILE_CREATE, 0666, &mapped_len, &is_pmem);
    if (addr == nullptr) {
      std::cerr << "pmem_map_file failed for " << p << ": " << strerror(errno) << std::endl;
      exit(EXIT_FAILURE);
    }
    mapped_addr_ = addr;
    mapped_len_ = mapped_len;
    // Round up to slab (4 MB) alignment
    uintptr_t raw = reinterpret_cast<uintptr_t>(addr);
    uintptr_t aligned = (raw + kSlabSize - 1) & ~(kSlabSize - 1);
    void *aligned_addr = reinterpret_cast<void *>(aligned);
    std::cout << "Cachelib pmem_map_file " << p << " at " << addr << " (aligned " << aligned_addr
              << ") size " << mapped_len << " is_pmem=" << is_pmem << std::endl;
    config.setUserMemoryAddr(aligned_addr);

    config.validate();
    cache_ = std::make_unique<LruCache>(config);
    default_pool_ = cache_->addPool("default", cache_->getCacheMemoryStats().ramCacheSize);
  }

  int get(char *key, size_t key_len, char *value, size_t &value_len) {
    auto handle = cache_->find(std::string_view(key, key_len));
    if (!handle) {
      return -2;  // Not found
    }
    size_t item_size = handle->getSize();
    std::memcpy(value, handle->getMemory(), value_len);

    return 1;  // Found
  }

  bool add(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    return set(key, key_len, value, value_len, ttl);
  }

  bool set(char *key, size_t key_len, char *value, size_t value_len, size_t ttl) {
    // set semantics: insert or replace
    auto handle = cache_->allocate(default_pool_, std::string_view(key, key_len), value_len,
                                   static_cast<uint32_t>(ttl));
    if (!handle) {
      return false;
    }
    std::memcpy(handle->getMemory(), value, value_len);
    cache_->insertOrReplace(handle);
    return true;
  }

  void update_time(size_t time) {}
  size_t get_time() { return 0; }
  std::string get_name() { return "Cachelib"; }
  void get_instance(int i = 0) {}
  void print() {
    auto stats = cache_->getPoolStats(default_pool_);
    std::cout << "Cachelib pool stats: numEvictions=" << stats.numEvictions()
              << " numItems=" << stats.numItems() << std::endl;
  }

 private:
  std::unique_ptr<LruCache> cache_;
  facebook::cachelib::PoolId default_pool_;
  void *mapped_addr_ = nullptr;
  size_t mapped_len_ = 0;
};

// ── Command-line argument parsing (--key value style) ──────────

struct BenchArgs {
  std::string cache_name;
  std::string datafile;
  size_t threads = 0;
  size_t capacity = 0;
  std::string config;
  float start = 0.0f;
  float end = 1.0f;
  std::unordered_map<std::string, std::string> hemu_opts;
};

static void print_usage() {
  std::cerr << R"(
Usage: benchmark --cache <name> --datafile <path> --threads <n> --capacity <bytes>
                 [--config <path>] [--start <float>] [--end <float>]
                 [--esm <1|2>] [--cxl-node <n>] [--pm-path <dir>] [--help]

Mandatory:
  --cache       Cache type (Hemu, Redis, PaperCache, Segcache, Viper, Plush, Memcached, Cachelib)
  --datafile    Workload file (keywords: ycsb / cluster / alibaba)
  --threads     Number of threads (>= 1)
  --capacity    Cache capacity in bytes

Optional:
  --config      Config file path (required when --cache Segcache)
  --start       Workload range start ratio (default 0.0)
  --end         Workload range end ratio   (default 1.0)
  --esm         Extended storage mode: 1=File/DAX, 2=NUMA (default 2)
  --cxl-node    CXL NUMA node number, only effective when --esm 2 (default 1)
  --pool-path     Persistent-memory mount path, only effective when --esm 1 (default /mnt/cxlpool/)
  --help        Show this help message

Cache-specific options (positional, format: Group.key=value):
  Hemu.CacheStrategy=<n>         Cache strategy (default 1)
  Hemu.PAGE_SIZE=<bytes>         Page size (default 1048576)
  Hemu.Balancer_ON=<0|1>         Enable balancer (default 1)
  Hemu.Balancer_Interval=<ms>    Balancer interval (default 100)
  Hemu.Access_Granularity=<n>    Access granularity (default 64)
  Hemu.ACR_WINDOW_SIZE=<n>       ACR sliding window size (default 64)
  Hemu.Thread0_DRAM_Page_Limit=<n>  Thread0 DRAM page limit (default 32)
  Hemu.Ini_Hashtable_Capacity=<n>   Initial hashtable bucket num (default 16M)
  Hemu.DRAM_PERCENT=<float>       Initial DRAM percentage (default 0.0)
)" << std::endl;
}

static BenchArgs parse_args(int argc, char **argv) {
  BenchArgs args;
  std::unordered_map<std::string, std::string> kv;
  const std::unordered_set<std::string> valid_long_opts = {
      "--cache", "--datafile", "--threads", "--capacity", "--config",
      "--start", "--end",      "--esm",     "--cxl-node", "--pool-path",
  };

  for (int i = 1; i < argc; ++i) {
    std::string tok = argv[i];
    if (tok == "-h" || tok == "--help") {
      print_usage();
      exit(EXIT_SUCCESS);
    }
    if (tok.rfind("--", 0) == 0) {
      if (!valid_long_opts.count(tok)) {
        std::cerr << "Error: unknown option " << tok << std::endl;
        std::cerr << "Hint: Hemu options use Group.key=value format, e.g. Hemu.Balancer_ON=0"
                  << std::endl;
        print_usage();
        exit(EXIT_FAILURE);
      }
      if (i + 1 >= argc) {
        std::cerr << "Error: missing value for " << tok << std::endl;
        print_usage();
        exit(EXIT_FAILURE);
      }
      kv[tok] = argv[++i];
    } else if (tok.find('.') != std::string::npos && tok.find('=') != std::string::npos) {
      // Parse Hemu.xx=xx style options
      size_t dot_pos = tok.find('.');
      size_t eq_pos = tok.find('=');
      std::string group = tok.substr(0, dot_pos);
      std::string key = tok.substr(dot_pos + 1, eq_pos - dot_pos - 1);
      std::string val = tok.substr(eq_pos + 1);
      if (group == "Hemu") {
        args.hemu_opts[key] = val;
      } else {
        std::cerr << "Error: unknown option group '" << group << "'" << std::endl;
        print_usage();
        exit(EXIT_FAILURE);
      }
    } else {
      std::cerr << "Error: unexpected positional argument '" << tok << "'" << std::endl;
      print_usage();
      exit(EXIT_FAILURE);
    }
  }

  auto get_or_die = [&](const char *key) -> std::string {
    if (kv.count(key)) return kv[key];
    std::cerr << "Error: missing mandatory argument " << key << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  };

  args.cache_name = get_or_die("--cache");
  args.datafile = get_or_die("--datafile");
  args.threads = stoull(get_or_die("--threads"));
  args.capacity = stoull(get_or_die("--capacity"));

  if (kv.count("--config")) args.config = kv["--config"];
  if (kv.count("--start")) args.start = stof(kv["--start"]);
  if (kv.count("--end")) args.end = stof(kv["--end"]);
  if (kv.count("--esm")) eSM = stoi(kv["--esm"]);
  if (kv.count("--cxl-node")) CXL_NODE = stoi(kv["--cxl-node"]);
  if (kv.count("--pool-path")) POOL_PATH = kv["--pool-path"];

  return args;
}

void runbench(Cache *cache, size_t capacity, Workload *workload, Counter *global_counter,
              std::vector<size_t> *latency, int _thread_id) {
  if constexpr (LATENCY_E)
    if (latency != nullptr) latency->reserve(workload->get_item_count() / THREADS_NUM + 1);

  cache->get_instance(_thread_id);
  uint64_t last_print = 0, timestamp = 0;
  Counter local_counter;
  time_t start_time = time(nullptr);
  Timer latency_timer;
  while (true) {
    if constexpr (MISS_MONITOR)
      if (timestamp % 60 == 0 && timestamp != last_print && _thread_id == 0) {
        last_print = timestamp;
        printf("%lu %f\n", timestamp,
               (double)(local_counter.miss_count) / (double)(local_counter.get_count + 1));
      }

    auto curr_opt = workload->get_next_run(_thread_id);
    if (!curr_opt.has_value()) break;
    const WorkloadItem &curr_item = *curr_opt;

    uint32_t ctime = item_timestamp(curr_item);
    // uint32_t ctime = time(nullptr) - start_time;

    if (ctime > timestamp) {
      timestamp = ctime;
      cache->update_time(timestamp);
    }

    if constexpr (LATENCY_E) latency_timer.start();

    op_type op = item_op(curr_item);
    char *key_addr = item_key_addr(curr_item);
    size_t key_len = item_key_len(curr_item);
    size_t value_len = item_value_len(curr_item);
    size_t ttl = item_ttl(curr_item);

    if (op == SET) {
      auto r = cache->set(key_addr, key_len, value[_thread_id], value_len, ttl);
      local_counter.set_count++;
    } else if (op == ADD) {
      auto r = cache->add(key_addr, key_len, value[_thread_id], value_len, ttl);
      local_counter.add_count++;
    } else if (op == GET) {
      local_counter.get_count++;

      auto r = cache->get(key_addr, key_len, value[_thread_id], value_len);
      if (r <= 0) {
        local_counter.miss_count++;
        if (value_len > 0) {
          cache->set(key_addr, key_len, value[_thread_id], value_len, ttl);
        }
        if (r == -1) local_counter.TTL_out_count++;
      } else if (r == 1) {
        local_counter.hit_count++;
        // for (size_t kk = 0; kk < value_len; kk++) {
        //   assert(value[_thread_id][kk] == ('A' + kk % 26));
        // }
      }
    }
    local_counter.ops_count++;
    if constexpr (LATENCY_E)
      if (latency != nullptr) latency->push_back(latency_timer.elapsed<std::chrono::nanoseconds>());
  }
  if (global_counter != nullptr) global_counter->add(local_counter);
}

void test(int argc, char **argv) {
  BenchArgs args = parse_args(argc, argv);

  std::cout << "cache=" << args.cache_name << " datafile=" << args.datafile
            << " threads=" << args.threads << " capacity=" << args.capacity;
  if (!args.config.empty()) std::cout << " config=" << args.config;
  std::cout << " start=" << args.start << " end=" << args.end;
  std::cout << " esm=" << eSM << " cxl_node=" << CXL_NODE << " pm_path=" << POOL_PATH;
  for (auto &[k, v] : args.hemu_opts) std::cout << " Hemu." << k << "=" << v;
  std::cout << std::endl;

  THREADS_NUM = args.threads;

  std::cout << "Loading Workload." << std::endl;
  Workload *workload = nullptr;

  if (args.datafile.find("cluster") != std::string::npos) {
    std::string datafile = "/data/" + args.datafile;
    std::cout << "Twitter on: " << datafile << std::endl;
    workload = new Twitter(datafile, THREADS_NUM, args.start, args.end);
  } else if (args.datafile.find("ycsb") != std::string::npos) {
    std::string run = "/data/YCSB_4096_1_256/ycsb_run_workload";
    char w = args.datafile.back();
    run.push_back(w);
    workload = new YCSB(run, THREADS_NUM, args.start, args.end);

    std::cout << "YCSB on: " << run << std::endl;
  } else {
    std::cerr << "No workload file found!" << std::endl;
    exit(EXIT_FAILURE);
  }

  Cache *cache = nullptr;

  if (args.cache_name == "Hemu") {
    cache = new Cache_hemu(args.capacity, args.hemu_opts);
  } else if (args.cache_name == "Redis") {
    cache = new Cache_redis();
  } else if (args.cache_name == "PaperCache") {
    cache = new Cache_Paper();
  } else if (args.cache_name == "Segcache") {
    if (args.config.empty()) {
      std::cerr << "Error: --config is required for Segcache" << std::endl;
      exit(EXIT_FAILURE);
    }
    cache = new Cache_Segcache();
  } else if (args.cache_name == "Viper") {
    cache = new KV_viper();
  } else if (args.cache_name == "Plush") {
    cache = new KV_Plush();
  } else if (args.cache_name == "Memcached") {
    cache = new Cache_Memcached();
  } else if (args.cache_name == "Cachelib") {
    cache = new Cache_Cachelib();
  } else {
    std::cerr << "Error: unknown cache type '" << args.cache_name << "'" << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  }
  cache->ini(args.capacity, THREADS_NUM, args.config);
  std::cout << "Workload loaded." << std::endl;
  std::thread ths[THREADS_NUM];

  std::cout << "Run." << std::endl;
  {
    Counter global_counter;
    std::vector<size_t> latency;  // only single thread.

    Timer timter;
    timter.start();
    for (size_t i = 0; i < THREADS_NUM; i++) {
      ths[i] = std::thread(runbench, cache, args.capacity, workload, &global_counter, &latency, i);
    }
    for (size_t i = 0; i < THREADS_NUM; i++) {
      ths[i].join();
    }
    auto time_cost = timter.elapsed<std::chrono::seconds>();

    float throughput = (global_counter.ops_count.load() / 1000000.0) / time_cost;
    float missratio = (100.0 * global_counter.miss_count.load()) / global_counter.get_count.load();

    std::cout << std::fixed << "Performance:\n"
              << "\tThroughput: " << std::setprecision(5)
              << (global_counter.ops_count.load() / 1000000.0) / time_cost << " Mops/s\n"
              << "\tMiss num: " << global_counter.miss_count.load() << "\n"
              << "\tMiss Ratio: " << std::setprecision(5)
              << (100.0 * global_counter.miss_count.load()) / global_counter.get_count.load()
              << " %\n"
              << std::setprecision(1)
              << "\tTotal Request: " << global_counter.ops_count.load() / 1000000.0 << " Mops"
              << "\n"
              << "\tSet Request: " << global_counter.set_count.load() / 1000000.0 << " Mops\n"
              << "\tAdd Request: " << global_counter.add_count.load() / 1000000.0 << " Mops\n"
              << "\tGet Request: " << global_counter.get_count.load() / 1000000.0 << " Mops\n"
              << "\tHit Count: " << global_counter.hit_count.load() / 1000000.0 << "Mops\n"
              << "\tRuntime: " << time_cost << " seconds\n"
              << "\tTTL_out_count: " << global_counter.TTL_out_count.load() << "\n"
              << "\tcurrent_time: " << cache->get_time() << std::endl;

    if constexpr (LATENCY_E) {
      size_t sum = 0;
      for (auto &&i : latency) {
        sum += i;
      }
      std::cout << latency.size() << std::endl;
      std::sort(latency.begin(), latency.end());

      printf("%lu\n", latency[(size_t)(1.0 * latency.size() * 0.90)]);
      printf("%lu\n", latency[(size_t)(1.0 * latency.size() * 0.99)]);
      printf("%lu\n", latency[(size_t)(1.0 * latency.size() * 0.999)]);
      printf("%lu\n", latency[(size_t)(1.0 * latency.size() * 0.9999)]);
      printf("%lu\n", latency[(size_t)(1.0 * latency.size() * 0.99999)]);
      printf("%lu\n", latency[(size_t)(1.0 * latency.size() * 0.999999)]);
    }

    std::ofstream out("./result", std::fstream::out | std::fstream::app);
    if constexpr (LATENCY_E) {
      out << cache->get_name() << " " << workload->get_workload_name() << " " << THREADS_NUM << " "
          << args.capacity << " " << throughput << " " << missratio << " "
          << latency[(size_t)(1.0 * latency.size() * 0.90)] << " "
          << latency[(size_t)(1.0 * latency.size() * 0.99)] << " "
          << latency[(size_t)(1.0 * latency.size() * 0.999)] << " "
          << latency[(size_t)(1.0 * latency.size() * 0.9999)] << " "
          << latency[(size_t)(1.0 * latency.size() * 0.99999)] << " "
          << latency[(size_t)(1.0 * latency.size() * 0.999999)] << std::endl;
    } else {
      out << cache->get_name() << " " << workload->get_workload_name() << " " << THREADS_NUM << " "
          << args.capacity << " " << throughput << " " << missratio << " "
          << Hemu_Cache::config.DRAM_PERCENT << " " << Hemu_Cache::config.highest_dram_percent
          << " "
          << Hemu_Cache::config.avg_dram_percent_sum / Hemu_Cache::config.avg_dram_percent_count;
      out << std::endl;
    }
    out.flush();
    out.close();

    cache->print();
    delete workload;
  }

  delete cache;
}

int main(int argc, char **argv) {
  test(argc, argv);
  print_DRAM_usage();
  return 0;
}
