cmake_minimum_required(VERSION 3.10)
project(app)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_BUILD_TYPE Debug)

set(SPDK_ROOT /home/jyshi/dev/next/spdk)
set(DPDK_ROOT /home/jyshi/dev/next/spdk/dpdk)

set(DPDKLIBS
    rte_bus_pci.so
    rte_bus_vdev.so
    rte_cmdline.so
    rte_dmadev.so
    rte_compressdev.so
    rte_cryptodev.so
    rte_eal.so
    rte_ethdev.so
    rte_hash.so
    rte_kvargs.so
    rte_mbuf.so
    rte_mempool_ring.so
    rte_mempool.so
    rte_meter.so
    rte_net.so
    rte_pci.so
    rte_power.so
    rte_rcu.so
    rte_reorder.so
    rte_ring.so
    rte_security.so
    rte_telemetry.so
    rte_timer.so
    rte_vhost.so
)

set(SPDKLIBS
    -Wl,--no-as-needed
    spdk
    spdk_env_dpdk
)


set(SYSLIBS
    -Wl,--as-needed
    pthread
    rt
    uuid
    crypto
    m
    aio
    numa
    dl
)


link_directories(${SPDK_ROOT}/build/lib/ ${DPDK_ROOT}/build/lib/)

add_executable(app app.cc)
target_include_directories(app PUBLIC ${SPDK_ROOT}/build/include/ ${DPDK_ROOT}/build/include/)

target_link_libraries(
    app
    ${SPDKLIBS}
    ${DPDKLIBS}
    ${SYSLIBS}
)

