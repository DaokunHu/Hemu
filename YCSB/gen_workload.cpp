#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../benchmark/benchmark.h"

size_t parse_key(const std::string& key_str) { return static_cast<size_t>(std::stoull(key_str)); }

uint32_t parse_length(size_t length) { return static_cast<uint32_t>(length); }

void check_args(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "Usage:\nworkload file\n";
    exit(EXIT_FAILURE);
  }
}

std::vector<std::string> read_config_file(const std::string& filename) {
  std::vector<std::string> args;
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Failed to open config file: " << filename << "\n";
    exit(EXIT_FAILURE);
  }
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty()) {
      args.push_back(line);
    }
  }
  return args;
}

void run_command(const std::string& command) {
  int result = std::system(command.c_str());
  if (result != 0) {
    std::cerr << "Command failed: " << command << "\n";
    exit(EXIT_FAILURE);
  }
}
std::unordered_map<size_t, size_t> key_map;

void process_load_file(const std::string& input_file, const std::string& output_file) {
  std::ifstream fin(input_file);
  std::ofstream fout(output_file, std::ios::binary);
  if (!fin || !fout) {
    std::cerr << "Failed to open file for processing.\n";
    exit(EXIT_FAILURE);
  }

  std::string line;
  while (std::getline(fin, line)) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }
    if (!tokens.empty() && tokens[0] == "INSERT") {
      size_t pos = line.find("[ field0=");
      if (pos != std::string::npos) {
        size_t key = parse_key(tokens[2].substr(4));
        std::string value = line.substr(pos + 1);
        uint32_t len = parse_length(value.length() - 2);
        key_map[key] = len;
      }
    }
  }
  fout.flush();
  fout.close();
  fin.close();
}

void process_run_file(const std::string& input_file, const std::string& output_file) {
  std::ifstream fin(input_file);
  std::ofstream fout(output_file, std::ios::binary);

  if (!fin || !fout) {
    std::cerr << "Failed to open file for processing.\n";
    exit(EXIT_FAILURE);
  }

  std::string line;

  while (std::getline(fin, line)) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }
    if (!tokens.empty()) {
      ycsb_workload item;

      if (tokens[0] == "READ") {
        size_t key = parse_key(tokens[2].substr(4));
        item.op = op_type::GET;
        item.key = key;
        if (!key_map.count(key)) std::runtime_error("no key existed.\n");
        item.value_len = key_map[key];
        fout.write(reinterpret_cast<const char*>(&item), sizeof(item));
      } else if (tokens[0] == "INSERT" || tokens[0] == "UPDATE") {
        size_t key = parse_key(tokens[2].substr(4));
        size_t pos = line.find("[ field0=");
        if (pos != std::string::npos) {
          std::string value = line.substr(pos + 1);
          uint32_t len = parse_length(value.length() - 2);
          key_map[key] = len;
          item.op = op_type::SET;
          item.value_len = len;
          item.key = key;
          fout.write(reinterpret_cast<const char*>(&item), sizeof(item));
        }
      }
    }
  }
}

int main(int argc, char* argv[]) {
  check_args(argc, argv);

  std::vector<std::string> args = read_config_file(argv[1]);
  if (args.size() < 2) {
    std::cerr << "Config file must contain at least two lines.\n";
    exit(EXIT_FAILURE);
  }

  std::string workload = args[0];
  std::string key_type = args[1];

  std::string ycsb_dir = "YCSB/";
  std::string workload_dir = "workload_spec/";
  std::string output_dir = "workloads/";

  std::string ycsb_load = output_dir + "Rycsb_load_" + key_type + "_" + workload;
  std::string ycsb_run = output_dir + "Rycsb_run_" + key_type + "_" + workload;
  std::string load = output_dir + "ycsb_load_" + workload;
  std::string run = output_dir + "ycsb_run_" + workload;

  std::ostringstream cmd_load, cmd_run;
  cmd_load << ycsb_dir << "bin/ycsb.sh load basic -P " << workload_dir << workload << " -s > "
           << ycsb_load;
  cmd_run << ycsb_dir << "bin/ycsb.sh run basic -P " << workload_dir << workload << " -s > "
          << ycsb_run;

  std::cout << cmd_load.str() << std::endl;
  std::cout << cmd_run.str() << std::endl;

  run_command(cmd_load.str());
  run_command(cmd_run.str());

  process_load_file(ycsb_load, load);
  process_run_file(ycsb_run, run);

  return 0;
}
