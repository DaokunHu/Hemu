#!/bin/bash
# 冷启动 vs 热启动对比实验
# 用法: ./cold_start.sh [cluster|ycsb|all]
# 结果保存在 benchmark/ 目录下

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/../benchmark"

run() {
    local data=$1 cap=$2

    echo "===== $data (cap=$cap) ====="

    # 1. 冷启动：直接跑后半段
    rm -rf /mnt/cxlpool/Hemu/*
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.0 --start 0.5 \
        > 冷启动_$data.txt

    # 2. 纯热：先跑前半段填充，再跑后半段
    rm -rf /mnt/cxlpool/Hemu/*
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.0 --end 0.5
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.0 --start 0.5 \
        > 热启动_0_$data.txt

    # 3. 热启动(DRAM 0.5填充)：先以DRAM=0.5跑前半段，再以DRAM=0.5跑后半段
    rm -rf /mnt/cxlpool/Hemu/*
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.5 --end 0.5
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.5 --start 0.5 \
        > 热启动_0.5_$data.txt

    # 4. 热启动(DRAM 0.2填充)：先以DRAM=0.2跑前半段，再以DRAM=0.2跑后半段
    rm -rf /mnt/cxlpool/Hemu/*
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.2 --end 0.5
    numactl -N 0 ./benchmark --cache Hemu --datafile $data --threads 1 \
        --capacity $cap --esm 1 --cxl-node 1 \
        Hemu.Balancer_ON=0 Hemu.DRAM_PERCENT=0.2 --start 0.5 \
        > 热启动_0.2_$data.txt

    echo "  done: 冷启动_$data.txt  热启动_0_$data.txt  热启动_0.5_$data.txt  热启动_0.2_$data.txt"
}

case "${1:-all}" in
    cluster)
        run cluster3  400031744
        run cluster10 1291845632
        run cluster22 1763704832
        run cluster24 5317328896
        run cluster30 812646400
        run cluster45 1096810496
        run cluster53 4322230272
        ;;
    ycsb)
        run ycsba 2000000000
        run ycsbb 4000000000
        run ycsbc 4000000000
        run ycsbd 2000000000
        run ycsbe 4000000000
        run ycsbf 4000000000
        ;;
    all)
        "$SCRIPT_DIR/cold_start.sh" cluster
        "$SCRIPT_DIR/cold_start.sh" ycsb
        ;;
    *)
        echo "Usage: $0 [cluster|ycsb|all]"
        exit 1
        ;;
esac
