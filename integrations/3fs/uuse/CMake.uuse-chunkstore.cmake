# ---- UUSE chunk store (M2.1 lab target) ----
set(UUSE_CORE_ROOT "/root/code-uuse" CACHE PATH "UUSE dev artifacts root (headers + libuuse_core.so)")
if(EXISTS "${UUSE_CORE_ROOT}/include/uuse/core.hpp" AND EXISTS "${UUSE_CORE_ROOT}/build-verify/libuuse_core.so")
  add_library(uuse_chunkstore STATIC src/storage/store/UuseChunkStore.cc)
  target_include_directories(uuse_chunkstore PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_BINARY_DIR}/src
      ${PROJECT_BINARY_DIR}
      PRIVATE ${UUSE_CORE_ROOT}/include)
  target_link_libraries(uuse_chunkstore PUBLIC common Threads::Threads
      "${UUSE_CORE_ROOT}/build-verify/libuuse_core.so")

  add_executable(uuse_chunkstore_smoke src/storage/store/UuseChunkStoreSmoke.cc)
  target_include_directories(uuse_chunkstore_smoke PRIVATE ${UUSE_CORE_ROOT}/include)
  target_link_libraries(uuse_chunkstore_smoke PRIVATE uuse_chunkstore)
  target_link_options(uuse_chunkstore_smoke PRIVATE
      "-Wl,--allow-shlib-undefined" "-Wl,-rpath,${UUSE_CORE_ROOT}/build-verify")
  set_target_properties(uuse_chunkstore_smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  message(STATUS "UUSE chunk store smoke enabled (${UUSE_CORE_ROOT})")
else()
  message(STATUS "UUSE chunk store smoke disabled (no artifacts at ${UUSE_CORE_ROOT})")
endif()
