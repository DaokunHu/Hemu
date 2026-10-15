/**
 * Hemu Playground
 */
#include <string>

#include "Hemu.h"

int main() {
  Hemu_Cache::config.esm_allocator_type =
      Hemu_Cache::AllocatorType::NUMA;  // set to NUMA if not using DAX mode.
  Hemu_Cache::config.eSM_NODE = 1;     // set only if allocator_type is NUMA

  Hemu_Cache::config.CacheCapacity = 512 * 1024 * 1024ULL;
  Hemu_Cache::config.CacheStrategy = 1;
  Hemu_Cache::config.DRAM_PERCENT = 0;
  Hemu_Cache::config.FILE_POOL_DIR = "/path/to/dax(ignore if NUMA mode)";

  Hemu_Cache::Hemu hemu;

  // set
  char key[] = "hello";
  char val[] = "world";
  hemu.set(key, strlen(key), val, strlen(val), 3600);

  // get
  char buf[4096];
  size_t vlen = 0;
  hemu.get(key, strlen(key), buf, vlen);
  std::string str(buf, vlen);
  std::cout <<"key: "<< key << " value: " << str << std::endl;

  // add (key already exists, won't overwrite)
  char val2[] = "hemu";
  hemu.add(key, strlen(key), val2, strlen(val2), 3600);

  hemu.get(key, strlen(key), buf, vlen);
  std::string str2(buf, vlen);
  std::cout <<"key: "<< key << " value: " << str << std::endl;


  return 0;
}