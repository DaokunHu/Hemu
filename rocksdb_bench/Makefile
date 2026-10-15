CXX = g++-14
CXXFLAGS = -std=c++2a -O3 -march=native -fPIE -pie
LDFLAGS = -pthread -lnuma -lpmem -lrocksdb

HOME = $(CURDIR)/..
HEMU_DIR = $(HOME)/Hemu
HEMU_INCLUDE = -I$(HEMU_DIR)

BENCH = rocksdb_bench
SRC = benchmark.cpp

all: $(BENCH)

$(BENCH): $(SRC)
	$(CXX) $(CXXFLAGS) $(HEMU_INCLUDE) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(BENCH)
	rm -rf ./rocksdb_data
	rm -rf /mnt/cxlpool/hemu_rocksdb_bench

.PHONY: all clean
