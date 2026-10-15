CXX_BASE = g++
BUILD_TYPE ?= Release
# BUILD_TYPE ?= Debug

# 通用编译选项
CXXFLAGS_BASE = -std=c++2a -fPIE -pie -march=native
# 根据构建类型设置编译选项
ifeq ($(BUILD_TYPE), Debug)
	CXXFLAGS_BASE += -ggdb -O0
else
	CXXFLAGS_BASE += -O3
endif

HOME = $(CURDIR)/../

HEMU_DIR = $(HOME)/Hemu
HEMU_LIB = $(HEMU_DIR)/Hemp.h
HEMU_LDFLAGS = -I$(HEMU_DIR) -L$(HEMU_DIR)

PAPERCACHE_DIR = $(HOME)/third/PaperCache
PAPER_CLIENT_CPP_LIB = $(PAPERCACHE_DIR)/libpaper_client_cpp.a $(PAPERCACHE_DIR)/libpaper_client_c.a
PAPER_CLIENT_LDFLAGS = -I$(PAPERCACHE_DIR)/include/paper_client_cpp -I$(PAPERCACHE_DIR)/include \
					   -I$(PAPERCACHE_DIR)/extern/paper-client-c/include/paper_client_c \
					   -I$(PAPERCACHE_DIR)/extern/paper-client-c/include \
					   -L$(PAPERCACHE_DIR)/build  -L$(PAPERCACHE_DIR)/build/extern/paper-client-c

SEGCACHE_DIR = $(HOME)/third/Segcache
SEGCACHE_LIB = $(SEGCACHE_DIR)/build/storage/libseg.a ./Segcache_bench/libsegcache.a
SEGCACHE_LDFLAGS= -I$(SEGCACHE_DIR)/build -I$(SEGCACHE_DIR)/deps/ccommon/include -I$(SEGCACHE_DIR)/src -I$(SEGCACHE_DIR)/benchmarks \
				  -I./Segcache_bench -L./Segcache_bench
SEGCACHE_BUILD_DIR = $(SEGCACHE_DIR)/build/benchmarks


VIPER_DIR = $(HOME)/third/viper/include/viper 
VIPER_INCLUDE = -I$(VIPER_DIR) -I./viper_bench


PLUSH_DIR = $(HOME)/third/Plush/src/hashtable
PLUSH_INCLUDE = -L$(PLUSH_DIR) -I$(PLUSH_DIR) 
PLUSH_LIB = $(PLUSH_DIR)/libPlush.a

# CacheLib — headers from source tree (tracks local modifications),
#            shared libs from getdeps aggregated directory
CACHELIB_SRC ?= $(HOME)/third/CacheLib
CACHELIB_SRC_ABS = $(abspath $(CACHELIB_SRC))
CACHELIB_DIR ?= $(HOME)/third/cachelib-unified
CACHELIB_SCRATCH ?= /tmp/cachelib-getdeps
CACHELIB_GETDEPS = $(CACHELIB_SRC)/build/fbcode_builder/getdeps.py
CACHELIB_INCLUDE = -I$(CACHELIB_SRC) -I$(CACHELIB_DIR)/include
CACHELIB_LDFLAGS = -L$(CACHELIB_DIR)/lib -Wl,--disable-new-dtags -Wl,-rpath,$(CACHELIB_DIR)/lib
CACHELIB_LIBS = -lcachelib_allocator -lcachelib_navy -lcachelib_shm \
                -lcachelib_common -lcachelib_datatype -lcachelib_interface \
                -lthriftcpp2 -lthriftprotocol -lthrift-core -lthriftfrozen2 \
                -lthriftmetadata -ltransport -lconcurrency -lasync -lruntime \
                -lwangle -lfizz -lfolly -lfmt -lglog \
                -lboost_context -lboost_filesystem -lboost_program_options \
                -lboost_regex -lboost_system -lboost_thread \
                -lssl -lcrypto -lsodium -ldouble-conversion \
                -llz4 -lsnappy -lz -lzstd -lxxhash



# Benchmark
BENCHMARK = benchmark
BENCHMARK_SRC = benchmark.cpp
BENCHMARK_CXXFLAGS = $(CXXFLAGS_BASE)
BENCHMARK_LDFLAGS = $(HEMU_LDFLAGS) $(SEGCACHE_LDFLAGS) \
					$(PAPER_CLIENT_LDFLAGS) $(VIPER_INCLUDE) \
					$(LDFLAGS_BASE) $(PLUSH_INCLUDE) \
					$(CACHELIB_INCLUDE) $(CACHELIB_LDFLAGS) \
					-lhiredis -lpaper_client_cpp -lpaper_client_c -lsegcache -pthread -mclwb \
					-lPlush -lmemcached -lnuma -lpmem \
					$(CACHELIB_LIBS)



all: $(BENCHMARK)

# 构建Hemu项目
$(PAPER_CLIENT_CPP_LIB):
	cd $(PAPERCACHE_DIR); mkdir -p build; cd build; \
	cmake .. -DCMAKE_C_FLAGS="-fPIE" -DCMAKE_CXX_FLAGS="-fPIE"; \
	make -j;

$(PLUSH_LIB): $(PLUSH_DIR)/Hashtable.cpp $(PLUSH_DIR)/Hashtable.h
	cd $(PLUSH_DIR);$(CXX_BASE) $(CXXFLAGS_BASE) Hashtable.cpp -c -o plush.o -mavx2 -march=native -lpmem -mclflushopt -pthread
	ar rv $@ $(PLUSH_DIR)/plush.o 

# 构建CacheLib (getdeps + --src-dir 使用本地源码)
$(CACHELIB_DIR)/lib/libcachelib_allocator.so:
	cd $(CACHELIB_SRC) && python3 $(CACHELIB_GETDEPS) \
		--scratch-path $(CACHELIB_SCRATCH) \
		--allow-system-packages build cachelib \
		--extra-cmake-defines '{"BUILD_SHARED_LIBS": "ON"}' \
		--src-dir cachelib:$(CACHELIB_SRC_ABS) \
		--aggregate-deps-dir $(CACHELIB_DIR)

$(SEGCACHE_LIB):
	cd $(SEGCACHE_DIR); mkdir -p build; cd build; cmake -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DUSE_PMEM=ON .. -DCMAKE_CXX_FLAGS="-fPIE" -DCMAKE_C_FLAGS="-fPIE" -DCMAKE_EXE_LINKER_FLAGS="-pie"; make trace_replay_seg -j;
	cc $(CFLAGS_BASE) -fPIE -c -o Segcache_bench/Segcache.o Segcache_bench/Segcache.c -D_GNU_SOURCE $(SEGCACHE_LDFLAGS) -lrt -pthread

	cd Segcache_bench;\
	ar -x $(SEGCACHE_BUILD_DIR)/storage_seg/libbench_storage_seg.a; \
	ar -x $(SEGCACHE_BUILD_DIR)/../storage/seg/libseg.a; \
	ar -x $(SEGCACHE_BUILD_DIR)/../time/libtime.a; \
	ar -x $(SEGCACHE_BUILD_DIR)/../ccommon/lib/libccommon-2.1.0.a;\
	ar -x $(SEGCACHE_BUILD_DIR)/../datapool/libdatapool.a;
						   
	ar rv Segcache_bench/libsegcache.a Segcache_bench/*.o 


# 构建Benchmark
$(BENCHMARK): $(BENCHMARK_SRC) $(CACHELIB_DIR)/lib/libcachelib_allocator.so $(PAPER_CLIENT_CPP_LIB) $(SEGCACHE_LIB) $(PLUSH_LIB)
	$(CXX_BASE) $(BENCHMARK_CXXFLAGS) $< -o $@ $(BENCHMARK_LDFLAGS) 
	mkdir -p  /mnt/cxlpool/Hemu /mnt/cxlpool/Segcache /mnt/cxlpool/Plush

# 清理所有生成文件
clean:
	rm -rf ./Segcache_bench/*.a ./Segcache_bench/*.o
	rm -f $(PLUSH_DIR)/*.o $(PLUSH_DIR)/*.a
	rm -rf $(PAPERCACHE_DIR)/build 
	rm -rf $(SEGCACHE_DIR)/build
	rm -rf $(CACHELIB_DIR)
	rm -f $(BENCHMARK)



.PHONY: all clean