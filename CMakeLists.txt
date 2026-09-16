cmake_minimum_required(VERSION 3.16)
project(hfs VERSION 2.0.0 LANGUAGES CXX)

# HFS 2.0 build: C++20 core skeleton (Shard-68). SPDK linkage and the runtime
# targets are added by later shards; src_legacy_frozen/ is never part of this
# build.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(HFS_CORE_SRC
  src/common/log.cpp
  src/common/stats.cpp
  src/common/trace.cpp
  src/config/parser.cpp
  src/core/engine.cpp
  src/core/client.cpp
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

add_executable(hfsctl tools/hfsctl.cpp)
target_link_libraries(hfsctl PRIVATE hfs_core)
target_compile_options(hfsctl PRIVATE -fno-exceptions -fno-rtti)

enable_testing()
set(HFS_TESTS config_parser_test engine_lifecycle_test task_test)
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
