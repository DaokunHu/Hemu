#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

#include "../benchmark/benchmark.h"
// #pragma pack(push, 1)
// struct ycsb_workload {
//   uint32_t op : 2;
//   uint32_t value_len : 30;
//   size_t key;
// };
// #pragma pack(pop)

#include <set>

#include <fcntl.h>
#include <fnmatch.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
  std::string workload = "/data/YCSB_4096_1_256/ycsb_run_workload" + std::string(argv[1]);
  int fd = open(workload.c_str(), O_RDONLY);
  size_t len = lseek(fd, 0, SEEK_END);
  std::cout << "Data file size: " << len << std::endl;

  if (fd < 0) {
    perror("fd is invalid");
    exit(EXIT_FAILURE);
  }
  if (len == 0) {
    fprintf(stderr, "len must be greater than 0\n");
    exit(EXIT_FAILURE);
  }

  char *addr = (char *)mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);

  if (addr == MAP_FAILED) {
    perror("mmap failed");
    close(fd);
    exit(EXIT_FAILURE);
  }

  size_t offset = 0;
  size_t count = 0;
  size_t total_size = 0;
  size_t wss = 0;
  size_t miss = 0;
  size_t write = 0;
  size_t read = 0;
  std::set<size_t> unique_keys;
  while (offset < len) {
    count++;
    auto curr = addr + offset;
    auto st = reinterpret_cast<ycsb_workload *>(curr);
    if (st->op != 0) {
      write++;
    }
    total_size += 8 + st->value_len;
    if (unique_keys.count(st->key) == 0) {
      if (st->op == 0) {
        miss++;
        write++;
      }
      wss += 8 + st->value_len;
      unique_keys.insert(st->key);
    } else {
      read++;
    }
    offset += sizeof(ycsb_workload);
  }
  std::cout << "count: " << count << " total size: " << total_size / 1024 / 1024 / 1024
            << " wss: " << wss * 1.0 / 1024 / 1024 / 1024 << " miss: " << miss * 1.0 / read
            << " read: " << read << " write: " << write << std::endl;
  return 0;
}
