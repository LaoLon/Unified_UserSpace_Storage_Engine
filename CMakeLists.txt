cmake_minimum_required(VERSION 3.16)
project(hfs VERSION 2.0.0 LANGUAGES CXX)

# HFS 2.0 build: C++20 core. SPDK is required for the runtime layer; without
# it the core still builds and the runtime returns NotSupported.
# src_legacy_frozen/ is never part of this build.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)

set(HFS_SPDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/spdk CACHE PATH "SPDK source and build root")
if(EXISTS ${HFS_SPDK_ROOT}/include/spdk/init.h AND EXISTS ${HFS_SPDK_ROOT}/build/lib)
  set(HFS_WITH_SPDK ON)
else()
  set(HFS_WITH_SPDK OFF)
  message(WARNING "SPDK not found at ${HFS_SPDK_ROOT}; building without SPDK runtime support")
endif()

set(HFS_CORE_SRC
  src/common/crc.cpp
  src/common/log.cpp
  src/common/stats.cpp
  src/common/trace.cpp
  src/config/parser.cpp
  src/core/actor.cpp
  src/core/client.cpp
  src/core/data_path.cpp
  src/core/data_plane.cpp
  src/core/dma_arena.cpp
  src/core/engine.cpp
  src/core/extent_map.cpp
  src/core/frame_pool.cpp
  src/core/io_planner.cpp
  src/core/operation.cpp
  src/core/parent_completion.cpp
  src/core/scheduler.cpp
  src/core/shard_ring.cpp
  src/metadata/allocator.cpp
  src/metadata/group_commit.cpp
  src/metadata/inode_map.cpp
  src/metadata/namespace.cpp
  src/runtime/spdk_runtime.cpp
  src/storage/journal.cpp
  src/storage/superblock.cpp
  src/storage/volume.cpp
)

add_library(hfs_core SHARED ${HFS_CORE_SRC})
add_library(hfs::core ALIAS hfs_core)
set_target_properties(hfs_core PROPERTIES
  OUTPUT_NAME hfs_core
  VERSION ${PROJECT_VERSION}
  SOVERSION 2
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
target_compile_definitions(hfs_core PRIVATE HFS_CORE_ABI_MAJOR=2)
target_compile_options(hfs_core PRIVATE -fno-exceptions -fno-rtti)
target_include_directories(hfs_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(hfs_core PRIVATE Threads::Threads)

set(FIO_SOURCE_DIR "" CACHE PATH "fio source tree for the optional HFS external engine")
if(EXISTS ${FIO_SOURCE_DIR}/fio.h)
  add_library(fio-hfs MODULE fio/fio_hfs.cpp)
  target_include_directories(fio-hfs PRIVATE ${FIO_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(fio-hfs PRIVATE -include ${FIO_SOURCE_DIR}/config-host.h)
  target_compile_options(fio-hfs PRIVATE -O0 -g)
  target_link_libraries(fio-hfs PRIVATE hfs_core)
  set_target_properties(fio-hfs PROPERTIES PREFIX "" OUTPUT_NAME "fio-hfs")
endif()

set(ROCKSDB_ROOT "" CACHE PATH "RocksDB 8.9.1 installation root for the optional adapter")
if(EXISTS ${ROCKSDB_ROOT}/include/rocksdb/file_system.h)
  find_library(ROCKSDB_LIBRARY NAMES rocksdb
    PATHS ${ROCKSDB_ROOT}/lib ${ROCKSDB_ROOT}/lib/x86_64-linux-gnu
          /usr/lib/x86_64-linux-gnu)
  if(NOT ROCKSDB_LIBRARY)
    message(FATAL_ERROR "RocksDB library not found below ${ROCKSDB_ROOT}")
  endif()
  add_library(hfs_rocksdb SHARED rocksdb/hfs_filesystem.cpp)
  target_include_directories(hfs_rocksdb PRIVATE
    ${ROCKSDB_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(hfs_rocksdb PRIVATE hfs_core
    ${ROCKSDB_LIBRARY})
  set_target_properties(hfs_rocksdb PROPERTIES OUTPUT_NAME hfs_rocksdb)
  add_executable(hfs_rocksdb_smoke rocksdb/hfs_filesystem_smoke.cpp)
  target_include_directories(hfs_rocksdb_smoke PRIVATE
    ${ROCKSDB_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(hfs_rocksdb_smoke PRIVATE hfs_rocksdb ${ROCKSDB_LIBRARY})
  add_executable(hfs_db_bench rocksdb/hfs_db_bench.cpp)
  target_include_directories(hfs_db_bench PRIVATE
    ${ROCKSDB_ROOT}/include ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(hfs_db_bench PRIVATE hfs_rocksdb ${ROCKSDB_LIBRARY})
endif()

if(HFS_WITH_SPDK)
  # Mirror of the legacy SPDK static link set; bdev/nvme/env/thread/init are
  # the modules the runtime uses, the rest keep subsystem registration intact.
  set(SPDK_LIB_LIST
    bdev_malloc bdev_null bdev_nvme bdev_passthru bdev_lvol
    bdev_raid bdev_error bdev_gpt bdev_split bdev_delay
    bdev_zone_block blob_bdev lvol blob nvme
    bdev_aio bdev_ftl ftl bdev_virtio virtio vfio_user
    accel_error accel_ioat ioat accel_ae4dma ae4dma
    scheduler_dynamic env_dpdk scheduler_dpdk_governor scheduler_gscheduler
    sock_posix keyring_file keyring_linux fsdev_aio fsdev
    event event_bdev bdev notify event_accel accel dma
    event_sock sock event_iobuf event_keyring keyring event_vmd
    init vmd thread trace rpc jsonrpc json util log
  )
  set(SPDK_STATIC_LIBS "")
  foreach(lib IN LISTS SPDK_LIB_LIST)
    list(APPEND SPDK_STATIC_LIBS ${HFS_SPDK_ROOT}/build/lib/libspdk_${lib}.a)
  endforeach()
  file(GLOB DPDK_STATIC_LIBS ${HFS_SPDK_ROOT}/dpdk/build/lib/librte_*.a)
  set(ISAL_LIBS
    ${HFS_SPDK_ROOT}/isa-l/.libs/libisal.a
    ${HFS_SPDK_ROOT}/isa-l-crypto/.libs/libisal_crypto.a
  )
  set(HFS_SYS_LIBS numa dl rt uuid ssl crypto m lz4 fuse3 keyutils aio)

  # Attach the SPDK runtime closure to a target (core library and the device
  # data-path test both need it).
  function(hfs_link_spdk target)
    target_compile_definitions(${target} PRIVATE HFS_WITH_SPDK=1)
    target_include_directories(${target} PRIVATE ${HFS_SPDK_ROOT}/include)
    target_link_libraries(${target} PRIVATE
      -Wl,--whole-archive -Wl,--no-as-needed
      ${SPDK_STATIC_LIBS}
      -Wl,--no-whole-archive
      -Wl,--whole-archive
      ${DPDK_STATIC_LIBS}
      -Wl,--no-whole-archive
      ${ISAL_LIBS}
      ${HFS_SYS_LIBS}
    )
  endfunction()

  hfs_link_spdk(hfs_core)
endif()

add_executable(hfsctl tools/hfsctl.cpp)
target_link_libraries(hfsctl PRIVATE hfs_core)
target_compile_options(hfsctl PRIVATE -fno-exceptions -fno-rtti)

enable_testing()
set(HFS_TESTS config_parser_test task_test crc_test geometry_test journal_test)
if(HFS_WITH_SPDK)
  list(APPEND HFS_TESTS engine_lifecycle_test spdk_runtime_test format_mount_test
       journal_device_test file_api_test file_data_test)
endif()
foreach(test_name IN LISTS HFS_TESTS)
  add_executable(${test_name} tests/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE hfs_core)
  target_compile_options(${test_name} PRIVATE -fno-exceptions -fno-rtti)
  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_definitions(${test_name} PRIVATE
    HFS_DEFAULT_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/hfs_nvme.conf")
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The data-plane structure test compiles the internal core sources directly so
# it can run with ASAN/TSAN and without SPDK.
add_executable(data_plane_test
  tests/data_plane_test.cpp
  src/common/log.cpp
  src/common/stats.cpp
  src/common/trace.cpp
  src/core/actor.cpp
  src/core/frame_pool.cpp
  src/core/operation.cpp
  src/core/shard_ring.cpp
)
target_include_directories(data_plane_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_options(data_plane_test PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(data_plane_test PRIVATE Threads::Threads)
add_test(NAME data_plane_test COMMAND data_plane_test)

# Inode dependency scheduler structure test (same internal-source approach).
add_executable(scheduler_test
  tests/scheduler_test.cpp
  src/common/log.cpp
  src/common/stats.cpp
  src/common/trace.cpp
  src/core/scheduler.cpp
)
target_include_directories(scheduler_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_options(scheduler_test PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(scheduler_test PRIVATE Threads::Threads)
add_test(NAME scheduler_test COMMAND scheduler_test)

# Extent planner / DMA arena / parent completion structure test.
add_executable(io_planner_test
  tests/io_planner_test.cpp
  src/common/log.cpp
  src/common/stats.cpp
  src/common/trace.cpp
  src/core/dma_arena.cpp
  src/core/extent_map.cpp
  src/core/io_planner.cpp
  src/core/parent_completion.cpp
)
target_include_directories(io_planner_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_options(io_planner_test PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(io_planner_test PRIVATE Threads::Threads)
add_test(NAME io_planner_test COMMAND io_planner_test)

# Device-backed data-path test: real NVMe read/write through the async data
# reactor path (compiles the core sources directly and links SPDK).
if(HFS_WITH_SPDK)
  add_executable(data_path_device_test tests/data_path_device_test.cpp ${HFS_CORE_SRC})
  target_include_directories(data_path_device_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_compile_options(data_path_device_test PRIVATE -fno-exceptions -fno-rtti)
  target_link_libraries(data_path_device_test PRIVATE Threads::Threads)
  hfs_link_spdk(data_path_device_test)
  add_test(NAME data_path_device_test COMMAND data_path_device_test)
endif()

# Namespace/inode/directory-index structure test (internal sources, no SPDK).
add_executable(namespace_test
  tests/namespace_test.cpp
  src/common/crc.cpp
  src/metadata/allocator.cpp
  src/metadata/inode_map.cpp
  src/metadata/namespace.cpp
)
target_include_directories(namespace_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_options(namespace_test PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(namespace_test PRIVATE Threads::Threads)
add_test(NAME namespace_test COMMAND namespace_test)

# Extent allocator and per-inode extent map structure test (no SPDK).
add_executable(allocator_test
  tests/allocator_test.cpp
  src/common/crc.cpp
  src/metadata/allocator.cpp
  src/metadata/inode_map.cpp
  src/metadata/namespace.cpp
)
target_include_directories(allocator_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
target_compile_options(allocator_test PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(allocator_test PRIVATE Threads::Threads)
add_test(NAME allocator_test COMMAND allocator_test)

# Keep clangd's compilation database at the repository root.
add_custom_target(compile-commands ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
  DEPENDS hfs_core hfsctl
  VERBATIM
)
