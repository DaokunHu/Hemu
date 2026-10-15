#!/bin/bash
# ════════════════════════════════════════════════════════════════════
# Unified benchmark runner for Hemu experiments.
#
# Usage: ./run.sh <experiment>
#
# Experiments:
#   throughput    Multi-cache throughput comparison (Twitter traces + YCSB)
#   dram-sweep    DRAM percentage sweep for Hemu (0 → 1.0, step 0.05)
#   acr-sweep     ACR sliding-window size sweep (R2.O2)
#   page-sweep    Page size sweep (R2.O3 IV)
#   all           Run all experiments sequentially
#   help          Show this help message
#
# Run from scripts/.  eSM: 1=File/DAX (PMem), 2=NUMA (CXL).
# All workload traces are read from /data/:
#   Twitter:  /data/cluster<d>
#   YCSB:     /data/ycsb_run_workload<letter>
# ════════════════════════════════════════════════════════════════════

set -o pipefail

# ── Common configuration ───────────────────────────────────────────
BENCH="../benchmark/benchmark"
ESM=2
CXL_NODE=1
POOL_PATH=/mnt/cxlpool
# Optional Hemu tuning (positional Hemu.key=value tokens). Uncomment to apply.
HEMU_OPTS=(
    # Hemu.CacheStrategy=1
    # Hemu.PAGE_SIZE=1048576
    # Hemu.Balancer_ON=1
    # Hemu.Balancer_Interval=100
    # Hemu.Access_Granularity=64
    # Hemu.ACR_WINDOW_SIZE=64
    # Hemu.Thread0_DRAM_Page_Limit=32
    # Hemu.Ini_Hashtable_Capacity=16777216
)

# Twitter cluster trace → capacity mapping (index aligns with CLUSTERS)
CLUSTERS=(3 10 22 24 30 45 53)
SIZE_TABLE=(
'400031744'   #3
'1291845632'  #10
'1763704832'  #22
'5317328896' #24
'812646400'   #30
'1096810496'  #45
'4322230272' #53
)

# YCSB configuration
YCSB_WORKLOADS=(a b c d e f)
YCSB_CAPACITYS=(2000000000 4000000000 4000000000 2000000000 4000000000 4000000000)


# ── Helpers ────────────────────────────────────────────────────────
drop_caches() {
    sync && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

clear_pool() {
    rm -rf "$POOL_PATH"/* 2>/dev/null || true
}

# Print the average of cache_recycler_log.txt (single column of floats),
# then clear the file so each experiment case starts fresh.
summarize_cache_recycler() {
    local log=cache_recycler_log.txt
    if [[ -s "$log" ]]; then
        awk '{ s += $1; n++ }
             END {
                 printf "  cache_recycler avg: %.6f\n", s/n
                 printf "  samples: %d\n", n
             }' "$log"
    else
        echo "  cache_recycler_log: empty or missing"
    fi
    : > "$log"
}

# ── Experiment: throughput ─────────────────────────────────────────
#   Multi-cache throughput comparison on Twitter cluster traces + YCSB.
#   All baselines are expanded inline (like throughput.sh).
exp_throughput() {
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  Experiment: throughput (Twitter traces + YCSB)              ║"
    echo "╚══════════════════════════════════════════════════════════════╝"

    # --- Twitter cluster traces ---
    for idx in "${!CLUSTERS[@]}"; do
        local d="${CLUSTERS[$idx]}"
        local j="${SIZE_TABLE[$idx]}"
        drop_caches
        for t in 64; do
            clear_pool

            echo "Redis"
            numactl -N 0 "$BENCH" --cache Redis --datafile "cluster$d" \
                --threads $t --capacity $j

            echo "PaperCache"
            numactl -N 0 "$BENCH" --cache PaperCache --datafile "cluster$d" \
                --threads $t --capacity $j

            # Memcached requires starting/stopping the daemon
            if ! service memcached stop 2>/dev/null; then
                echo "Warning: Failed to stop memcached service, trying to kill process"
                pkill -f "memcached -p 11211" 2>/dev/null || true
            fi
            mem_mb=$(( j / 1000000 ))
            numactl -m 1 memcached -p 11211 -m $mem_mb -d -u root
            echo "Memcached"
            numactl -N 0 "$BENCH" --cache Memcached --datafile "cluster$d" \
                --threads $t --capacity $j
            pkill -f "memcached -p 11211" 2>/dev/null || true

            clear_pool
            echo "Hemu"
            numactl -N 0 "$BENCH" --cache Hemu --datafile "cluster$d" \
                --threads $t --capacity $j \
                --esm $ESM --cxl-node $CXL_NODE Hemu.Balancer_ON=0 \
                ${HEMU_OPTS[@]}
            
            echo "Hemu"
            numactl -N 0 "$BENCH" --cache Hemu --datafile "cluster$d" \
                --threads $t --capacity $j \
                --esm $ESM --cxl-node $CXL_NODE Hemu.Balancer_ON=1 \
                ${HEMU_OPTS[@]}



            clear_pool
            echo "Cachelib"
            numactl -N 0 "$BENCH" --cache Cachelib --datafile "cluster$d" \
                --threads $t --capacity $j

            clear_pool
            echo "Segcache"
            numactl -N 0 "$BENCH" --cache Segcache --datafile "cluster$d" \
                --threads $t --capacity $j \
                --config ../benchmark/Segcache_bench/Segcache.conf

            clear_pool
            echo "Viper"
            numactl -N 0 "$BENCH" --cache Viper --datafile "cluster$d" \
                --threads $t --capacity  $j

            clear_pool
            echo "Plush"
            numactl -N 0 "$BENCH" --cache Plush --datafile "cluster$d" \
                --threads $t --capacity $j
        done
    done

    # --- YCSB workloads ---
    for idx in "${!YCSB_WORKLOADS[@]}"; do
        local w="${YCSB_WORKLOADS[$idx]}"
        local capacity="${YCSB_CAPACITYS[$idx]}"
        for t in 64; do
            drop_caches
            clear_pool

            echo "Redis  ycsb$w"
            numactl -N 0 "$BENCH" --cache Redis --datafile "ycsb$w" \
                --threads $t --capacity $capacity

            echo "PaperCache  ycsb$w"
            numactl -N 0 "$BENCH" --cache PaperCache --datafile "ycsb$w" \
                --threads $t --capacity $capacity

            Memcached requires starting/stopping the daemon
            if ! service memcached stop 2>/dev/null; then
                echo "Warning: Failed to stop memcached service, trying to kill process"
                systemctl stop memcached
            fi
            mem_mb=$(( capacity / 1000000 ))
            numactl -m 1 memcached -p 11211 -m $mem_mb -d -u root
            echo "Memcached  ycsb$w"
            numactl -N 0 "$BENCH" --cache Memcached --datafile "ycsb$w" \
                --threads $t --capacity $capacity
            systemctl stop memcached

            clear_pool
            echo "Hemu  ycsb$w"
            numactl -N 0 "$BENCH" --cache Hemu --datafile "ycsb$w" \
                --threads $t --capacity $capacity \
                --esm $ESM --cxl-node $CXL_NODE \
                ${HEMU_OPTS[@]} Hemu.Balancer_ON=0

            echo "Hemu  ycsb$w"
            numactl -N 0 "$BENCH" --cache Hemu --datafile "ycsb$w" \
                --threads $t --capacity $capacity \
                --esm $ESM --cxl-node $CXL_NODE \
                ${HEMU_OPTS[@]} Hemu.Balancer_ON=1



            clear_pool
            echo "Cachelib  ycsb$w"
            numactl -N 0 "$BENCH" --cache Cachelib --datafile "ycsb$w" \
                --threads $t --capacity $capacity
            
            clear_pool
            echo "Segcache  ycsb$w"
            numactl -N 0 "$BENCH" --cache Segcache --datafile "ycsb$w" \
                --threads $t --capacity $capacity \
                --config ../benchmark/Segcache_bench/Segcache.conf
            
            clear_pool
            echo "Viper  ycsb$w"
            numactl -N 0 "$BENCH" --cache Viper --datafile "ycsb$w" \
                --threads $t --capacity $capacity --end 0.5

            clear_pool
            echo "Plush  ycsb$w"
            numactl -N 0 "$BENCH" --cache Plush --datafile "ycsb$w" \
                --threads $t --capacity $capacity 
        done
    done
}

# ── Experiment: dram-sweep ────────────────────────────────────────
#   Sweeps DRAM_PERCENT from 0 to 1.0 (step 0.05) for Hemu across all
#   Twitter cluster traces, YCSB workloads, and thread counts. Each config is run 3 times
#   for stability.
exp_dram_sweep() {
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  Experiment: DRAM percentage sweep (Hemu)                    ║"
    echo "╚══════════════════════════════════════════════════════════════╝"

    for idx in "${!CLUSTERS[@]}"; do
        local d="${CLUSTERS[$idx]}"
        local j="${SIZE_TABLE[$idx]}"
        drop_caches

        for t in 64; do
            echo " "
            echo " "
            echo $(date +%Y-%m-%d" "%H:%M:%S)
            echo "Threads $t Data $d Size $j"

            for p in 0  0.1  0.20  0.30  0.40  0.50 0.60 0.70 0.80; do
                echo "--------------------"
                echo "$p"
                clear_pool

                numactl -N 0 "$BENCH" --cache Hemu --datafile "cluster$d" \
                    --threads $t --capacity $j \
                    --esm $ESM --cxl-node $CXL_NODE \
                    ${HEMU_OPTS[@]} Hemu.DRAM_PERCENT=$p Hemu.Balancer_ON=0

            done
        done
    done

    for idx in "${!YCSB_WORKLOADS[@]}"; do
        local w="${YCSB_WORKLOADS[$idx]}"
        local capacity="${YCSB_CAPACITYS[$idx]}"

        for t in 64; do
            echo " "
            echo " "
            echo $(date +%Y-%m-%d" "%H:%M:%S)
            echo "Threads $t Data ycsb$w Size $capacity"

            for p in 0  0.1  0.20  0.30  0.40  0.50 0.60 0.70 0.80; do
                echo "--------------------"
                echo "$p"
                drop_caches
                clear_pool


                numactl -N 0 "$BENCH" --cache Hemu --datafile "ycsb$w" \
                    --threads $t --capacity $capacity \
                    --esm $ESM --cxl-node $CXL_NODE \
                    ${HEMU_OPTS[@]} Hemu.DRAM_PERCENT=$p Hemu.Balancer_ON=0

            done
        done
    done
}

# ── Experiment: acr-sweep ─────────────────────────────────
#   ACR sliding-window size n sweep.
#   Default n=64. Smaller n → noisier density estimate; larger n → slower
#   adaptation. Sweep and record miss ratio / throughput.
#   Uses representative workload cluster24 with matched capacity.
exp_acr_sweep() {
    local data=cluster53
    local capacity=4322230272

    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  Experiment: ACR_WINDOW_SIZE sweep                           ║"
    echo "║  Data=$data  Cap=$capacity"
    echo "╚══════════════════════════════════════════════════════════════╝"

    for t in 1; do
        for n in 1 2 4 16 32 64 128 256 1024 4096 8192; do
            clear_pool
            drop_caches
            echo "=== ACR_WINDOW_SIZE=$n  threads=$t ==="
            numactl -N 0 "$BENCH" --cache Hemu --datafile "$data" \
                --threads $t --capacity $capacity \
                --esm $ESM --cxl-node $CXL_NODE \
                ${HEMU_OPTS[@]} Hemu.ACR_WINDOW_SIZE=$n
            echo "--- cache_recycler summary for ACR_WINDOW_SIZE=$n ---"
            summarize_cache_recycler
        done
    done
}

# ── Experiment: page-sweep      ─────────────────────────────
#   Page size sweep.
#   Paper uses 4 MB (4194304). Sweep to justify the choice and measure its
#   effect on throughput, miss ratio, and fragmentation.
#   Uses representative workload cluster24 with matched capacity.
exp_page_sweep() {
    local data=cluster53
    local capacity=4322230272

    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  Experiment: PAGE_SIZE sweep                                 ║"
    echo "║  Data=$data  Cap=$capacity"
    echo "╚══════════════════════════════════════════════════════════════╝"

    for t in 1 8 16 32 48 64; do
        for ps in 1048576 2097152 4194304 8388608; do
            clear_pool
            drop_caches
            echo "=== PAGE_SIZE=$ps  threads=$t ==="
            numactl -N 0 "$BENCH" --cache Hemu --datafile "$data" \
                --threads $t --capacity $capacity \
                --esm $ESM --cxl-node $CXL_NODE 
                ${HEMU_OPTS[@]} Hemu.PAGE_SIZE=$ps
        done
    done
}

# ── Usage ─────────────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $0 <experiment>

Experiments:
  throughput    Multi-cache throughput comparison (Twitter + YCSB)
  dram-sweep    DRAM percentage sweep for Hemu (0 → 1.0, step 0.05)
  acr-sweep     ACR sliding-window size sweep
  page-sweep    Page size sweep
  all           Run all experiments sequentially
  help          Show this help message

Configuration (edit top of script):
  BENCH, ESM, CXL_NODE, POOL_PATH, HEMU_OPTS, CLUSTERS, SIZE_TABLE
  YCSB_WORKLOADS, YCSB_CAPACITYS

Examples:
  $0 throughput
  $0 dram-sweep
  $0 acr-sweep
  $0 page-sweep
  $0 all
EOF
}

# ── Main dispatch ─────────────────────────────────────────────────
if [[ $# -eq 0 ]]; then
    usage
    exit 1
fi

cmd="$1"; shift

case "$cmd" in
    throughput)
        exp_throughput
        ;;
    dram-sweep)
        exp_dram_sweep
        ;;
    acr-sweep)
        exp_acr_sweep
        ;;
    page-sweep)
        exp_page_sweep
        ;;
    all)
        exp_throughput
        exp_dram_sweep
        exp_acr_sweep
        exp_page_sweep
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        echo "Error: unknown experiment '$cmd'"
        echo
        usage
        exit 1
        ;;
esac
