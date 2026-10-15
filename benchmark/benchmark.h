#include <cstddef>
#include <cstdint>
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
  uint32_t op : 2;
  uint32_t value_len : 30;
  size_t key;
};
#pragma pack(pop)