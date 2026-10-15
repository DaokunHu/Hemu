#include "Segcache.h"

#include "bench_storage.h"

#include <cc_debug.h>
#include <cc_define.h>
#include <cc_log.h>
#include <cc_mm.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time/cc_timer.h>

#define N_MAX_THREAD 128
#define THREAD_PIN

volatile static bool start = false;
volatile static bool stop = false;
static char val_array[MAX_VAL_LEN];
static int n_thread;
static struct reader *readers[N_MAX_THREAD];
static int time_speedup;
volatile int64_t op_cnt[op_invalid];
volatile static uint64_t n_req = 0;
volatile static uint64_t n_get_req = 0;
volatile static uint64_t n_miss = 0;

static delta_time_i default_ttls[100];

#define BENCHMARK_OPTION(ACTION)                                                                \
  ACTION(trace_path, OPTION_TYPE_STR, NULL, "path to the trace")                                \
  ACTION(default_ttl_list, OPTION_TYPE_STR, "86400:1", "a comma separated list of ttl:percent") \
  ACTION(n_thread, OPTION_TYPE_UINT, 1, "the number of threads")                                \
  ACTION(debug_logging, OPTION_TYPE_BOOL, true, "turn on debug logging")                        \
  ACTION(time_speedup, OPTION_TYPE_UINT, 1, "speed up the time in replay")                      \
  ACTION(nottl, OPTION_TYPE_UINT, 0, "whether use trace TTL")

struct replay_specific {
  BENCHMARK_OPTION(OPTION_DECLARE)
};

struct benchmark_options {
  struct replay_specific benchmark;
  debug_options_st debug;
  struct option engine[]; /* storage-engine specific options... */
};
typedef struct benchmark_options bench_options_st;

struct benchmark b;

rstatus_i benchmark_create(struct benchmark *b, const char *config) {
  unsigned n_opts_all, n_opts_bench, n_opts_dbg, n_opts_storage;
  struct replay_specific bench_opts = {BENCHMARK_OPTION(OPTION_INIT)};
  debug_options_st debug_opts = {DEBUG_OPTION(OPTION_INIT)};

  n_opts_bench = OPTION_CARDINALITY(struct replay_specific);
  n_opts_dbg = OPTION_CARDINALITY(debug_options_st);
  n_opts_storage = bench_storage_config_nopts();
  n_opts_all = n_opts_bench + n_opts_dbg + n_opts_storage;

  b->options = cc_alloc(sizeof(struct option) * n_opts_all);
  ASSERT(b->options != NULL);

  option_load_default((struct option *)&bench_opts, n_opts_bench);
  option_load_default((struct option *)&debug_opts, n_opts_dbg);

  BENCH_OPTS(b)->benchmark = bench_opts;
  BENCH_OPTS(b)->debug = debug_opts;
  bench_storage_config_init(BENCH_OPTS(b)->engine);

  if (config != NULL) {
    FILE *fp = fopen(config, "r");
    if (fp == NULL) {
      printf("cannot open config %s\n", config);
      exit(EX_CONFIG);
    }
    option_load_file(fp, (struct option *)b->options, n_opts_all);
    fclose(fp);
  }

  if (O_BOOL(b, debug_logging)) {
    if (debug_setup(&(BENCH_OPTS(b)->debug)) != CC_OK) {
      printf("debug log setup failed");
      exit(EX_CONFIG);
    }
  }

  n_thread = O_UINT(b, n_thread);
  time_speedup = O_UINT(b, time_speedup);

  return CC_OK;
}
void _seg_init(size_t capacity, size_t thread_num, const char *config) {
  char cmd[1024];
  sprintf(cmd, "sed -i \"s/seg_n_thread:[0-9]*/seg_n_thread:%ld/g\" %s", thread_num, config);
  if (system(cmd) != 0) fprintf(stderr, "Error!");
  sprintf(cmd, "sed -i \"s/n_thread:[0-9]*/n_thread:%ld/g\" %s", thread_num, config);
  if (system(cmd) != 0) fprintf(stderr, "Error!");
  sprintf(cmd, "sed -i \"s/heap_mem:[0-9]*/heap_mem:%lu/g\" %s", capacity, config);
  if (system(cmd) != 0) fprintf(stderr, "Error!");

  if (benchmark_create(&b, config) != 0) {
    printf("failed to create benchmark instance");
    exit(0);
  }
  bench_storage_init(BENCH_OPTS(&b)->engine, 0, 0);
  printf("Init done.\n");
}
bool _seg_set(char *key, size_t k_len, char *value, size_t v_len, uint32_t etime) {
  struct benchmark_entry e;
  e.key = key;
  e.key_len = k_len;
  e.val = value;
  e.val_len = v_len;
  e.ttl = etime;
  e.expire_at = proc_sec + etime;
  e.op = op_set;
  run_op(&e);
  return true;
}
bool _seg_add(char *key, size_t k_len, char *value, size_t v_len, uint32_t etime) {
  struct benchmark_entry e;
  e.key = key;
  e.key_len = k_len;
  e.val = value;
  e.val_len = v_len;
  e.ttl = etime;
  e.expire_at = proc_sec + etime;
  e.op = op_add;
  run_op(&e);
  return true;
}
int _seg_get(char *key, size_t k_len, char *value, size_t *value_len) {
  int r = 0;
  struct benchmark_entry e;
  e.key = key;
  e.key_len = k_len;
  e.val = value;
  e.op = op_get;
  rstatus_i status = run_op(&e);
  if (status == CC_EEMPTY) {
    r = -2;
  } else if (status == CC_OK)
    r = 1;
  return r;
}

void _seg_update_time(size_t time) {
  int32_t curr = time;
  size_t t = __atomic_load_n(&proc_sec, __ATOMIC_RELAXED);
  if (t < curr) {
    __atomic_store_n(&proc_sec, curr, __ATOMIC_RELAXED);
  }
}

size_t _seg_get_time() { return proc_sec; }

void _seg_get_instance(int i) {}