#include <algorithm>
#include <array>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../benchmark/benchmark.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace std;
// enum OP_t { GET, GETS, SET, ADD, REPLACE, CAS, APPEND, PREPEND, DELETE, INCR, DECR };

std::map<string, int> ops = {
    {"get", op_type::GET},       {"gets", op_type::GETS},       {"set", op_type::SET},
    {"add", op_type::ADD},       {"replace", op_type::REPLACE}, {"cas", op_type::CAS},
    {"append", op_type::APPEND}, {"prepend", op_type::PREPEND}, {"delete", op_type::DELETE},
    {"incr", op_type::INCR},     {"dec", op_type::DECR}};
std::vector<std::string> split(const std::string &str, const std::string &delims = " ") {
  std::vector<std::string> output;
  auto first = std::cbegin(str);
  while (first != std::cend(str)) {
    const auto second =
        std::find_first_of(first, std::cend(str), std::cbegin(delims), std::cend(delims));
    if (first != second) output.emplace_back(first, second);
    if (second == std::cend(str)) break;
    first = std::next(second);
  }
  return output;
}

class ttl_generator_t {
 public:
  void init(std::vector<float> ttls) {
    std::default_random_engine gen;
    std::discrete_distribution<uint32_t> ttls_weights(ttls.begin(), ttls.end());
    for (unsigned int i = 0; i < ttls_.size(); ++i) {
      ttls_[i] = ttls_weights(gen);
    }
  }

  int next() { return ttls_[dist_(gen_) % 1024]; }

 private:
  uint32_t seed_;
  std::default_random_engine gen_;
  std::uniform_int_distribution<uint32_t> dist_;
  std::array<int, 1024> ttls_;
};
ttl_generator_t ttl_generator;
std::vector<size_t> TTLS;
std::vector<float> rates;

int main(int argc, char **argv) {
  string file = argv[1];

  for (size_t i = 2; i < argc; i += 2) {
    auto s = std::string(argv[i]);
    size_t toseconds = 1;
    switch (s.back()) {
      case 's':
        toseconds *= 1;
        break;
      case 'h':
        toseconds *= 60 * 60;
        break;
      case 'd':
        toseconds *= 60 * 60 * 24;
        break;
      default:
        std::cerr << "TTL ERROR";
        exit(0);
    }
    s.pop_back();
    auto ttl = stof(s) * toseconds;
    TTLS.push_back(ttl);
    rates.push_back(stof(std::string(argv[i + 1])));
  }
  ttl_generator.init(rates);

  ifstream infile(file);
  ofstream outfile(file + ".bin", ios::out | ios::binary);
  string line;
  size_t n = 0;
  while (getline(infile, line)) {
    n++;
    auto r = split(line, ",");
    if (r.size() == 7) {
      size_t timestamp = stoull(r[0]);
      // size_t ttl = stoull(r[6]);
      auto ne = ttl_generator.next();
      size_t ttl = TTLS[ne];

      uint32_t keysize = stoul(r[2]);
      uint32_t vsize = stoul(r[3]);
      char op = ops[r[5]];
      if (op > 11) std::cout << line << std::endl;
      if (keysize > 500) std::cout << line << std::endl;
      string key = r[1];
      twitter_workload item;
      item.op = op;
      item.timestamp = timestamp;
      item.ttl = ttl;
      item.key_len = keysize;
      item.value_len = vsize;
      outfile.write(reinterpret_cast<char *>(&item), sizeof(twitter_workload));
      outfile.write(key.c_str(), keysize);
    }
  }
  std::cout << n << std::endl;
  return 0;
}