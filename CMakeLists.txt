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
  src/common/log.cpp
  src/common/stats.cpp
  src/common/trace.cpp
  src/config/parser.cpp
  src/core/engine.cpp
  src/core/client.cpp
  src/runtime/spdk_runtime.cpp
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

if(HFS_WITH_SPDK)
  target_compile_definitions(hfs_core PRIVATE HFS_WITH_SPDK=1)
  target_include_directories(hfs_core PRIVATE ${HFS_SPDK_ROOT}/include)

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

  target_link_libraries(hfs_core PRIVATE
    -Wl,--whole-archive -Wl,--no-as-needed
    ${SPDK_STATIC_LIBS}
    -Wl,--no-whole-archive
    -Wl,--whole-archive
    ${DPDK_STATIC_LIBS}
    -Wl,--no-whole-archive
    ${ISAL_LIBS}
    ${HFS_SYS_LIBS}
  )
endif()

add_executable(hfsctl tools/hfsctl.cpp)
target_link_libraries(hfsctl PRIVATE hfs_core)
target_compile_options(hfsctl PRIVATE -fno-exceptions -fno-rtti)

enable_testing()
set(HFS_TESTS config_parser_test task_test)
if(HFS_WITH_SPDK)
  list(APPEND HFS_TESTS engine_lifecycle_test spdk_runtime_test)
endif()
foreach(test_name IN LISTS HFS_TESTS)
  add_executable(${test_name} tests/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE hfs_core)
  target_compile_options(${test_name} PRIVATE -fno-exceptions -fno-rtti)
  target_include_directories(${test_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(${test_name} PRIVATE
    HFS_DEFAULT_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config/hfs_nvme.conf")
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Keep clangd's compilation database at the repository root.
add_custom_target(compile-commands ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
  DEPENDS hfs_core hfsctl
  VERBATIM
)
