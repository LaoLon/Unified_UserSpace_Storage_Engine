cmake_minimum_required(VERSION 3.16)
project(hfs LANGUAGES C)

set(SPDK_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/spdk)
set(HFS_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/config/hfs.json)

if(NOT EXISTS ${SPDK_ROOT_DIR}/CONFIG)
  message(FATAL_ERROR "SPDK is not configured. Follow README.md, then run 'make spdk'.")
endif()
if(NOT EXISTS ${SPDK_ROOT_DIR}/build/lib)
  message(FATAL_ERROR "SPDK is not built. Run 'make spdk' first.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# 与 spdk.common.mk 对齐的编译选项
add_compile_options(
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
  -Wmissing-declarations -fno-strict-aliasing -march=native -mno-avx512f
  -fno-lto -Wformat -Wformat-security -D_GNU_SOURCE -fPIC -fstack-protector
  -fno-common -Wno-pointer-sign -Wstrict-prototypes -Wold-style-definition
  -DNDEBUG -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
)

include_directories(
  ${SPDK_ROOT_DIR}/include
  ${SPDK_ROOT_DIR}/isa-l/..
  ${SPDK_ROOT_DIR}/isalbuild
  ${SPDK_ROOT_DIR}/isa-l-crypto/..
  ${SPDK_ROOT_DIR}/isalcryptobuild
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(HFS_CORE_SRC
  src/hfs.c
  src/hfs_mount.c
  src/hfs_file.c
  src/hfs_log.c
  src/hfs_lib.c
  src/hfs_meta.c
)

# SPDK 模块库列表（与 Makefile 的 ALL_MODULES_LIST + event + event_bdev 一致）
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
  list(APPEND SPDK_STATIC_LIBS ${SPDK_ROOT_DIR}/build/lib/libspdk_${lib}.a)
endforeach()

file(GLOB DPDK_STATIC_LIBS ${SPDK_ROOT_DIR}/dpdk/build/lib/librte_*.a)
set(ISAL_LIBS
  ${SPDK_ROOT_DIR}/isa-l/.libs/libisal.a
  ${SPDK_ROOT_DIR}/isa-l-crypto/.libs/libisal_crypto.a
)
set(HFS_SYS_LIBS numa dl rt uuid ssl crypto m fuse3 keyutils aio)

# 共享链接的公共部分：SPDK(whole-archive) + DPDK(whole-archive) + ISA-L + 系统库
# 注意：链接选项必须与库在同一 target_link_libraries 调用中，保证顺序
function(hfs_link_spdk target)
  target_link_libraries(${target} PRIVATE
    -Wl,--whole-archive -Wl,--no-as-needed
    ${SPDK_STATIC_LIBS}
    -Wl,--no-whole-archive
    -Wl,--whole-archive
    ${DPDK_STATIC_LIBS}
    -Wl,--no-whole-archive
    ${ISAL_LIBS}
    ${HFS_SYS_LIBS}
    Threads::Threads
  )
endfunction()

# libhfs.so：核心库（不含 main）
add_library(hfs SHARED ${HFS_CORE_SRC})
set_target_properties(hfs PROPERTIES OUTPUT_NAME "hfs" VERSION 0.1 SOVERSION 0)
target_compile_definitions(hfs PRIVATE "HFS_DEFAULT_CONFIG=\"${HFS_CONFIG}\"")
hfs_link_spdk(hfs)

# spdk-hfs：自测可执行（含 main）
add_executable(spdk-hfs ${HFS_CORE_SRC} src/hfs_main.c)
target_compile_definitions(spdk-hfs PRIVATE "HFS_DEFAULT_CONFIG=\"${HFS_CONFIG}\"")
hfs_link_spdk(spdk-hfs)

# run：检查 hugepages 后运行自测程序
add_custom_target(run
  COMMAND bash -c "if [ $$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0) = 0 ]; then echo 'No hugepages configured, allocating 1024 x 2MB...'; echo 1024 > /proc/sys/vm/nr_hugepages; fi; $<TARGET_FILE:spdk-hfs>"
  DEPENDS spdk-hfs
  USES_TERMINAL
)

# 把编译数据库复制到项目根目录（供 clangd 使用）
add_custom_target(compile-commands ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
  DEPENDS spdk-hfs hfs
  VERBATIM
)