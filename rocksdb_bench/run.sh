#!/bin/bash
# ════════════════════════════════════════════════════════════════════
# Hemu + RocksDB Throughput Benchmark Runner
#
# Compares RocksDB-only vs Hemu+RocksDB on real trace data.
#
# By default the script AUTOMATICALLY runs ALL THREE scenarios for every
# datafile as separate, isolated invocations (each with its own
# clear_data + drop_caches):
#     0. RocksDB-only      (--hemu=0)
#     1. Hemu + RocksDB    (--hemu=1)
#     2. Hemu-only         (--hemu=2)
#
# Set HEMU=0, HEMU=1, or HEMU=2 to run only one scenario.
#
# Usage: ./run.sh <experiment|datafile> [extra args...]
#
# Experiments:
#   single        Run one datafile (requires --datafile=<name>)
#   throughput    Run all Twitter cluster traces + YCSB workloads
#   thread-sweep  Sweep thread count (1, 8, 16, 32, 64) on one datafile
#   cap-sweep     Sweep Hemu cache capacity (256MB→4GB) on one datafile
#   all           Alias for 'throughput'
#   help          Show this help message
#
# Datafiles:
#   Twitter cluster traces:  cluster3, cluster10, cluster22, cluster24,
#                           cluster30, cluster35, cluster45, cluster53
#   YCSB workloads:         ycsba, ycsbb, ycsbc, ycsbd, ycsbe, ycsbf
#
# Environment variables (override defaults):
#   THREADS     Number of threads             (default: 1)
#   HEMU_CAP    Hemu cache capacity in bytes   (default: 2147483648)
#   ESM         eSM mode: 1=File/DAX, 2=NUMA   (default: 2)
#   CXL_NODE    NUMA node for CXL memory       (default: 1)
#   START       Trace start fraction 0..1      (default: 0.0)
#   END         Trace end fraction 0..1        (default: 1.0)
#   HEMU        0=RocksDB only, 1=Hemu+RocksDB, 2=Hemu only (default: unset → run all 3)
#   NUMA        0=disable numactl, 1=enable     (default: 1)
#
# Examples:
#   ./run.sh single --datafile=ycsba
#   ./run.sh single --datafile=cluster10 --threads=4
#   ./run.sh throughput
#   ./run.sh thread-sweep --datafile=cluster53
#   ./run.sh cap-sweep --datafile=ycsba
#   ./run.sh ycsba --threads=8          # shorthand for "single"
#   THREADS=64 ./run.sh throughput
#   HEMU=0 ./run.sh ycsba               # RocksDB-only
#   HEMU=1 ./run.sh ycsba               # Hemu+RocksDB only
#   HEMU=2 ./run.sh ycsba               # Hemu-only (no RocksDB)
# ════════════════════════════════════════════════════════════════════
set -o pipefail

# ── Paths ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$SCRIPT_DIR/rocksdb_bench"
RESULT_FILE="$SCRIPT_DIR/result"
POOL_PATH=/mnt/cxlpool/hemu_rocksdb_bench
DB_PATH="$SCRIPT_DIR/rocksdb_data"

# ── Common configuration ──────────────────────────────────────────
ESM=${ESM:-2}
CXL_NODE=${CXL_NODE:-1}
NUMA=${NUMA:-1}

# HEMU env var: unset → run all 3 scenarios; 0 → RocksDB only;
# 1 → Hemu+RocksDB; 2 → Hemu only (no RocksDB).
if [[ -n "${HEMU+x}" ]]; then
  HEMU_VALUES=("$HEMU")
else
  HEMU_VALUES=(0 1)
fi

# ── Twitter cluster trace → capacity mapping (bytes) ───────────────
# Same values as the main benchmark (scripts/run.sh SIZE_TABLE).
CLUSTERS=(45 53)
SIZE_TABLE=(
  # 400031744    # cluster3
  # 1291845632   # cluster10
  # 1763704832   # cluster22
  # 5317328896   # cluster24
  # 812646400    # cluster30
  1096810496   # cluster45
  4322230272   # cluster53
)

# ── YCSB workload → capacity mapping (bytes) ───────────────────────
YCSB_WORKLOADS=(a b c d e f)
YCSB_CAPACITYS=(2000000000 4000000000 4000000000 2000000000 4000000000 4000000000)

# ── Helpers ────────────────────────────────────────────────────────

# Drop OS page cache to ensure cold reads.
drop_caches() {
  sync
  echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

# Clear Hemu CXL pool and RocksDB data directory.
clear_data() {
  rm -rf "$POOL_PATH"/* 2>/dev/null || true
  rm -rf "$DB_PATH" 2>/dev/null || true
}

# Run the benchmark binary with numactl (if enabled).
run_bench() {
  mkdir -p "$POOL_PATH"
  if [[ "$NUMA" -eq 1 ]]; then
    numactl -N 0 "$BENCH" --result-path="$RESULT_FILE" "$@"
  else
    "$BENCH" --result-path="$RESULT_FILE" "$@"
  fi
}

# Print a section header.
header() {
  echo ""
  echo "╔══════════════════════════════════════════════════════════════╗"
  echo "║  $1"
  echo "╚══════════════════════════════════════════════════════════════╝"
}

# Print a timestamp.
timestamp() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')]"
}

# Parse CLI args: handles both --key=value and --key value forms.
# Sets the variables: datafile, threads, hemu_cap, start, end, esm,
# cxl_node, hemu_val, db_path.
# Remaining unknown args go into EXTRA_ARGS array (forwarded to benchmark).
parse_cli_args() {
  EXTRA_ARGS=()
  datafile=""
  threads=${THREADS:-1}
  hemu_cap=${HEMU_CAP:-2147483648}
  start=${START:-0.0}
  end=${END:-1.0}
  esm=$ESM
  cxl_node=$CXL_NODE
  hemu_val=""
  db_path="$DB_PATH"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --datafile=*)       datafile="${1#*=}"; shift ;;
      --datafile)         datafile="$2"; shift 2 ;;
      --threads=*)        threads="${1#*=}"; shift ;;
      --threads)          threads="$2"; shift 2 ;;
      --hemu-capacity=*)  hemu_cap="${1#*=}"; shift ;;
      --hemu-capacity)    hemu_cap="$2"; shift 2 ;;
      --start=*)         start="${1#*=}"; shift ;;
      --start)           start="$2"; shift 2 ;;
      --end=*)           end="${1#*=}"; shift ;;
      --end)             end="$2"; shift 2 ;;
      --esm=*)           esm="${1#*=}"; shift ;;
      --esm)             esm="$2"; shift 2 ;;
      --cxl-node=*)      cxl_node="${1#*=}"; shift ;;
      --cxl-node)        cxl_node="$2"; shift 2 ;;
      --hemu=*)          hemu_val="${1#*=}"; shift ;;
      --hemu)            hemu_val="$2"; shift 2 ;;
      --db-path=*)       db_path="${1#*=}"; shift ;;
      --db-path)         db_path="$2"; shift 2 ;;
      *)                 EXTRA_ARGS+=("$1"); shift ;;
    esac
  done
}

# Override HEMU_VALUES if --hemu was given on CLI.
apply_hemu_override() {
  if [[ -n "$hemu_val" ]]; then
    HEMU_VALUES=("$hemu_val")
  fi
}

# Run a single benchmark invocation with a specific hemu setting.
#   $1 = hemu value (0 or 1)
#   remaining args are forwarded to the benchmark binary.
run_hemu_scenario() {
  local hemu_val=$1; shift
  local label
  case "$hemu_val" in
    0) label="RocksDB-only" ;;
    1) label="Hemu + RocksDB" ;;
    2) label="Hemu-only" ;;
    *) label="hemu=$hemu_val" ;;
  esac

  echo "  ── $label (hemu=$hemu_val) ──"
  timestamp
  drop_caches
  clear_data
  # --result-path is already added inside run_bench(); just forward the rest.
  run_bench --hemu="$hemu_val" "$@"
}

# Automatically run one or more Hemu scenarios (RocksDB-only, Hemu+RocksDB,
# Hemu-only). Each scenario is a separate, isolated invocation with fresh state.
# Args (all positional):
#   $1 = datafile, $2 = threads, $3 = hemu_cap, $4 = start, $5 = end
#   remaining args are forwarded (--esm/--cxl-node/--db-path/...).
run_auto() {
  local datafile=$1
  local threads=$2
  local hemu_cap=$3
  local start=$4
  local end=$5
  shift 5

  for hv in "${HEMU_VALUES[@]}"; do
    run_hemu_scenario "$hv" \
      --datafile="$datafile" \
      --threads="$threads" \
      --hemu-capacity="$hemu_cap" \
      --start="$start" \
      --end="$end" \
      "$@"
  done
}

# ── Experiment: single ─────────────────────────────────────────────
#   Run one datafile with current settings.
exp_single() {
  parse_cli_args "$@"
  apply_hemu_override

  if [[ -z "$datafile" ]]; then
    echo "Error: --datafile=<name> is required for 'single' experiment"
    echo "  Twitter: cluster3, cluster10, cluster22, ..."
    echo "  YCSB:    ycsba, ycsbb, ycsbc, ..."
    exit 1
  fi

  header "Single: $datafile"
  timestamp
  echo "  threads:       $threads"
  echo "  hemu_capacity: $hemu_cap bytes"
  echo "  scenarios:     ${HEMU_VALUES[*]/#/hemu=}"
  echo "  esm:           $esm"
  echo "  cxl_node:      $cxl_node"
  echo "  start/end:     $start / $end"

  run_auto "$datafile" "$threads" "$hemu_cap" "$start" "$end" \
    --esm="$esm" --cxl-node="$cxl_node" --db-path="$db_path" \
    "${EXTRA_ARGS[@]}"
}

# ── Experiment: throughput ─────────────────────────────────────────
#   Run all Twitter cluster traces + YCSB workloads.
exp_throughput() {
  parse_cli_args "$@"
  apply_hemu_override

  header "Throughput: all Twitter + YCSB traces"
  timestamp
  echo "  scenarios: ${HEMU_VALUES[*]/#/hemu=}"
  echo "  threads:   $threads"

  # --- Twitter cluster traces ---
  for idx in "${!CLUSTERS[@]}"; do
    local d="${CLUSTERS[$idx]}"
    local cap="${SIZE_TABLE[$idx]}"

    echo ""
    echo "══ cluster$d  cap=${cap}B  threads=$threads ══"
    timestamp
    run_auto "cluster$d" "$threads" "$cap" "$start" "$end" \
      --esm="$esm" --cxl-node="$cxl_node" --db-path="$db_path" \
      "${EXTRA_ARGS[@]}"
  done

  # --- YCSB workloads ---
  for idx in "${!YCSB_WORKLOADS[@]}"; do
    local w="${YCSB_WORKLOADS[$idx]}"
    local cap="${YCSB_CAPACITYS[$idx]}"

    echo ""
    echo "══ ycsb$w  cap=${cap}B  threads=$threads ══"
    timestamp
    run_auto "ycsb$w" "$threads" "$cap" "$start" "$end" \
      --esm="$esm" --cxl-node="$cxl_node" --db-path="$db_path" \
      "${EXTRA_ARGS[@]}"
  done
}

# ── Experiment: thread-sweep ───────────────────────────────────────
#   Sweep thread count on a single datafile.
exp_thread_sweep() {
  parse_cli_args "$@"
  apply_hemu_override

  if [[ -z "$datafile" ]]; then
    echo "Error: --datafile=<name> is required for 'thread-sweep'"
    exit 1
  fi

  local thread_counts=(32)

  header "Thread sweep: $datafile  cap=${hemu_cap}B"
  timestamp
  echo "  scenarios: ${HEMU_VALUES[*]/#/hemu=}"

  for t in "${thread_counts[@]}"; do
    echo ""
    echo "══ threads=$t ══"
    timestamp
    run_auto "$datafile" "$t" "$hemu_cap" "$start" "$end" \
      --esm="$esm" --cxl-node="$cxl_node" --db-path="$db_path" \
      "${EXTRA_ARGS[@]}"
  done
}

# ── Experiment: cap-sweep ──────────────────────────────────────────
#   Sweep Hemu cache capacity on a single datafile.
exp_cap_sweep() {
  parse_cli_args "$@"
  apply_hemu_override

  if [[ -z "$datafile" ]]; then
    echo "Error: --datafile=<name> is required for 'cap-sweep'"
    exit 1
  fi

  local caps=(268435456 536870912 1073741824 2147483648 4294967296)

  header "Capacity sweep: $datafile  threads=$threads"
  timestamp
  echo "  scenarios: ${HEMU_VALUES[*]/#/hemu=}"

  for cap in "${caps[@]}"; do
    echo ""
    echo "══ hemu_cap=${cap}B ══"
    timestamp
    run_auto "$datafile" "$threads" "$cap" "$start" "$end" \
      --esm="$esm" --cxl-node="$cxl_node" --db-path="$db_path" \
      "${EXTRA_ARGS[@]}"
  done
}

# ── Usage ─────────────────────────────────────────────────────────
usage() {
  cat <<EOF
Usage: $0 <experiment|datafile> [extra args...]

Experiments:
  single        Run one datafile (requires --datafile=<name>)
  throughput    Run all Twitter cluster traces + YCSB workloads
  thread-sweep  Sweep thread count (1, 8, 16, 32, 64) on one datafile
  cap-sweep     Sweep Hemu cache capacity (256MB→4GB) on one datafile
  all           Alias for 'throughput'
  help          Show this help message

Shorthand:
  $0 ycsba              # equivalent to: $0 single --datafile=ycsba
  $0 cluster10          # equivalent to: $0 single --datafile=cluster10

Datafiles:
  Twitter: cluster3, cluster10, cluster22, cluster24,
           cluster30, cluster35, cluster45, cluster53
  YCSB:    ycsba, ycsbb, ycsbc, ycsbd, ycsbe, ycsbf

Environment variables:
  THREADS     Number of threads             (default: 1)
  HEMU_CAP    Hemu cache capacity in bytes   (default: 2147483648)
  ESM         eSM mode: 1=File/DAX, 2=NUMA   (default: 2)
  CXL_NODE    NUMA node for CXL memory       (default: 1)
  START       Trace start fraction 0..1      (default: 0.0)
  END         Trace end fraction 0..1        (default: 1.0)
  HEMU        0=RocksDB only, 1=Hemu+RocksDB, 2=Hemu only (default: unset → run all 3)
  NUMA        0=disable numactl, 1=enable     (default: 1)

CLI args (override env vars, support both --key=val and --key val):
  --datafile=<name>      --threads=<T>       --hemu-capacity=<bytes>
  --start=<0..1>         --end=<0..1>        --esm=<1|2>
  --cxl-node=<N>         --hemu=<0|1>        --db-path=<path>

By default each datafile is run THREE times (RocksDB-only, Hemu+RocksDB,
then Hemu-only) as separate isolated invocations.
Set HEMU=0, HEMU=1, or HEMU=2 to run just one.

Examples:
  $0 single --datafile=ycsba
  $0 single --datafile=cluster10 --threads=4
  $0 throughput
  $0 thread-sweep --datafile=cluster53
  $0 cap-sweep --datafile=ycsba
  $0 ycsba --threads=8
  $0 ycsba --threads 16              # space form also works
  THREADS=64 $0 throughput
  HEMU=0 $0 ycsba               # RocksDB-only
  HEMU=1 $0 ycsba               # Hemu+RocksDB only
  HEMU=2 $0 ycsba               # Hemu-only (no RocksDB)
  $0 ycsba --hemu=0 --threads 16    # CLI overrides env
EOF
}

# ── Main dispatch ─────────────────────────────────────────────────

# If no args, show usage.
if [[ $# -eq 0 ]]; then
  usage
  exit 1
fi

cmd="$1"; shift

# Shorthand: if cmd looks like a datafile name, treat it as "single".
if [[ "$cmd" == "cluster"* ]] || [[ "$cmd" == "ycsb"* ]]; then
  # Prepend --datafile= and dispatch to single.
  set -- "--datafile=$cmd" "$@"
  exp_single "$@"
  exit 0
fi

case "$cmd" in
  single)
    exp_single "$@"
    ;;
  throughput)
    exp_throughput "$@"
    ;;
  thread-sweep)
    exp_thread_sweep "$@"
    ;;
  cap-sweep)
    exp_cap_sweep "$@"
    ;;
  all)
    exp_throughput "$@"
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    echo "Error: unknown experiment '$cmd'"
    echo ""
    usage
    exit 1
    ;;
esac
