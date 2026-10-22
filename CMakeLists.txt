cmake_minimum_required(VERSION 3.20)
project(filesystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
find_library(LIBURING_LIBRARY NAMES liburing uring)
find_package(Threads REQUIRED)

if (NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
    message(FATAL_ERROR "liburing is required for the async I/O layer")
endif()

add_library(filesystem_core
    src/async_io.cpp
    src/block_device.cpp
    src/disk_format.cpp
    src/layout_engine.cpp
    src/bitmap.cpp
    src/buffer_pool.cpp
    src/crash_consistency.cpp
    src/extent.cpp
    src/core_worker_pool.cpp
)

target_include_directories(filesystem_core PUBLIC include ${LIBURING_INCLUDE_DIR})
target_link_libraries(filesystem_core PUBLIC ${LIBURING_LIBRARY} Threads::Threads)
target_compile_options(filesystem_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem main.cpp)
target_link_libraries(filesystem PRIVATE filesystem_core)
target_compile_options(filesystem PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-mkfs tools/mkfs.cpp)
target_link_libraries(filesystem-mkfs PRIVATE filesystem_core)
target_compile_options(filesystem-mkfs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(filesystem-dumpfs tools/dumpfs.cpp)
target_link_libraries(filesystem-dumpfs PRIVATE filesystem_core)
target_compile_options(filesystem-dumpfs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(async-io tests/async_io.cpp)
target_link_libraries(async-io PRIVATE filesystem_core)
target_compile_options(async-io PRIVATE -Wall -Wextra -Wpedantic)

add_executable(extent tests/extent.cpp)
target_link_libraries(extent PRIVATE filesystem_core)
target_compile_options(extent PRIVATE -Wall -Wextra -Wpedantic)

add_executable(buffer-pool tests/buffer_pool.cpp)
target_link_libraries(buffer-pool PRIVATE filesystem_core)
target_compile_options(buffer-pool PRIVATE -Wall -Wextra -Wpedantic)

add_executable(metadata tests/metadata.cpp)
target_link_libraries(metadata PRIVATE filesystem_core)
target_compile_options(metadata PRIVATE -Wall -Wextra -Wpedantic)

add_executable(layout tests/layout.cpp)
target_link_libraries(layout PRIVATE filesystem_core)
target_compile_options(layout PRIVATE -Wall -Wextra -Wpedantic)

add_executable(crash-consistency tests/crash_consistency.cpp)
target_link_libraries(crash-consistency PRIVATE filesystem_core)
target_compile_options(crash-consistency PRIVATE -Wall -Wextra -Wpedantic)

add_executable(crash-recovery tests/crash_recovery.cpp)
target_link_libraries(crash-recovery PRIVATE filesystem_core)
target_compile_options(crash-recovery PRIVATE -Wall -Wextra -Wpedantic)

add_executable(core-worker-pool tests/core_worker_pool.cpp)
target_link_libraries(core-worker-pool PRIVATE filesystem_core)
target_compile_options(core-worker-pool PRIVATE -Wall -Wextra -Wpedantic)

add_executable(block-device tests/block_device.cpp)
target_link_libraries(block-device PRIVATE filesystem_core)
target_compile_options(block-device PRIVATE -Wall -Wextra -Wpedantic)

add_executable(wal-recovery-edges tests/wal_recovery_edges.cpp)
target_link_libraries(wal-recovery-edges PRIVATE filesystem_core)
target_compile_options(wal-recovery-edges PRIVATE -Wall -Wextra -Wpedantic)

add_executable(database-workload tests/database_workload.cpp)
target_link_libraries(database-workload PRIVATE filesystem_core)
target_compile_options(database-workload PRIVATE -Wall -Wextra -Wpedantic)

add_executable(database-benchmark benchmarks/database_benchmark.cpp)
target_link_libraries(database-benchmark PRIVATE filesystem_core)
target_compile_options(database-benchmark PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if (BUILD_TESTING)
    add_test(NAME test-async-io COMMAND async-io)
    add_test(NAME test-extent COMMAND extent)
    add_test(NAME test-buffer-pool COMMAND buffer-pool)
    add_test(NAME test-metadata COMMAND metadata)
    add_test(NAME test-layout COMMAND layout)
    add_test(NAME test-crash-consistency COMMAND crash-consistency)
    add_test(NAME test-crash-recovery COMMAND crash-recovery)
    add_test(NAME test-core-worker-pool COMMAND core-worker-pool)
    add_test(NAME test-block-device COMMAND block-device)
    add_test(NAME test-wal-recovery-edges COMMAND wal-recovery-edges)
    add_test(NAME test-database-workload COMMAND database-workload)
    set_tests_properties(
        test-async-io
        test-extent
        test-buffer-pool
        test-metadata
        test-layout
        test-crash-consistency
        test-crash-recovery
        test-core-worker-pool
        test-block-device
        test-wal-recovery-edges
        test-database-workload
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    )
endif()
