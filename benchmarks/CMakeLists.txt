# Stratum 性能基准
#
# 运行：./benchmarks/stratum_bench [--quick]

add_executable(stratum_bench bench_main.cc)

target_link_libraries(stratum_bench PRIVATE
    stratum_recorder
    stratum_storage_lsm
    stratum_storage_memory
)
