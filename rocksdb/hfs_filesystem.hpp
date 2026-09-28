#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "hfs/export.hpp"
#include "rocksdb/file_system.h"
#include "rocksdb/env.h"

namespace hfs::rocksdb_adapter {

/* Creates the Shard-82 buffered RocksDB FileSystem backed by an HFS engine. */
HFS_API std::shared_ptr<rocksdb::FileSystem>
createFileSystem(const std::string &configPath);

/* Creates RocksDB's composite Env wrapper around the HFS FileSystem. */
HFS_API std::unique_ptr<rocksdb::Env> createCompositeEnv(const std::string &configPath);

/* Test-only injection: delays each asynchronous read by N milliseconds so the
 * timeout/abort contract can be exercised deterministically. The value is
 * cached in an atomic, so ReadAsync never calls getenv; tests switch it at
 * runtime and 0 disables it. HFS_TEST_ASYNC_READ_DELAY_MS remains the startup
 * default until the setter is used. */
HFS_API void setAsyncReadDelayMs(std::uint32_t milliseconds) noexcept;

} // namespace hfs::rocksdb_adapter
