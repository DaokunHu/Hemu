#!/bin/bash


KEY_TYPE=randint

for WORKLOAD_TYPE in b; do
  echo workload${WORKLOAD_TYPE} > workload_config.inp
  echo ${KEY_TYPE} >> workload_config.inp
  ./gen_workload workload_config.inp
done